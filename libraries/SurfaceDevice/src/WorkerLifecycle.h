#pragma once
#include <surface_sml.hpp>
#include <cstdint>
#include <limits>

namespace surface::device {
// All lifecycle clocks are monotonic milliseconds, supplied by the caller.
// No platform calls, waits, or cross-machine ownership belong in this model.
enum class JobOutcome {
  None,
  Success,
  Failure,
  Timeout,
  NetworkLost,
  Unavailable,
  Shutdown,
  Preempted
};
// User jobs come from local input (button, touch, NFC, USB). Automatic jobs are
// scheduled reads; only they can be preempted, and they never preempt anything.
enum class JobOrigin { User, Automatic };
inline const char* originName(JobOrigin origin) {
  return origin == JobOrigin::Automatic ? "automatic" : "user";
}
inline const char* outcomeName(JobOutcome outcome) {
  switch (outcome) {
  case JobOutcome::None:
    return "none";
  case JobOutcome::Success:
    return "success";
  case JobOutcome::Failure:
    return "failure";
  case JobOutcome::Timeout:
    return "timeout";
  case JobOutcome::NetworkLost:
    return "network-lost";
  case JobOutcome::Unavailable:
    return "unavailable";
  case JobOutcome::Shutdown:
    return "shutdown";
  case JobOutcome::Preempted:
    return "preempted";
  }
  return "unknown";
}
struct WorkerSnapshot {
  bool running = false;
  uint64_t jobId = 0, startedAt = 0, deadline = 0;
  uint64_t lastJobId = 0, accepted = 0, completed = 0, staleResults = 0;
  uint64_t preempted = 0, ignoredPreempts = 0;
  JobOrigin origin = JobOrigin::User, lastOrigin = JobOrigin::User;
  JobOutcome lastOutcome = JobOutcome::None;
};
struct WorkerEffects {
  virtual ~WorkerEffects() = default;
  virtual void started(uint64_t id, uint64_t deadline, JobOrigin origin) = 0;
  virtual void finished(uint64_t id, JobOutcome outcome) = 0;
  virtual void staleResult(uint64_t id) = 0;
};
namespace worker_lifecycle {
struct Idle {};
struct Running {};
struct Submit {
  uint64_t now, budget;
  JobOrigin origin = JobOrigin::User;
};
struct Preempt {};
struct Success {
  uint64_t id;
};
struct Failure {
  uint64_t id;
};
struct DeadlineExpired {
  uint64_t now;
};
struct NetworkUnavailable {};
struct WorkerUnavailable {};
struct Shutdown {};
struct Context {
  WorkerSnapshot data;
  WorkerEffects& effects;
  void finish(JobOutcome outcome) {
    const auto id = data.jobId;
    data.lastJobId = id;
    data.lastOrigin = data.origin;
    data.jobId = data.startedAt = data.deadline = 0;
    data.origin = JobOrigin::User;
    data.lastOutcome = outcome;
    ++data.completed;
    effects.finished(id, outcome);
  }
};
struct Table {
  auto operator()() const {
    using namespace boost::sml;
    const auto valid = [](const Submit& e, const Context& c) {
      return e.budget && e.now <= std::numeric_limits<uint64_t>::max() - e.budget &&
             c.data.accepted != std::numeric_limits<uint64_t>::max();
    };
    const auto start = [](const Submit& e, Context& c) {
      c.data.jobId = ++c.data.accepted;
      c.data.startedAt = e.now;
      c.data.deadline = e.now + e.budget;
      c.data.origin = e.origin;
      c.effects.started(c.data.jobId, c.data.deadline, e.origin);
    };
    const auto successMatches = [](const Success& e, const Context& c) {
      return e.id == c.data.jobId;
    };
    const auto failureMatches = [](const Failure& e, const Context& c) {
      return e.id == c.data.jobId;
    };
    const auto lateSuccess = [](const Success& e, Context& c) {
      ++c.data.staleResults;
      c.effects.staleResult(e.id);
    };
    const auto lateFailure = [](const Failure& e, Context& c) {
      ++c.data.staleResults;
      c.effects.staleResult(e.id);
    };
    const auto automatic = [](const Context& c) { return c.data.origin == JobOrigin::Automatic; };
    const auto ignorePreempt = [](Context& c) { ++c.data.ignoredPreempts; };
    return make_transition_table(
        *state<Idle> + event<Submit>[valid] / start = state<Running>,
        state<Running> + event<Success>[successMatches] /
                             [](Context& c) { c.finish(JobOutcome::Success); } = state<Idle>,
        state<Running> + event<Failure>[failureMatches] /
                             [](Context& c) { c.finish(JobOutcome::Failure); } = state<Idle>,
        state<Running> + event<Success>[!successMatches] / lateSuccess,
        state<Running> + event<Failure>[!failureMatches] / lateFailure,
        state<Idle> + event<Success> / lateSuccess, state<Idle> + event<Failure> / lateFailure,
        state<Running> + event<DeadlineExpired>[([](const DeadlineExpired& e, const Context& c) {
                           return e.now >= c.data.deadline;
                         })] /
                             [](Context& c) { c.finish(JobOutcome::Timeout); } = state<Idle>,
        state<Running> + event<NetworkUnavailable> /
                             [](Context& c) { c.finish(JobOutcome::NetworkLost); } = state<Idle>,
        state<Running> + event<WorkerUnavailable> /
                             [](Context& c) { c.finish(JobOutcome::Unavailable); } = state<Idle>,
        state<Running> + event<Shutdown> / [](Context& c) { c.finish(JobOutcome::Shutdown); } =
            state<Idle>,
        state<Running> + event<Preempt>[automatic] /
                             [](Context& c) {
                               ++c.data.preempted;
                               c.finish(JobOutcome::Preempted);
                             } = state<Idle>,
        state<Running> + event<Preempt>[!automatic] / ignorePreempt,
        state<Idle> + event<Preempt> / ignorePreempt);
  }
};
} // namespace worker_lifecycle
class WorkerLifecycle {
  worker_lifecycle::Context context_;
  boost::sml::sm<worker_lifecycle::Table> machine_;

public:
  explicit WorkerLifecycle(WorkerEffects& effects) : context_{{}, effects}, machine_(context_) {}
  WorkerLifecycle(const WorkerLifecycle&) = delete;
  WorkerLifecycle& operator=(const WorkerLifecycle&) = delete;
  template <class Event> bool process(const Event& event) { return machine_.process_event(event); }
  WorkerSnapshot snapshot() const {
    auto result = context_.data;
    result.running = machine_.is(boost::sml::state<worker_lifecycle::Running>);
    return result;
  }
  bool active(uint64_t id, uint64_t now) const {
    const auto s = snapshot();
    return s.running && id == s.jobId && now < s.deadline;
  }
  uint64_t submit(uint64_t now, uint64_t budget, JobOrigin origin = JobOrigin::User) {
    return process(worker_lifecycle::Submit{now, budget, origin}) ? context_.data.jobId : 0;
  }
};
inline bool workerInvariant(const WorkerSnapshot& s) {
  // Preemption only ever ends an automatic read.
  if (s.lastOutcome == JobOutcome::Preempted && s.lastOrigin != JobOrigin::Automatic)
    return false;
  return (s.running ? s.jobId != 0 && s.deadline > s.startedAt && s.accepted == s.completed + 1
                    : s.jobId == 0 && s.startedAt == 0 && s.deadline == 0 &&
                          s.origin == JobOrigin::User && s.accepted == s.completed) &&
         s.preempted <= s.completed;
}
} // namespace surface::device
