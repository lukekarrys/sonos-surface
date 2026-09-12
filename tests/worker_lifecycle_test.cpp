#include "WorkerLifecycle.h"
#include "lifecycle_test_support.h"
#include <cassert>
#include <set>
#include <vector>
using namespace surface::device;
using namespace surface::device::worker_lifecycle;
struct Effects final : WorkerEffects {
  std::vector<uint64_t> starts;
  std::set<uint64_t> terminals;
  uint64_t stale = 0;
  bool valid = true;
  void started(uint64_t id, uint64_t deadline) override {
    valid = valid && id != 0 && deadline != 0 && (starts.empty() || id > starts.back());
    starts.push_back(id);
  }
  void finished(uint64_t id, JobOutcome) override {
    const bool inserted = terminals.insert(id).second;
    valid = valid && id != 0 && inserted;
  }
  void staleResult(uint64_t) override { ++stale; }
};
void examples() {
  Effects effects;
  WorkerLifecycle worker(effects);
  assert(workerInvariant(worker.snapshot()));
  assert(!worker.submit(0, 0));
  assert(!worker.submit(UINT64_MAX, 1));
  const auto a = worker.submit(10, 20);
  assert(a == 1 && worker.active(a, 29) && !worker.active(a, 30));
  assert(!worker.submit(11, 20));
  worker.process(Success{a + 1});
  worker.process(Failure{a + 1});
  assert(worker.snapshot().jobId == a);
  worker.process(DeadlineExpired{29});
  assert(worker.snapshot().running);
  worker.process(DeadlineExpired{30});
  assert(!worker.snapshot().running && worker.snapshot().lastOutcome == JobOutcome::Timeout);
  const auto b = worker.submit(31, 20);
  worker.process(Success{a});
  worker.process(Failure{a});
  assert(worker.snapshot().jobId == b && worker.snapshot().running);
  worker.process(Success{b});
  assert(!worker.snapshot().running);
  for (auto outcome : {JobOutcome::Failure, JobOutcome::NetworkLost, JobOutcome::Unavailable,
                       JobOutcome::Shutdown}) {
    const auto id = worker.submit(100, 20);
    switch (outcome) {
    case JobOutcome::Failure:
      worker.process(Failure{id});
      break;
    case JobOutcome::NetworkLost:
      worker.process(NetworkUnavailable{});
      break;
    case JobOutcome::Unavailable:
      worker.process(WorkerUnavailable{});
      break;
    case JobOutcome::Shutdown:
      worker.process(Shutdown{});
      break;
    default:
      assert(false);
    }
    assert(worker.snapshot().lastOutcome == outcome && workerInvariant(worker.snapshot()));
    const auto next = worker.submit(130, 10);
    worker.process(Success{next});
    assert(workerInvariant(worker.snapshot()));
  }
  for (uint64_t i = 0; i < 10000; ++i) {
    const auto id = worker.submit(200 + i, 1);
    assert(id);
    worker.process(Success{id});
    assert(workerInvariant(worker.snapshot()));
  }
  assert(effects.valid);
}
std::string describe(const WorkerSnapshot& s) {
  return std::string(s.running ? "Running" : "Idle") + " id=" + std::to_string(s.jobId) +
         " startedAt=" + std::to_string(s.startedAt) + " deadline=" + std::to_string(s.deadline) +
         " accepted=" + std::to_string(s.accepted) + " completed=" + std::to_string(s.completed) +
         " staleResults=" + std::to_string(s.staleResults) +
         " outcome=" + outcomeName(s.lastOutcome);
}
void chaos(uint64_t seed, uint64_t steps) {
  LifecycleTrace trace(seed);
  Effects effects;
  WorkerLifecycle worker(effects);
  uint64_t now = 0, modelId = 0, modelDeadline = 0, accepted = 0, completed = 0;
  for (uint64_t i = 0; i < steps; ++i) {
    now += trace.random() % 20;
    const auto event = trace.random() % 10;
    const auto callback = trace.random() % 2 ? modelId : (accepted ? accepted - 1 : 999);
    const char* names[] = {"submit",
                           "submit",
                           "success",
                           "failure",
                           "deadline",
                           "network-lost",
                           "worker-unavailable",
                           "shutdown",
                           "stale-success",
                           "stale-failure"};
    trace.record(std::string(names[event]) + " now=" + std::to_string(now) +
                 " callback=" + std::to_string(callback));
    bool terminal = false;
    uint64_t submitResult = 0, expectedSubmit = 0;
    switch (event) {
    case 0:
    case 1: {
      submitResult = worker.submit(now, 50);
      if (!modelId) {
        modelId = ++accepted;
        modelDeadline = now + 50;
        expectedSubmit = modelId;
      }
      break;
    }
    case 2:
    case 3:
      if (event == 2)
        worker.process(Success{callback});
      else
        worker.process(Failure{callback});
      terminal = modelId && callback == modelId;
      break;
    case 4:
      worker.process(DeadlineExpired{now});
      terminal = modelId && now >= modelDeadline;
      break;
    case 5:
      worker.process(NetworkUnavailable{});
      terminal = modelId != 0;
      break;
    case 6:
      worker.process(WorkerUnavailable{});
      terminal = modelId != 0;
      break;
    case 7:
      worker.process(Shutdown{});
      terminal = modelId != 0;
      break;
    case 8:
      worker.process(Success{UINT64_MAX});
      break;
    case 9:
      worker.process(Failure{UINT64_MAX});
      break;
    }
    if (terminal) {
      modelId = 0;
      ++completed;
    }
    const auto s = worker.snapshot();
    const auto state = describe(s) + " submitResult=" + std::to_string(submitResult) +
                       " expectedSubmit=" + std::to_string(expectedSubmit);
    trace.check(submitResult == expectedSubmit, "submit admission and returned identity", state);
    trace.check(effects.valid, "monotonic starts and unique terminal effects", state);
    trace.check(workerInvariant(s), "worker invariant", state);
    trace.check(s.jobId == modelId && s.accepted == accepted && s.completed == completed,
                "independent reference model", state);
    trace.check(effects.starts.size() == accepted && effects.terminals.size() == completed,
                "one terminal effect per accepted job", state);
  }
  trace.record("final deadline now=" + std::to_string(UINT64_MAX));
  worker.process(DeadlineExpired{UINT64_MAX});
  const auto s = worker.snapshot();
  trace.check(effects.valid && workerInvariant(s) && !s.running,
              "eventual deadline terminates final job exactly once", describe(s));
}
int main(int argc, char** argv) {
  const auto seed = argc > 1 ? std::stoull(argv[1]) : 0x5eed1234ULL;
  const auto steps = argc > 2 ? std::stoull(argv[2]) : 100000ULL;
  examples();
  chaos(seed, steps);
  std::cout << "worker lifecycle: examples + " << steps << " transitions seed=" << seed << '\n';
}
