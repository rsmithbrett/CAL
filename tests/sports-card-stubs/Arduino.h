#pragma once
// Enough of Arduino.h for App/Sports.h, App/Cards.h, App/CardColumns.h,
// App/Display.h and App/Log.h to parse on a host, and for the String operations
// App/Sports.cpp performs.
//
// A class rather than `using String = std::string`, which the loader and
// recovery stubs use: Sports.cpp's scoreText() builds a String from an int16_t,
// and std::string has no constructor for one.
//
// The operator+ overloads below return String rather than std::string, and that
// is what keeps describeCardAt()'s `text += " " + awayScore` compiling - the
// left operand there is a char literal pointer and the right is a String, which
// std::string's own operators answer as std::string while the `+=` target is a
// String.
//
// string.h, stdio.h and time.h come in here because Arduino.h brings them on the
// device: Sports.cpp calls strcmp, strncpy, snprintf, time() and gmtime_r, and
// includes a header for none of them.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <string>

// What Sports.cpp's startTimeText() and listingIdCardAt() both call. gmtime_r is
// POSIX, so glibc declares it and the ESP32 core's newlib defines it; a Windows
// time.h carries gmtime_s instead, which takes its two arguments the other way
// round and answers an error code rather than a pointer. So this stands in on
// Windows and the platform's own function serves everywhere else: glibc declares
// gmtime_r noexcept, and a definition here would clash with it on the exception
// specifier even where gmtime_s existed to call.
#ifdef _WIN32
inline struct tm* gmtime_r(const time_t* when, struct tm* into) {
  return gmtime_s(into, when) == 0 ? into : nullptr;
}
#endif

class String : public std::string {
 public:
  String() = default;
  String(const char* text) : std::string(text == nullptr ? "" : text) {}
  String(const std::string& text) : std::string(text) {}
  String(int value) : std::string(std::to_string(value)) {}
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
