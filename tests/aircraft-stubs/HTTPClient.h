#pragma once
// The part of HTTPClient that App/Aircraft.cpp's fetchMine() calls. The test
// sets `status` and `body` before a fetch, and the request that follows gets
// that answer.
#include <Arduino.h>

class HTTPClient {
 public:
  int status = 0;
  std::string body;

  void setTimeout(uint32_t) {}
  void addHeader(const String&, const String&) {}
  int GET() { return status; }
  void end() {}
  String getString() { return String(body); }
  Stream& getStream() {
    stream_.body = body;
    return stream_;
  }

 private:
  Stream stream_;
};
