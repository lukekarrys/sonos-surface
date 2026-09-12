#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

namespace surface::device {
// Socket supplies the Arduino Client/Stream interface, but read(buffer, count)
// and write(buffer, count) must each make one bounded, nonblocking attempt.
// connect(address, port, timeout) must honor its supplied timeout. Address must
// parse numeric addresses via fromString(); DNS is deliberately not invoked.
// Each instance belongs to one HTTP request and cannot reopen after stop().
template <class Socket, class Address> class DeadlineClient : public Socket {
public:
  using Clock = std::function<uint64_t()>;
  using Active = std::function<bool()>;
  using Wait = std::function<void()>;

  DeadlineClient(uint64_t deadline, Clock clock, Active active, Wait wait,
                 size_t maxReadBytes = 73728)
      : deadline_(deadline), clock_(std::move(clock)), active_(std::move(active)),
        wait_(std::move(wait)), remainingReadBytes_(maxReadBytes) {}
  DeadlineClient(const DeadlineClient&) = delete;
  DeadlineClient& operator=(const DeadlineClient&) = delete;

  int connect(Address address, uint16_t port) override { return connect(address, port, 3000); }
  int connect(Address address, uint16_t port, int32_t timeout) override {
    if (!usable())
      return 0;
    const auto now = clock_();
    if (now >= deadline_) {
      cancel();
      return 0;
    }
    const auto remaining = deadline_ - now;
    const uint64_t requested = timeout > 0 ? uint64_t(timeout) : uint64_t(3000);
    const auto budget = std::min(remaining, requested);
    if (!budget) {
      cancel();
      return 0;
    }
    const int connected =
        Socket::connect(address, port, static_cast<int32_t>(std::min<uint64_t>(budget, INT32_MAX)));
    return usable() ? connected : 0;
  }
  int connect(const char* host, uint16_t port) override { return connect(host, port, 3000); }
  int connect(const char* host, uint16_t port, int32_t timeout) override {
    Address address;
    return host && address.fromString(host) ? connect(address, port, timeout) : 0;
  }
  int available() override {
    if (!readable())
      return 0;
    const auto count = Socket::available();
    return count > 0 ? int(std::min(size_t(count), remainingReadBytes_)) : count;
  }
  uint8_t connected() override { return usable() ? Socket::connected() : 0; }
  int peek() override { return readable() ? Socket::peek() : -1; }
  int read() override {
    uint8_t byte = 0;
    return read(&byte, 1) == 1 ? byte : -1;
  }
  int read(uint8_t* buffer, size_t size) override {
    if (!readable())
      return -1;
    // Bound work inside a base-client call even for a continuously full socket.
    const auto count = Socket::read(buffer, std::min({size, size_t(1024), remainingReadBytes_}));
    if (count > 0) {
      remainingReadBytes_ -= size_t(count);
      if (!remainingReadBytes_)
        cancel();
    }
    return count;
  }
  size_t readBytes(char* buffer, size_t size) override {
    return readBytes(reinterpret_cast<uint8_t*>(buffer), size);
  }
  size_t readBytes(uint8_t* buffer, size_t size) override {
    size_t received = 0;
    auto progressAt = clock_();
    while (received < size && usable()) {
      const int count = read(buffer + received, size - received);
      if (count < 0)
        break;
      if (count) {
        received += size_t(count);
        progressAt = clock_();
      } else {
        if (clock_() - progressAt >= this->getTimeout())
          break;
        wait_();
      }
    }
    return received;
  }
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* buffer, size_t size) override {
    size_t sent = 0;
    auto progressAt = clock_();
    while (sent < size && usable()) {
      const size_t count = Socket::write(buffer + sent, std::min(size - sent, size_t(1024)));
      sent += count;
      if (count)
        progressAt = clock_();
      else {
        if (clock_() - progressAt >= this->getTimeout())
          break;
        wait_();
      }
    }
    return sent; // Preserve exact partial progress; never resend acknowledged bytes.
  }
  void stop() override {
    if (closed_)
      return;
    closed_ = true;
    // Stream::timedRead/timedPeek retry virtual read/peek until this timeout.
    // Setting zero is essential when cancellation happens inside readStringUntil.
    this->setTimeout(0);
    Socket::stop();
  }
  bool cancelled() const { return cancelled_; }

private:
  void cancel() {
    cancelled_ = true;
    stop();
  }
  bool usable() {
    if (closed_)
      return false;
    if (clock_() >= deadline_ || !active_()) {
      cancel();
      return false;
    }
    return true;
  }
  bool readable() {
    if (!usable())
      return false;
    // HTTP header and chunk framing parsers allocate before the body sink can
    // enforce its own limit. Bound the entire response, including that framing.
    if (!remainingReadBytes_) {
      cancel();
      return false;
    }
    return true;
  }
  uint64_t deadline_;
  Clock clock_;
  Active active_;
  Wait wait_;
  size_t remainingReadBytes_;
  bool closed_ = false, cancelled_ = false;
};
} // namespace surface::device
