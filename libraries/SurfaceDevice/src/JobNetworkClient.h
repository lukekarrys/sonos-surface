#pragma once
#include "DeadlineClient.h"
#include <NetworkClient.h>
#include <cerrno>
#include <lwip/sockets.h>

namespace surface::device {
// The pinned NetworkClient read path uses MSG_DONTWAIT, but its write path
// retries internally while partial progress continues. Make writes one attempt
// so DeadlineClient owns all waits and can observe cancellation between sends.
class DeadlineSocket : public NetworkClient {
public:
  size_t write(const uint8_t* buffer, size_t size) override {
    if (fd() < 0)
      return 0;
    const auto sent = ::send(fd(), buffer, size, MSG_DONTWAIT);
    if (sent > 0)
      return size_t(sent);
    if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
      stop();
    return 0;
  }
};
using JobNetworkClient = DeadlineClient<DeadlineSocket, IPAddress>;
} // namespace surface::device
