#pragma once
// Enough of Arduino.h for App/Assets.h, App/Cards.h, App/Display.h and
// App/Log.h to parse on a host, and for the String operations
// App/PersonalPhotos.cpp performs.
//
// A class rather than `using String = std::string`, which the stubs beside this
// one use: invalidateDeparted() appends an unsigned to a String, and
// std::string has no operator+= for one.
#include <stdint.h>
#include <stddef.h>

#include <string>

class String : public std::string {
 public:
  String() = default;
  String(const char* text) : std::string(text == nullptr ? "" : text) {}
  String(const std::string& text) : std::string(text) {}

  String& operator+=(const char* text) {
    std::string::operator+=(text == nullptr ? "" : text);
    return *this;
  }

  String& operator+=(const std::string& text) {
    std::string::operator+=(text);
    return *this;
  }

  String& operator+=(unsigned value) {
    std::string::operator+=(std::to_string(value));
    return *this;
  }
};

uint32_t millis();
void delay(uint32_t ms);
