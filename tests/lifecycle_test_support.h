#pragma once
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <string>

// Reproducible across standard libraries/platforms; failure retains the last
// 32 events and a full current snapshot rather than an unbounded trace.
struct LifecycleTrace {
  uint64_t seed, randomState, step = 0;
  std::deque<std::string> history;
  explicit LifecycleTrace(uint64_t value) : seed(value), randomState(value ? value : 1) {}
  uint32_t random() {
    randomState ^= randomState << 13;
    randomState ^= randomState >> 7;
    randomState ^= randomState << 17;
    return uint32_t(randomState >> 16);
  }
  void record(const std::string& event) {
    ++step;
    if (history.size() == 32)
      history.pop_front();
    history.push_back(std::to_string(step) + " " + event);
  }
  void check(bool valid, const char* invariant, const std::string& snapshot) const {
    if (valid)
      return;
    std::cerr << "lifecycle failure seed=" << seed << " step=" << step << " invariant=" << invariant
              << "\nstate=" << snapshot << '\n';
    for (const auto& event : history)
      std::cerr << event << '\n';
    std::abort();
  }
};
