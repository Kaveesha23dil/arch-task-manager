#include "process_actions.hpp"

#include <cerrno>
#include <csignal>
#include <sys/resource.h>
#include <unistd.h>

namespace atm {

namespace {

/// Maps errno from a failed kill(2) to an ActionStatus, preserving the raw
/// errno for context.
ActionResult signalResult() {
  ActionResult result;
  result.errno_value = errno;
  switch (errno) {
    case ESRCH:
      result.status = ActionStatus::ProcessNotFound;
      break;
    case EPERM:
      result.status = ActionStatus::PermissionDenied;
      break;
    default:
      result.status = ActionStatus::Failed;  // EINVAL (bad signal), etc.
      break;
  }
  return result;
}

}  // namespace

const char *actionStatusMessage(ActionStatus status) {
  switch (status) {
    case ActionStatus::Success:
      return "Operation completed.";
    case ActionStatus::InvalidPid:
      return "Invalid PID.";
    case ActionStatus::ProcessNotFound:
      return "Process does not exist.";
    case ActionStatus::PermissionDenied:
      return "Permission denied. You do not have permission to control this "
             "process.";
    case ActionStatus::InvalidPriority:
      return "Invalid priority. Choose a value between -20 and 19.";
    case ActionStatus::Failed:
      return "The operation failed.";
  }
  return "Unknown error.";
}

bool isProtectedPid(::pid_t pid) {
  return pid <= 1 || pid == ::getpid();
}

ActionResult ProcessActions::terminate(::pid_t pid) {
  if (pid <= 0) {
    return {ActionStatus::InvalidPid, 0};
  }
  if (::kill(pid, SIGTERM) == 0) {
    return {ActionStatus::Success, 0};
  }
  return signalResult();
}

ActionResult ProcessActions::kill(::pid_t pid) {
  if (pid <= 0) {
    return {ActionStatus::InvalidPid, 0};
  }
  if (::kill(pid, SIGKILL) == 0) {
    return {ActionStatus::Success, 0};
  }
  return signalResult();
}

ActionResult ProcessActions::pause(::pid_t pid) {
  if (pid <= 0) {
    return {ActionStatus::InvalidPid, 0};
  }
  if (::kill(pid, SIGSTOP) == 0) {
    return {ActionStatus::Success, 0};
  }
  return signalResult();
}

ActionResult ProcessActions::resume(::pid_t pid) {
  if (pid <= 0) {
    return {ActionStatus::InvalidPid, 0};
  }
  if (::kill(pid, SIGCONT) == 0) {
    return {ActionStatus::Success, 0};
  }
  return signalResult();
}

ActionResult ProcessActions::setPriority(::pid_t pid, int priority) {
  if (pid <= 0) {
    return {ActionStatus::InvalidPid, 0};
  }
  if (priority < -20 || priority > 19) {
    return {ActionStatus::InvalidPriority, 0};
  }
  if (::setpriority(PRIO_PROCESS, pid, priority) == 0) {
    return {ActionStatus::Success, 0};
  }

  ActionResult result;
  result.errno_value = errno;
  switch (errno) {
    case ESRCH:
      result.status = ActionStatus::ProcessNotFound;
      break;
    case EPERM:
    case EACCES:
      result.status = ActionStatus::PermissionDenied;
      break;
    case EINVAL:
      result.status = ActionStatus::InvalidPriority;
      break;
    default:
      result.status = ActionStatus::Failed;
      break;
  }
  return result;
}

std::optional<int> ProcessActions::currentPriority(::pid_t pid) const {
  errno = 0;
  const int value = ::getpriority(PRIO_PROCESS, pid);
  // getpriority() returns -1 both for niceness -1 and for errors; errno
  // alone disambiguates.
  if (value == -1 && errno != 0) {
    return std::nullopt;
  }
  return value;
}

}  // namespace atm