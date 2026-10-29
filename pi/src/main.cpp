#include "Layout.h"
#include "LiveClient.h"
#include <future>
#include <chrono>

#include <SDL.h>
#include <SDL_ttf.h>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void fill(SDL_Renderer* renderer, PiLayout::Rect r, SDL_Color color) {
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  SDL_Rect rect{r.x, r.y, r.w, r.h};
  SDL_RenderFillRect(renderer, &rect);
}

void label(SDL_Renderer* renderer, const std::string& text, PiLayout::Rect area,
           int fontSize, SDL_Color color, const std::string& fontPath) {
  if (area.w <= 0 || area.h <= 0) return;
  TTF_Font* font = TTF_OpenFont(fontPath.c_str(), fontSize);
  if (!font) throw std::runtime_error(TTF_GetError());
  SDL_Surface* surface = TTF_RenderUTF8_Blended(font, text.c_str(), color);
  TTF_CloseFont(font);
  if (!surface) throw std::runtime_error(TTF_GetError());
  SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
  if (!texture) {
    SDL_FreeSurface(surface);
    throw std::runtime_error(SDL_GetError());
  }
  const double scale = std::min({1.0, double(area.w) / surface->w,
                                  double(area.h) / surface->h});
  SDL_Rect target{area.x, area.y, std::max(1, int(surface->w * scale)),
                  std::max(1, int(surface->h * scale))};
  SDL_RenderCopy(renderer, texture, nullptr, &target);
  SDL_DestroyTexture(texture);
  SDL_FreeSurface(surface);
}

void draw(SDL_Renderer* renderer, int width, int height,
          const std::string& fontPath) {
  const auto frame = PiLayout::makeFrame(width, height, 4, 3);
  fill(renderer, {0, 0, width, height}, {16, 32, 49, 255});
  fill(renderer, frame.safe, {244, 247, 250, 255});
  fill(renderer, frame.header, {18, 65, 94, 255});
  const int pad = std::max(6, frame.safe.x / 2);
  label(renderer, "DISCOVER AROUND ME - DEVELOPMENT FIXTURE",
        {frame.header.x + pad, frame.header.y + pad,
         frame.header.w - 2 * pad, frame.header.h - 2 * pad},
        std::clamp(height / 28, 14, 36), {255, 255, 255, 255}, fontPath);

  // A 4:3 graphic fixture centered with contain. The server asset fetch and
  // actual decoded image are later stages; this color block proves geometry.
  fill(renderer, frame.graphic, {208, 227, 235, 255});
  const int inset = std::max(6, frame.graphic.w / 25);
  label(renderer, "FORECAST", {frame.graphic.x + inset, frame.graphic.y + inset,
                                frame.graphic.w - 2 * inset, frame.graphic.h / 5},
        std::clamp(height / 20, 18, 50), {18, 65, 94, 255}, fontPath);
  label(renderer, "72 F  |  Sample content",
        {frame.graphic.x + inset, frame.graphic.y + frame.graphic.h / 3,
         frame.graphic.w - 2 * inset, frame.graphic.h / 3},
        std::clamp(height / 13, 22, 78), {18, 65, 94, 255}, fontPath);

  fill(renderer, frame.footer, {224, 231, 235, 255});
  label(renderer, "Fixture only - no cloud data or device enrollment",
        {frame.footer.x + pad, frame.footer.y + pad,
         frame.footer.w - 2 * pad, frame.footer.h - 2 * pad},
        std::clamp(height / 34, 12, 28), {18, 65, 94, 255}, fontPath);
  SDL_RenderPresent(renderer);
}

void drawLive(SDL_Renderer* renderer, int width, int height,
              const std::string& fontPath, const PiCards::Runtime& state) {
  const auto frame = PiLayout::makeFrame(width, height, 4, 3);
  fill(renderer, {0, 0, width, height}, {16, 32, 49, 255});
  fill(renderer, frame.safe, {244, 247, 250, 255});
  fill(renderer, frame.header, {18, 65, 94, 255});
  const int pad = std::max(4, frame.safe.x / 2);
  const auto* forecast = state.displayed();
  label(renderer, forecast && !forecast->location.empty() ? forecast->location + " - Forecast" : "Discover Around Me",
      {frame.header.x + pad, frame.header.y + pad, frame.header.w - 2 * pad, frame.header.h - 2 * pad},
      std::clamp(height / 28, 14, 36), {255,255,255,255}, fontPath);
  // One combined slide, matching the current forecast card's single-item semantics.
  // Every period remains visible even on the smallest supported viewport.
  const PiLayout::Rect area{frame.safe.x, frame.header.y + frame.header.h, frame.safe.w, frame.footer.y - frame.header.y - frame.header.h};
  if (!forecast || forecast->periods.empty()) {
    label(renderer, forecast ? forecast->status : state.status,
        {area.x + pad, area.y + pad, area.w - 2 * pad, area.h - 2 * pad},
        std::clamp(height / 22, 14, 36), {18,65,94,255}, fontPath);
  } else {
    const int rowHeight = area.h / static_cast<int>(forecast->periods.size());
    for (size_t i = 0; i < forecast->periods.size(); ++i) {
      const auto& period = forecast->periods[i];
      label(renderer, period.name + "  " + std::to_string(period.temperature) + " " + period.unit + "  " + period.forecast,
          {area.x + pad, area.y + static_cast<int>(i) * rowHeight, area.w - 2 * pad, rowHeight},
          std::clamp(rowHeight / 2, 12, 36), {18,65,94,255}, fontPath);
    }
  }
  fill(renderer, frame.footer, {224,231,235,255});
  label(renderer, forecast ? forecast->status : state.status,
      {frame.footer.x + pad, frame.footer.y + pad, frame.footer.w - 2 * pad, frame.footer.h - 2 * pad},
      std::clamp(height / 34, 12, 28), {18,65,94,255}, fontPath);
  SDL_RenderPresent(renderer);
}

void save(SDL_Renderer* renderer, int width, int height,
          const std::string& path) {
  SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(
      0, width, height, 32, SDL_PIXELFORMAT_ARGB8888);
  if (!surface) throw std::runtime_error(SDL_GetError());
  if (SDL_RenderReadPixels(renderer, nullptr, surface->format->format,
                           surface->pixels, surface->pitch) != 0 ||
      SDL_SaveBMP(surface, path.c_str()) != 0) {
    std::string error = SDL_GetError();
    SDL_FreeSurface(surface);
    throw std::runtime_error(error);
  }
  SDL_FreeSurface(surface);
}

}  // namespace

int main(int argc, char** argv) {
  int width = 1280, height = 720;
  std::string screenshot;
  std::string origin, credentialPath, version;
  std::string font = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
  try {
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if ((arg == "--width" || arg == "--height" || arg == "--once" ||
           arg == "--font" || arg == "--server" || arg == "--credentials" || arg == "--version") && i + 1 < argc) {
        const std::string value = argv[++i];
        if (arg == "--width") width = std::stoi(value);
        else if (arg == "--height") height = std::stoi(value);
        else if (arg == "--font") font = value;
        else if (arg == "--server") origin = value;
        else if (arg == "--credentials") credentialPath = value;
        else if (arg == "--version") version = value;
        else screenshot = value;
      } else {
        throw std::runtime_error("usage: pi-card [--width N --height N] [--font PATH] [--once output.bmp]");
      }
    }
    const bool live = !origin.empty() || !credentialPath.empty() || !version.empty();
    if (live && (origin.empty() || credentialPath.empty() || version.empty()))
      throw std::runtime_error("live mode requires --server, --credentials and --version");
    PiClient::heartbeat(live ? version : "fixture");
    PiInstallation::Credentials credentials;
    if (live) credentials = PiInstallation::load(credentialPath);
    if (width < 160 || height < 120 || width > 3840 || height > 2160)
      throw std::runtime_error("viewport outside 160x120..3840x2160");
    if (live && curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
      throw std::runtime_error("could not initialize transport");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) throw std::runtime_error(SDL_GetError());
    if (TTF_Init() != 0) throw std::runtime_error(TTF_GetError());
    SDL_Window* window = SDL_CreateWindow("Discover Around Me Pi fixture",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height,
        screenshot.empty() ? SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_HIDDEN);
    if (!window) throw std::runtime_error(SDL_GetError());
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) throw std::runtime_error(SDL_GetError());
    SDL_GetRendererOutputSize(renderer, &width, &height);
    PiCards::Runtime state;
    auto render = [&] {
      if (live) drawLive(renderer, width, height, font, state);
      else draw(renderer, width, height, font);
    };
    render();
    if (!screenshot.empty()) {
      if (live) {
        if (!PiClient::refresh(state, origin, credentials, version))
          throw std::runtime_error("live frame check-in failed");
        render();
      }
      save(renderer, width, height, screenshot);
      std::cout << (live ? "server policy frame " : "fixture frame ") << width << 'x' << height << " -> "
                << screenshot << '\n';
    } else {
      bool running = true;
      using Clock = std::chrono::steady_clock;
      auto nextRefresh = Clock::now(), nextCard = Clock::now() + std::chrono::seconds(state.dwell());
      std::future<std::pair<PiCards::Runtime, bool>> pending;
      while (running) {
        const auto now = Clock::now();
        if (live && !pending.valid() && now >= nextRefresh) {
          // Copy state into the worker: SDL and scheduling stay on this thread.
          pending = std::async(std::launch::async, [state, origin, credentials, version]() mutable {
            const bool ok = PiClient::refresh(state, origin, credentials, version);
            return std::make_pair(std::move(state), ok);
          });
          nextRefresh = now + std::chrono::seconds(60);
        }
        if (pending.valid() && pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
          auto result = pending.get();
          state.acceptRefresh(std::move(result.first));
          std::cout << "policy refresh " << (result.second ? "validated" : "unavailable")
                    << ", unsupported cards " << state.unknownCount << '\n';
          nextCard = now + std::chrono::seconds(state.dwell());
          render();
        }
        if (live && now >= nextCard) {
          state.advance();
          nextCard = now + std::chrono::seconds(state.dwell());
          render();
        }
        SDL_Event event;
        if (SDL_WaitEventTimeout(&event, 100)) {
          if (event.type == SDL_QUIT ||
              (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)) running = false;
          if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            SDL_GetRendererOutputSize(renderer, &width, &height);
            render();
          }
        }
      }
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
    if (live) curl_global_cleanup();
  } catch (const std::exception& e) {
    std::cerr << "pi-card: " << e.what() << '\n';
    return 1;
  }
}
