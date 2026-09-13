#include "WaveshareInjection.h"
#include <cassert>
#include <iostream>
using namespace surface::device;
int main() {
  InjectedTouchQueue queue;
  uint32_t now = 1000;
  assert(queue.pending() == 0 && !queue.open());
  // An empty queue leaves the hardware sample alone.
  auto poll = queue.poll(false, false, now);
  assert(!poll.consumed && !poll.cancelled && !poll.held);
  // Bounded: the capacity is the in-flight budget for a host-side drag.
  for (unsigned i = 0; i < InjectedTouchQueue::capacity; ++i)
    assert(queue.push({int(i), int(i) * 2, 1}));
  assert(queue.pending() == InjectedTouchQueue::capacity);
  assert(!queue.push({1, 1, 1}));
  assert(queue.pending() == InjectedTouchQueue::capacity);
  // One sample per poll, in order.
  for (unsigned i = 0; i < InjectedTouchQueue::capacity; ++i) {
    poll = queue.poll(false, false, now);
    assert(poll.consumed && !poll.held);
    assert(poll.sample.x == int(i) && poll.sample.y == int(i) * 2 && poll.sample.fingers == 1);
    assert(queue.pending() == InjectedTouchQueue::capacity - i - 1);
  }
  // The queue is empty, but the last contact left a gesture open.
  poll = queue.poll(false, false, now);
  assert(!poll.consumed && poll.suppress && queue.open());
  queue.cancel();
  // The ring reuses its storage after draining.
  for (int i = 0; i < 40; ++i) {
    assert(queue.push({i, i, 1}));
    poll = queue.poll(false, false, now);
    assert(poll.consumed && poll.sample.x == i);
  }
  // A physical finger cancels everything queued and is reported once.
  assert(queue.push({10, 10, 1}) && queue.push({20, 20, 1}));
  poll = queue.poll(true, false, now);
  assert(poll.cancelled && !poll.consumed && queue.pending() == 0);
  assert(!queue.poll(true, false, now).cancelled);
  // A held screen consumes the contact sample without delivering it, exactly
  // as it swallows a finger held through boot or recovery.
  assert(queue.push({30, 40, 1}));
  poll = queue.poll(false, true, now);
  assert(poll.consumed && poll.held && poll.sample.x == 30 && poll.sample.y == 40);
  // A release is never held: it is what clears the hold.
  assert(queue.push({}));
  poll = queue.poll(false, true, now);
  assert(poll.consumed && !poll.held && poll.sample.fingers == 0);
  // Two fingers are a normal sample; the UI model rejects them itself.
  assert(queue.push({5, 6, 2}));
  poll = queue.poll(false, false, now);
  assert(poll.consumed && poll.sample.fingers == 2);
  queue.push({1, 2, 1});
  queue.cancel();
  assert(queue.pending() == 0 && !queue.poll(false, false, now).consumed);
  // A gap between injected samples is not a lifted finger: an open gesture
  // keeps the screen until its release or the bounded timeout.
  assert(queue.push({100, 200, 1}));
  assert(queue.poll(false, false, now).consumed && queue.open());
  poll = queue.poll(false, false, now + 100);
  assert(poll.suppress && !poll.consumed && !poll.expired && queue.open());
  poll = queue.poll(false, false, now + InjectedTouchQueue::gestureTimeoutMs);
  assert(poll.suppress && !poll.expired);
  poll = queue.poll(false, false, now + InjectedTouchQueue::gestureTimeoutMs + 1);
  assert(poll.expired && !poll.suppress && !poll.consumed && !queue.open());
  assert(!queue.poll(false, false, now + 5000).expired);
  // A release closes the gesture immediately.
  assert(queue.push({100, 200, 1}) && queue.push({}));
  assert(queue.poll(false, false, now).consumed && queue.open());
  poll = queue.poll(false, false, now);
  assert(poll.consumed && poll.sample.fingers == 0 && !queue.open());
  assert(!queue.poll(false, false, now).suppress);
  // A physical finger ends an open gesture and reports it once.
  assert(queue.push({100, 200, 1}));
  assert(queue.poll(false, false, now).consumed && queue.open());
  poll = queue.poll(true, false, now);
  assert(poll.cancelled && !poll.suppress && !queue.open());
  // A contact swallowed by held starts no gesture: it was never delivered.
  assert(queue.push({100, 200, 1}));
  poll = queue.poll(false, true, now);
  assert(poll.held && !queue.open());
  assert(!queue.poll(false, false, now).suppress);
  std::cout << "Waveshare injection checks passed: bounded FIFO, one sample per poll, physical "
               "cancel, gesture hold and expiry, release and held handling\n";
}
