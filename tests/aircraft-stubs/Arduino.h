#pragma once
// Enough of Arduino.h for App/Aircraft.cpp and App/CardManager.cpp to compile
// on a host as shipped, with the headers both of them include.
//
// A String class rather than `using String = std::string`: Aircraft.cpp builds
// a String from a double with a number of decimals, and appends an unsigned
// long to one, and std::string does neither.
//
// string.h, stdio.h, math.h and time.h come in here because Arduino.h brings
// them on the device, and both files call strlen, snprintf and time() on that
// footing.
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <string>

class String : public std::string {
 public:
  String() = default;
  String(const char* text) : std::string(text == nullptr ? "" : text) {}
  String(const std::string& text) : std::string(text) {}
  String(int value) : std::string(std::to_string(value)) {}
  String(double value, int decimals) {
    char buffer[48];
    snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    assign(buffer);
  }
};

inline String operator+(const String& left, const String& right) {
  String joined(left);
  joined.append(right);
  return joined;
}

inline String operator+(const String& left, const char* right) {
  String joined(left);
  joined.append(right == nullptr ? "" : right);
  return joined;
}

inline String operator+(const char* left, const String& right) {
  String joined(left == nullptr ? "" : left);
  joined.append(right);
  return joined;
}

inline String operator+(char left, const String& right) {
  String joined;
  joined.push_back(left);
  joined.append(right);
  return joined;
}

inline String operator+(const String& left, unsigned long right) {
  String joined(left);
  joined.append(std::to_string(right));
  return joined;
}

// The body of an HTTP answer, as HTTPClient::getStream() hands it to
// deserializeJson().
class Stream {
 public:
  std::string body;
};

// The device's clock. The test owns it, so a dwell or a refresh interval passes
// when the test says so.
unsigned long millis();

// CardManager.cpp reads the action button with these. The test answers "not
// pressed" for every read.
constexpr uint8_t INPUT_PULLUP = 5;
constexpr int LOW = 0;
constexpr int HIGH = 1;
void pinMode(uint8_t pin, uint8_t mode);
int digitalRead(uint8_t pin);

// Arduino.h supplies max() unqualified, and Aircraft.cpp's cardIsNotable()
// calls it that way.
template <typename Left, typename Right>
constexpr auto max(Left left, Right right) -> decltype(left > right ? left : right) {
  return left > right ? left : right;
}
