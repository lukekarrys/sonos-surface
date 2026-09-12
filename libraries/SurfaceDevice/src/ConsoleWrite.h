#pragma once
#include <cstdint>
#include <string>

namespace surface::device {
// The adapter must configure nonblocking availableForWrite/write calls. Routine
// diagnostics get one attempt; explicit command replies may yield within a bound.
template <class Port, class Clock, class Yield>
bool writeConsoleLine(Port& port, const std::string& line, uint32_t waitMs, Clock now,
                      Yield yield) {
  const uint32_t start = now();
  for (;;) {
    const int available = port.availableForWrite();
    if (available >= 0 && size_t(available) >= line.size()) {
      const auto sent = port.write(reinterpret_cast<const uint8_t*>(line.data()), line.size());
      if (sent)
        return sent == line.size(); // Never duplicate a partially transmitted reply.
    }
    if (uint32_t(now() - start) >= waitMs)
      return false;
    yield();
  }
}
} // namespace surface::device
