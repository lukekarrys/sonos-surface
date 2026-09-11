#pragma once
#if defined(SURFACE_STICK_S3)
#include "CardWriter.h"
#include "WriterHttp.h"
#include <WiFi.h>

namespace surface::device {
class WriterServer {
  WiFiServer server{80};
  WiFiClient client;
  WriterHttpRequest request;
  std::string output, ip;
  size_t sent = 0;
  uint32_t accepted = 0;
  bool started = false;
  void reply(int code, const std::string& type, const std::string& body);

public:
  // Returns deliberate page-opening activity; API action activity belongs to CardWriter.
  bool poll(CardWriter& writer, uint64_t now);
  void stop();
  const std::string& address() const { return ip; }
};
} // namespace surface::device
#endif
