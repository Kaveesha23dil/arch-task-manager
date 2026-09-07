#pragma once

#include <optional>
#include <sys/types.h>

namespace atm {

/// Outcome of a process-control operation.
enum class ActionStatus {
  Success,
  InvalidPid,        // pid <= 0, rejected before any system call
  ProcessNotFound,   // ESRCH: no such process (e.g. it exited meanwhile)
  PermissionDenied,  // EPERM / EACCES: caller may not control the process
  InvalidPriority,   // priority outside [-20, 19]
  Failed,            // any other operating-system error
};

/// Result of one process-control call. `errno_value` is meaningful whenever
/// `status` reflects a failed system call and carries the original errno.
struct ActionResult {
  ActionStatus status = ActionStatus::Failed;
  int errno_value = 0;

  [[nodiscard]] bool success() const { return status == ActionStatus::Success; }
};

/// Short, user-facing description of an ActionStatus. Callers should add
/// process/action context on top of this phrase.
[[nodiscard]] const char *actionStatusMessage(ActionStatus status);

/**
 * True when a pid must not be managed from this interface: PID 0, PID 1, or
 * the Task Manager's own PID (via getpid()). PID 1 and pid 0 are the kernel
 * idle/scheduler entries, and sending signals to itself would kill the
 * monitor.
 */
[[nodiscard]] bool isProtectedPid(::pid_t pid);

/**
 * Process-control operations. Thin wrappers around kill(2) and
 * setpriority(2) — no shell commands are ever executed.
 *
 * Safety policy (protection of PID 0/1 and the monitor's own PID) is
 * intentionally not enforced inside these wrappers so the class stays a
 * faithful syscall mapping; callers should gate with isProtectedPid().
 */
class ProcessActions {
 public:
  ProcessActions() = default;
  ~ProcessActions() = default;

  // Stateless wrapper object; copy/move are harmless.
  ProcessActions(const ProcessActions &) = default;
  ProcessActions &operator=(const ProcessActions &) = default;

  /// Graceful termination: kill(pid, SIGTERM).
  [[nodiscard]] ActionResult terminate(::pid_t pid);

  /// Forced termination: kill(pid, SIGKILL). Callers must confirm first.
  [[nodiscard]] ActionResult kill(::pid_t pid);

  /// Suspend: kill(pid, SIGSTOP).
  [[nodiscard]] ActionResult pause(::pid_t pid);

  /// Resume a suspended ("stopped") process: kill(pid, SIGCONT).
  [[nodiscard]] ActionResult resume(::pid_t pid);

  /**
   * Set the scheduling niceness: setpriority(PRIO_PROCESS, pid, priority).
   *
   * `priority` must lie in [-20, 19]; lower values mean higher scheduling
   * priority, higher values mean lower priority. Increasing priority (moving
   * the value below the caller's hard nice limit, usually 0 for unprivileged
   * users, or below another process's nice value) requires privileges and
   * will fail with EPERM/EACCES for a normal user.
   */
  [[nodiscard]] ActionResult setPriority(::pid_t pid, int priority);

  /// Current niceness of pid (getpriority), or std::nullopt on error.
  [[nodiscard]] std::optional<int> currentPriority(::pid_t pid) const;
};

}  // namespace atm