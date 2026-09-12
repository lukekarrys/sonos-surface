#include "DeadlineClient.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
using namespace surface::device;

struct Address {
  bool fromString(const char* host) { return std::strcmp(host, "192.168.1.2") == 0; }
};
struct SocketState {
  uint64_t now = 0, activeJob = 1, cancelAt = UINT64_MAX;
  uint32_t readStep = 1, connectDelay = 0;
  size_t chunk = 1, readOffset = 0, stops = 0, connects = 0;
  size_t reads = 0, peeks = 0, availableCalls = 0, largestRead = 0;
  int availableBytes = 1;
  int32_t connectBudget = 0;
  bool incoming = true, writable = true, open = true;
  std::string incomingBytes = "x", sent;
};
// The parser below follows the pinned Stream timedRead/readStringUntil behavior:
// each successful byte starts a fresh inactivity timeout. It must still stop
// when the client retires this job despite uninterrupted byte arrivals.
struct Socket {
  SocketState* io = nullptr;
  unsigned long timeout = 8000;
  virtual ~Socket() = default;
  virtual int connect(Address, uint16_t) { return 0; }
  virtual int connect(Address, uint16_t, int32_t budget) {
    ++io->connects;
    io->connectBudget = budget;
    io->now += std::min<uint32_t>(io->connectDelay, uint32_t(budget));
    return io->connectDelay < uint32_t(budget);
  }
  virtual int connect(const char*, uint16_t) { return 0; }
  virtual int connect(const char*, uint16_t, int32_t) { return 0; }
  virtual int available() {
    ++io->availableCalls;
    return io->incoming && io->open ? io->availableBytes : 0;
  }
  virtual uint8_t connected() { return io->open; }
  virtual int peek() {
    ++io->peeks;
    return io->incoming && io->open ? 'x' : -1;
  }
  virtual int read() { return -1; }
  virtual int read(uint8_t* buffer, size_t size) {
    ++io->reads;
    io->largestRead = std::max(io->largestRead, size);
    io->now += io->readStep;
    if (!io->incoming || !io->open)
      return 0;
    const auto count = std::min(size, io->chunk);
    for (size_t i = 0; i < count; ++i)
      buffer[i] = io->incomingBytes[io->readOffset++ % io->incomingBytes.size()];
    return int(count);
  }
  virtual size_t readBytes(char*, size_t) { return 0; }
  virtual size_t readBytes(uint8_t*, size_t) { return 0; }
  virtual size_t write(uint8_t) { return 0; }
  virtual size_t write(const uint8_t* bytes, size_t size) {
    ++io->now;
    const auto count = io->writable && io->open ? std::min(size, io->chunk) : 0;
    io->sent.append(reinterpret_cast<const char*>(bytes), count);
    return count;
  }
  virtual void stop() {
    ++io->stops;
    io->open = false;
  }
  void setTimeout(unsigned long value) { timeout = value; }
  unsigned long getTimeout() const { return timeout; }
  int timedRead() {
    const auto start = io->now;
    do {
      const auto byte = read();
      if (byte >= 0)
        return byte;
    } while (io->now - start < timeout);
    return -1;
  }
  std::string readStringUntil(char terminator) {
    std::string result;
    int byte = timedRead();
    while (byte >= 0 && byte != terminator) {
      result += char(byte);
      byte = timedRead();
    }
    return result;
  }
};
struct Fixture {
  SocketState socket;
  DeadlineClient<Socket, Address> client;
  explicit Fixture(uint64_t deadline = 100, size_t maxReadBytes = 73728)
      : client(
            deadline, [&] { return socket.now; },
            [&] { return socket.activeJob == 1 && socket.now < socket.cancelAt; },
            [&] { ++socket.now; }, maxReadBytes) {
    client.io = &socket;
  }
};
int main() {
  // Header allocation is bounded even when the sender can make unlimited
  // progress without advancing the clock. Include a single unbounded line,
  // chunk framing, and a continuous stream of individually short headers.
  for (const auto* bytes : {"X-Unfinished: ", "0000000000000000f", "X-Test: 1\r\n"}) {
    Fixture f(1000000, 8192);
    f.socket.readStep = 0;
    f.socket.incomingBytes = bytes;
    size_t longestLine = 0;
    while (f.client.connected())
      longestLine = std::max(longestLine, f.client.readStringUntil('\n').size());
    assert(longestLine <= 8192 && f.socket.readOffset == 8192 && f.socket.now == 0);
    assert(f.client.cancelled() && f.socket.stops == 1 && f.client.getTimeout() == 0);
  }
  // The default covers a 64KiB body plus 8KiB response framing, cumulatively.
  {
    Fixture f(1000000);
    f.socket.readStep = 0;
    f.socket.chunk = 1024;
    std::vector<uint8_t> bytes(65536);
    assert(f.client.readBytes(bytes.data(), bytes.size()) == 65536);
    assert(!f.client.cancelled());
    assert(f.client.readBytes(bytes.data(), bytes.size()) == 8192);
    assert(f.socket.readOffset == 73728 && f.client.cancelled() && f.socket.stops == 1);
  }
  // Buffered body reads advertise only the remaining budget and never consume
  // a byte beyond the cap, including the final exact-size read.
  for (bool chars : {true, false}) {
    Fixture f(1000000, 1536);
    f.socket.readStep = 0;
    f.socket.chunk = 10000;
    f.socket.availableBytes = 10000;
    assert(f.client.available() == 1536);
    uint8_t bytes[4096]{};
    assert(f.client.read(bytes, sizeof(bytes)) == 1024);
    assert(f.client.available() == 512 && !f.client.cancelled());
    assert(f.client.peek() == 'x' && f.client.peek() == 'x' && f.socket.readOffset == 1024);
    const auto count = chars ? f.client.readBytes(reinterpret_cast<char*>(bytes), 512)
                             : f.client.readBytes(bytes, 512);
    assert(count == 512 && f.socket.readOffset == 1536 && f.socket.reads == 2);
    assert(f.socket.largestRead == 1024 && f.client.cancelled() && f.socket.stops == 1);
    assert(f.client.read(bytes, sizeof(bytes)) == -1 && f.client.peek() == -1 &&
           f.client.available() == 0 && !f.client.connected());
    assert(f.socket.readOffset == 1536 && f.socket.reads == 2 && f.socket.peeks == 2 &&
           f.socket.availableCalls == 2);
    f.client.stop();
    assert(f.socket.stops == 1);
  }
  // Zero-byte budgets cannot bypass the bound through peek or available, and
  // cancellation leaves the same socket cleanup behavior as deadline expiry.
  for (int operation : {0, 1, 2}) {
    Fixture f(1000000, 0);
    if (operation == 0)
      assert(f.client.read() == -1);
    else if (operation == 1)
      assert(f.client.peek() == -1);
    else
      assert(f.client.available() == 0);
    assert(f.client.cancelled() && f.socket.stops == 1 && f.client.getTimeout() == 0);
    assert(f.socket.reads == 0 && f.socket.peeks == 0 && f.socket.availableCalls == 0);
    f.client.stop();
    assert(f.socket.stops == 1);
  }
  // Infinite full header lines cannot keep the HTTP header loop alive.
  {
    Fixture f;
    f.socket.incomingBytes = "X-Test: 1\r\n";
    unsigned lines = 0;
    while (f.client.connected()) {
      f.client.readStringUntil('\n');
      ++lines;
    }
    assert(lines > 1 && f.socket.now == 100 && f.client.cancelled() && f.socket.stops == 1);
  }
  // Unfinished header and chunk-size lines both bypass socket inactivity
  // timeouts. The absolute deadline interrupts their inner Stream parser.
  for (const auto* bytes : {"X-Unfinished: ", "0000000000000000f"}) {
    Fixture f;
    f.socket.incomingBytes = bytes;
    assert(f.client.readStringUntil('\n').size() == 100);
    assert(f.socket.now == 100 && f.client.cancelled() && f.client.getTimeout() == 0 &&
           f.socket.stops == 1);
    f.client.stop();
    assert(f.socket.stops == 1 && f.client.peek() == -1 && !f.client.connected() &&
           !f.client.available() && f.client.read() == -1);
  }
  // Body readBytes must stop although each arriving byte resets idle timeout.
  for (bool chars : {true, false}) {
    Fixture f;
    uint8_t bytes[256]{};
    const auto count = chars ? f.client.readBytes(reinterpret_cast<char*>(bytes), sizeof(bytes))
                             : f.client.readBytes(bytes, sizeof(bytes));
    assert(count == 100 && f.socket.now == 100 && f.client.cancelled() && f.socket.stops == 1);
  }
  // Cancellation is latched: a later job becoming active cannot revive A.
  {
    Fixture f;
    f.socket.cancelAt = 23;
    assert(f.client.readStringUntil('\n').size() == 23 && f.socket.now == 23);
    f.socket.cancelAt = UINT64_MAX;
    f.socket.activeJob = 2;
    assert(!f.client.connect("192.168.1.2", 1400, 3000));
    assert(f.client.read() == -1 && f.socket.stops == 1 && f.socket.connects == 0);
    f.socket.activeJob = 1;
    assert(!f.client.connected());
    Fixture next;
    assert(next.client.connect("192.168.1.2", 1400, 3000));
    assert(next.client.readStringUntil('x').empty() && !next.client.cancelled());
  }
  // Once a write partially succeeds it returns its exact prefix, never replaying
  // that prefix when cancellation interrupts the next nonblocking send.
  {
    Fixture f(7);
    const std::string payload = "mutation payload that must not replay";
    assert(f.client.write(reinterpret_cast<const uint8_t*>(payload.data()), payload.size()) == 7);
    assert(f.socket.sent == payload.substr(0, 7) && f.client.cancelled());
    assert(f.client.write(uint8_t('!')) == 0 && f.socket.stops == 1);
  }
  // No-progress paths retain the ordinary idle timeout and yield to the adapter.
  for (bool reading : {true, false}) {
    Fixture f;
    f.client.setTimeout(5);
    f.socket.incoming = f.socket.writable = false;
    uint8_t byte = 0;
    assert((reading ? f.client.readBytes(&byte, 1) : f.client.write(&byte, 1)) == 0);
    assert(f.socket.now == 5 && !f.client.cancelled());
  }
  // Connect gets the smaller of its explicit budget and remaining job lifetime.
  {
    Fixture f;
    f.socket.now = 90;
    f.socket.connectDelay = 50;
    assert(!f.client.connect("192.168.1.2", 1400, 3000));
    assert(f.socket.connectBudget == 10 && f.socket.now == 100 && f.client.cancelled());
    assert(f.socket.stops == 1);
    Fixture shortAttempt;
    shortAttempt.socket.connectDelay = 50;
    assert(!shortAttempt.client.connect(Address{}, 1400, 20));
    assert(shortAttempt.socket.connectBudget == 20 && shortAttempt.socket.now == 20);
    assert(!shortAttempt.client.cancelled());
  }
  // Hostnames never invoke potentially unbounded DNS, and already-expired jobs
  // cannot open sockets even with a nonpositive/unbounded requested timeout.
  {
    Fixture f;
    assert(!f.client.connect("speaker.local", 1400) && f.socket.connects == 0);
    f.socket.now = 100;
    assert(!f.client.connect(Address{}, 1400, -1) && f.socket.connects == 0);
    assert(f.client.cancelled() && f.socket.stops == 1);
  }
  std::cout
      << "deadline client: response byte limit, parser progress, cancellation, partial writes, "
         "bounded connect passed\n";
}
