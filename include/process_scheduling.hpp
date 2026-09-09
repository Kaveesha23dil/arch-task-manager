#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <sys/types.h>
#include <vector>

namespace atm {

/// Platform nice range. Linux exposes no named constant for the [-20, 19]
/// kernel range, so the documented values are kept here; every validation and
/// every confirmation message refers to these two.
constexpr int kMinimumNice = -20;
constexpr int kMaximumNice = 19;

/// Outcome of a process-scheduling modification.
enum class SchedulingStatus {
  Success,         // the kernel accepted the change (or nothing was needed)
  InvalidPid,      // pid <= 0, rejected before any system call
  ProcessNotFound, // ESRCH: no such process (it exited meanwhile)
  PermissionDenied, // EPERM / EACCES: caller may not change this process
  ProcessReused,   // PID was reused by a different process since selection
  IdentityUnknown, // could not re-read the process identity before applying
  InvalidNice,     // requested nice is outside [kMinimumNice, kMaximumNice]
  InvalidCpu,      // a CPU id is outside the online CPU range
  EmptyAffinity,   // an empty CPU mask was rejected
  InvalidCpuset,   // EINVAL from sched_setaffinity
  Failed,          // any other operating-system error
};

/// Result of one scheduling operation. `errno_value` carries the original
/// errno whenever a system call failed.
struct SchedulingResult {
  SchedulingStatus status = SchedulingStatus::Failed;
  int errno_value = 0;

  [[nodiscard]] bool success() const {
    return status == SchedulingStatus::Success;
  }
};

/// Short, user-facing description of a SchedulingStatus. Callers add the
/// process/operation context around this phrase.
[[nodiscard]] const char *schedulingStatusMessage(SchedulingStatus status);

/**
 * The identity of the process a scheduling change refers to. Captured from the
 * process table / inspector at selection time (PID + the kernel start-time
 * tick from /proc/<pid>/stat) and re-verified immediately before applying, so
 * a PID that was reused by a newer process never receives a change meant for
 * its predecessor.
 */
struct ProcessIdentity {
  ::pid_t pid = 0;
  std::uint64_t starttime_ticks = 0;

  /// Captures the current identity of `pid` by reading /proc/<pid>/stat.
  /// Returns std::nullopt when the process does not exist or its stat is
  /// unreadable / malformed. Never throws.
  [[nodiscard]] static std::optional<ProcessIdentity> current(::pid_t pid);
};

/// Formats a CPU-id list compactly, e.g. {0,2,3} -> "0,2,3".
[[nodiscard]] std::string formatCpuList(const std::vector<int> &cpus);

/**
 * Parses a user-supplied CPU selection such as "0,2", "0 2" or "0-3" (or any
 * mix of the three) into a sorted, deduplicated list of CPU ids. Every id must
 * satisfy 0 <= id < max_cpu (the online CPU count). Returns std::nullopt for
 * empty input, malformed tokens, out-of-range ids, or when no valid CPU
 * remains — an empty mask is never produced here.
 */
[[nodiscard]] std::optional<std::vector<int>>
parseCpuSelection(std::string_view input, int max_cpu);

/**
 * Safe, explicit scheduling controls for one process.
 *
 * This component only touches two kernel mechanisms:
 *   - CPU affinity via sched_getaffinity(2) / sched_setaffinity(2);
 *   - the normal nice value via getpriority(2) / setpriority(2, PRIO_PROCESS).
 *
 * Real-time scheduling (SCHED_FIFO/RR/DEADLINE), I/O priority, cgroups, CPU
 * quotas and persistent process rules are deliberately out of scope. No shell
 * command (taskset, renice, chrt, ...) is ever executed, no sudo/su/password
 * is used, and a denied operation is reported, never auto-elevated.
 *
 * Every modification is gated by the supplied ProcessIdentity: the identity is
 * re-read from the kernel straight before the syscall and the operation is
 * refused when the running process is not the one the user selected.
 */
class ProcessSchedulingManager {
 public:
  ProcessSchedulingManager() = default;
  ~ProcessSchedulingManager() = default;

  // Stateless wrapper object; copy/move are harmless.
  ProcessSchedulingManager(const ProcessSchedulingManager &) = default;
  ProcessSchedulingManager &operator=(const ProcessSchedulingManager &) =
      default;

  /// Current niceness of `pid` via getpriority(2), or std::nullopt on error
  /// (including "no such process", reported via errno).
  [[nodiscard]] std::optional<int> getNice(::pid_t pid) const;

  /// Sets the niceness of the selected process via setpriority(2). The value
  /// is validated, the process identity is re-checked, and no syscall is made
  /// when the value is already in effect.
  [[nodiscard]] SchedulingResult setNice(const ProcessIdentity &identity,
                                         int nice);

  /// The CPUs the process is allowed to run on, from sched_getaffinity(2), as
  /// a sorted list of CPU ids; std::nullopt when the syscall fails (process
  /// gone, permissions, ...).
  [[nodiscard]] std::optional<std::vector<int>>
  getCpuAffinity(::pid_t pid) const;

  /// Restricts the process to `cpus` via sched_setaffinity(2). The list is
  /// validated (non-empty, every id in range), the identity is re-checked, and
  /// no syscall is made when the mask is unchanged.
  [[nodiscard]] SchedulingResult
  setCpuAffinity(const ProcessIdentity &identity, const std::vector<int> &cpus);

  /// How many CPUs are online on this system (sysconf(_SC_NPROCESSORS_ONLN)).
  /// Always >= 1.
  [[nodiscard]] static int systemCpuCount();
};

}  // namespace atm