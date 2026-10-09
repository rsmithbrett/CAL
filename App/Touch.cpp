#include "Touch.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "Display.h"
#include "Log.h"

namespace Touch {
namespace {

constexpr uint8_t kMaxZones = 4;
Rect gZones[kMaxZones];
uint8_t gZoneCount = 0;

/// Roughly 1/3 of the 320px panel width per side, full height, no visible
/// chrome. Widened from the original 16px (1/20, matching CYD-Dickey's own
/// `x < 16` / `x > 304`) after real-device feedback that a strip 5% of the
/// screen wide was too easy to miss with a thumb and made edge navigation
/// feel unresponsive - the same complaint the approved backlog suggestion
/// "Increase the size of the area on the touchscreen you use to page to the
/// left and right" describes. A wider strip can only ever compete with card
/// content in the sense of covering more of it with an invisible zone, never
/// with an actual button: poll() below checks action-button zones first
/// regardless of how much they overlap the edge strips, so widening this can
/// dim more of the reachable card content but can never swallow a button
/// press. Leaves an inner ~108px strip (320 - 2*106) for whatever a card
/// draws in the middle.
constexpr int32_t kEdgeZoneWidth = 106;
constexpr int32_t kScreenWidth = 320;

/// What the sampling task hands over: where the finger landed and when, with
/// no interpretation attached.
struct Sample {
  int32_t x;
  int32_t y;
  uint32_t atMs;
};

/// Four is enough to hold a repeated tap through the worst stall the fleet has
/// logged (13.9 seconds) without being large enough for a backlog to build up
/// unnoticed. A fifth tap in one stall is the same situation as the fourth and
/// is counted rather than kept.
constexpr UBaseType_t kQueueDepth = 4;

/// Above the Arduino loop task, which runs at 1, so a sample is taken even
/// while loop() is in the middle of a card draw that never yields.
constexpr UBaseType_t kTaskPriority = 2;

/// getTouchRaw() uses 57 bytes of transfer buffer, three 7-entry arrays and a
/// std::sort. 4KB is well clear of that and of a Log call the task does not
/// make.
constexpr uint32_t kTaskStackBytes = 4096;

/// Fast enough that a deliberate tap cannot fall between two samples, slow
/// enough to leave the core alone. A finger is on the glass for 80ms at the
/// very least.
constexpr uint32_t kSampleIntervalMs = 10;

/// The pacing loop in App.ino drains this queue every 5ms, so a tap taken
/// while loop() is idle is delivered almost at once. Anything older than this
/// waited through something slow, and delivering it then would advance a card
/// with nobody touching the screen.
constexpr uint32_t kStaleAfterMs = 1500;

/// Core 0 rather than core 1. Card draws are the longest CPU-bound stretch on
/// this device and they run on core 1 with the Arduino loop; sampling from
/// core 0 means a draw and a sample never wait for each other at all, where
/// sharing a core would rely on preemption every time.
constexpr BaseType_t kTaskCore = 0;

QueueHandle_t gQueue = nullptr;

/// The two the sampling task keeps. They only ever count up, and only the task
/// writes them, so loop()'s side reports a difference against its own snapshot
/// instead of resetting them. A shared counter that both threads modified would
/// be a read-modify-write race across two cores; this way a 32-bit aligned read
/// is all the other thread needs, and the worst case is a report one sample out
/// of date rather than a lost or torn count. A mutex here would let loop()'s
/// thread block the sampler, which is the one thing this design exists to stop.
volatile uint32_t gSampled = 0;
volatile uint32_t gDroppedQueueFull = 0;

/// What the last report had already counted, so the next one covers one
/// interval.
uint32_t gSampledReported = 0;
uint32_t gDroppedQueueFullReported = 0;

/// Loop()'s own, touched by nothing else.
uint32_t gDelivered = 0;
uint32_t gDroppedStale = 0;
uint32_t gCollapsed = 0;

bool contains(const Rect& rect, int32_t x, int32_t y) {
  return x >= rect.x && x <= rect.x + rect.w && y >= rect.y && y <= rect.y + rect.h;
}

void sampleTask(void*) {
  bool wasTouched = false;

  for (;;) {
    int32_t x = 0;
    int32_t y = 0;
    const bool isTouched = Display::readTouchRaw(x, y);

    if (isTouched && !wasTouched) {
      ++gSampled;
      const Sample sample{x, y, millis()};
      if (xQueueSend(gQueue, &sample, 0) != pdTRUE) {
        ++gDroppedQueueFull;
      }
    }

    wasTouched = isTouched;
    vTaskDelay(pdMS_TO_TICKS(kSampleIntervalMs));
  }
}

}  // namespace

void begin() {
  gQueue = xQueueCreate(kQueueDepth, sizeof(Sample));
  if (gQueue == nullptr) {
    Log::line("[touch] could not create the tap queue - taps will not be seen");
    return;
  }

  const BaseType_t created = xTaskCreatePinnedToCore(
      sampleTask, "touch", kTaskStackBytes, nullptr, kTaskPriority, nullptr, kTaskCore);

  if (created != pdPASS) {
    vQueueDelete(gQueue);
    gQueue = nullptr;
    Log::line("[touch] could not start the sampling task - taps will not be seen");
    return;
  }

  Log::printf("[touch] sampling every %lu ms on core %d at priority %u, queue depth %u",
              static_cast<unsigned long>(kSampleIntervalMs), static_cast<int>(kTaskCore),
              static_cast<unsigned>(kTaskPriority), static_cast<unsigned>(kQueueDepth));
}

void setActionZones(const Rect* zones, uint8_t count) {
  gZoneCount = 0;
  if (zones == nullptr) {
    return;
  }
  for (uint8_t i = 0; i < count && i < kMaxZones; ++i) {
    gZones[gZoneCount++] = zones[i];
  }
}

bool poll(Tap& tap) {
  if (gQueue == nullptr) {
    return false;
  }

  Sample sample;
  uint32_t age = 0;

  // Walk forward through whatever is waiting until something fresh turns up.
  // A stale entry is counted and discarded rather than delivered, so a tap
  // that waited out a multi-second fetch does not move the rotation after the
  // finger has gone.
  for (;;) {
    if (xQueueReceive(gQueue, &sample, 0) != pdTRUE) {
      return false;
    }

    age = millis() - sample.atMs;
    if (age <= kStaleAfterMs) {
      break;
    }

    ++gDroppedStale;
  }

  // Anything still queued behind a tap being delivered now arrived during the
  // same stall, which means the same person tapping again because the first
  // press looked lost. Delivering those would page through several cards from
  // one intent.
  Sample discard;
  while (xQueueReceive(gQueue, &discard, 0) == pdTRUE) {
    ++gCollapsed;
  }

  ++gDelivered;

  tap = Tap();
  tap.x = sample.x;
  tap.y = sample.y;
  tap.ageMs = age;

  // Buttons first - see Touch.h on why this ordering is fixed rather than
  // incidental.
  for (uint8_t i = 0; i < gZoneCount; ++i) {
    if (contains(gZones[i], tap.x, tap.y)) {
      tap.hit = Hit::ActionButton;
      tap.actionIndex = i;
      return true;
    }
  }

  if (tap.x < kEdgeZoneWidth) {
    tap.hit = Hit::Reverse;
  } else if (tap.x > kScreenWidth - kEdgeZoneWidth) {
    tap.hit = Hit::Forward;
  }
  // A tap in the middle of the card with no button under it is Hit::None -
  // reported, not swallowed, so a caller can still treat "the glass was
  // touched at all" as a signal if it ever wants to.
  return true;
}

void logAndResetCounters() {
  // Unsigned subtraction, which is also what makes a wrap harmless: the
  // difference is right even across the point where the task's counter rolls
  // over.
  const uint32_t sampledNow = gSampled;
  const uint32_t queueFullNow = gDroppedQueueFull;
  const uint32_t sampled = sampledNow - gSampledReported;
  const uint32_t queueFull = queueFullNow - gDroppedQueueFullReported;

  if (sampled == 0 && gDelivered == 0 && gDroppedStale == 0 && gCollapsed == 0 && queueFull == 0) {
    return;
  }

  Log::printf("[touch] %lu sampled, %lu delivered, %lu dropped as stale over %lu ms, "
              "%lu collapsed as repeats, %lu dropped with the queue full",
              static_cast<unsigned long>(sampled), static_cast<unsigned long>(gDelivered),
              static_cast<unsigned long>(gDroppedStale),
              static_cast<unsigned long>(kStaleAfterMs),
              static_cast<unsigned long>(gCollapsed), static_cast<unsigned long>(queueFull));

  gSampledReported = sampledNow;
  gDroppedQueueFullReported = queueFullNow;
  gDelivered = 0;
  gDroppedStale = 0;
  gCollapsed = 0;
}

}  // namespace Touch
