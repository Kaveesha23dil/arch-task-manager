// GNU extensions (sched_getaffinity/sched_setaffinity and the dynamic
// CPU_ALLOC/CPU_*_S mask macros) are required; enable them before any header.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "process_scheduling.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fstream>
#include <sched.h>
#include <set>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <unistd.h>

#include "logger.hpp"
#include "process_resources.hpp"

namespace atm {

namespace {

/// Reads a whole file into a string, or std::nullopt when it cannot be opened
/// or read through (process exited / access denied).
std::optional<std::string> readFile(std::string_view path) {
  std::ifstream in{std::string(path)};
  if (!in.is_open()) {
    return std::nullopt;
  }
  std::ostringstream contents;
  contents << in.rdbuf();
  if (in.bad()) {
    return std::nullopt;
  }
  return contents.str();
}

/// Parses a leading (optionally signed) integer with std::from_chars.
bool parseInt(std::string_view view, int &out) {
  if (view.empty()) {
    return false;
  }
  const char *begin = view.data();
  const char *end = view.data() + view.size();
  const auto result = std::from_chars(begin, end, out);
  return result.ec == std::errc{} && result.ptr == end;
}

/// Number of CPUs the kernel is configured for (used only to size the mask).
long configuredCpuCount() {
  const long count = ::sysconf(_SC_NPROCESSORS_CONF);
  return count > 0 ? count : 1;
}

/// Maps an errno from a failed scheduling syscall to a SchedulingStatus.
SchedulingResult schedulingError(int errno_value) {
  switch (errno_value) {
    case ESRCH:
      return {SchedulingStatus::ProcessNotFound, errno_value};
    case EPERM:
    case EACCES:
      return {SchedulingStatus::PermissionDenied, errno_value};
    case EINVAL:
      return {SchedulingStatus::InvalidCpuset, errno_value};
    default:
      return {SchedulingStatus::Failed, errno_value};
  }
}

}  // namespace

const char *schedulingStatusMessage(SchedulingStatus status) {
  switch (status) {
    case SchedulingStatus::Success:
      return "Scheduling change applied.";
    case SchedulingStatus::InvalidPid:
      return "Invalid PID.";
    case SchedulingStatus::ProcessNotFound:
      return "Process does not exist.";
    case SchedulingStatus::PermissionDenied:
      return "Permission denied. You do not have permission to change this "
             "process's scheduling.";
    case SchedulingStatus::ProcessReused:
      return "The process identity changed (the PID was reused by another "
             "process); no change was applied.";
    case SchedulingStatus::IdentityUnknown:
      return "The process no longer exists.";
    case SchedulingStatus::InvalidNice:
      return "Invalid nice value. Choose a value between -20 and 19.";
    case SchedulingStatus::InvalidCpu:
      return "Invalid CPU id.";
    case SchedulingStatus::EmptyAffinity:
      return "At least one CPU must remain selected.";
    case SchedulingStatus::InvalidCpuset:
      return "The kernel rejected the CPU set.";
    case SchedulingStatus::Failed:
      return "The scheduling change failed.";
  }
  return "Unknown error.";
}

std::optional<ProcessIdentity> ProcessIdentity::current(::pid_t pid) {
  if (pid <= 0) {
    return std::nullopt;
  }
  const std::optional<std::string> stat = readFile("/proc/" + std::to_string(pid) + "/stat");
  if (!stat) {
    return std::nullopt;  // process gone
  }
  const std::optional<ProcessStatParse> data = parseProcessStat(*stat);
  if (!data) {
    return std::nullopt;  // malformed stat
  }
  return ProcessIdentity{pid, data->starttime_ticks};
}

std::string formatCpuList(const std::vector<int> &cpus) {
  std::ostringstream out;
  for (std::size_t i = 0; i < cpus.size(); ++i) {
    if (i != 0) {
      out << ',';
    }
    out << cpus[i];
  }
  return out.str();
}

std::optional<std::vector<int>> parseCpuSelection(std::string_view input,
                                                  int max_cpu) {
  std::vector<std::string> tokens;
  {
    std::string current;
    for (const char c : input) {
      if (c == ',' || c == ' ' || c == '\t') {
        if (!current.empty()) {
          tokens.push_back(current);
          current.clear();
        }
      } else {
        current.push_back(c);
      }
    }
    if (!current.empty()) {
      tokens.push_back(current);
    }
  }

  std::set<int> selected;
  for (const std::string &token : tokens) {
    const std::size_t dash = token.find('-');
    if (dash == std::string::npos) {
      int cpu = 0;
      if (!parseInt(token, cpu) || cpu < 0 || cpu >= max_cpu) {
        return std::nullopt;
      }
      selected.insert(cpu);
      continue;
    }
    // A single dash token is malformed; require N-M with N <= M.
    const std::string lo_text = token.substr(0, dash);
    const std::string hi_text = token.substr(dash + 1);
    if (lo_text.empty() || hi_text.empty() ||
        hi_text.find('-') != std::string::npos) {
      return std::nullopt;
    }
    int lo = 0;
    int hi = 0;
    if (!parseInt(lo_text, lo) || !parseInt(hi_text, hi) || lo < 0 ||
        hi < lo || hi >= max_cpu) {
      return std::nullopt;
    }
    for (int cpu = lo; cpu <= hi; ++cpu) {
      selected.insert(cpu);
    }
  }

  if (selected.empty()) {
    return std::nullopt;  // never produce an empty mask
  }
  return std::vector<int>(selected.begin(), selected.end());
}

std::optional<int> ProcessSchedulingManager::getNice(::pid_t pid) const {
  errno = 0;
  const int value = ::getpriority(PRIO_PROCESS, pid);
  // getpriority() returns -1 both for a genuine niceness of -1 and for an
  // error; errno alone disambiguates the two.
  if (value == -1 && errno != 0) {
    return std::nullopt;
  }
  return value;
}

SchedulingResult ProcessSchedulingManager::setNice(
    const ProcessIdentity &identity, int nice) {
  if (identity.pid <= 0) {
    return {SchedulingStatus::InvalidPid, 0};
  }
  if (nice < kMinimumNice || nice > kMaximumNice) {
    return {SchedulingStatus::InvalidNice, 0};
  }

  // Identity check: never modify a PID that was reused by another process.
  const std::optional<ProcessIdentity> current =
      ProcessIdentity::current(identity.pid);
  if (!current) {
    return {SchedulingStatus::IdentityUnknown, 0};
  }
  if (current->starttime_ticks != identity.starttime_ticks) {
    return {SchedulingStatus::ProcessReused, 0};
  }

  // Avoid the syscall when the value is already in effect.
  const std::optional<int> existing = getNice(identity.pid);
  if (existing && *existing == nice) {
    Logger::info("Nice request skipped: PID " + std::to_string(identity.pid) +
                 " already has nice " + std::to_string(nice));
    return {SchedulingStatus::Success, 0};
  }

  Logger::info("Nice change requested PID: " + std::to_string(identity.pid) +
               " Current nice: " + std::to_string(existing.value_or(-1)) +
               " New nice: " + std::to_string(nice));

  if (::setpriority(PRIO_PROCESS, identity.pid, nice) == 0) {
    Logger::info("Nice changed successfully (PID " +
                 std::to_string(identity.pid) + " to " + std::to_string(nice) +
                 ")");
    return {SchedulingStatus::Success, 0};
  }
  const SchedulingResult result = schedulingError(errno);
  Logger::warn("Nice change failed (PID " + std::to_string(identity.pid) +
               "): " + std::strerror(errno));
  return result;
}

std::optional<std::vector<int>>
ProcessSchedulingManager::getCpuAffinity(::pid_t pid) const {
  if (pid <= 0) {
    return std::nullopt;
  }
  const long configured = configuredCpuCount();
  std::size_t setsize = CPU_ALLOC_SIZE(configured);
  cpu_set_t *mask = CPU_ALLOC(configured);
  if (mask == nullptr) {
    return std::nullopt;  // ENOMEM-sized allocation failure
  }
  CPU_ZERO_S(setsize, mask);
  const int result = ::sched_getaffinity(pid, setsize, mask);
  if (result != 0) {
    CPU_FREE(mask);
    return std::nullopt;  // ESRCH / EPERM / EINVAL / EFAULT
  }
  std::vector<int> cpus;
  for (int cpu = 0; cpu < configured; ++cpu) {
    if (CPU_ISSET_S(cpu, setsize, mask)) {
      cpus.push_back(cpu);
    }
  }
  CPU_FREE(mask);
  return cpus;
}

SchedulingResult ProcessSchedulingManager::setCpuAffinity(
    const ProcessIdentity &identity, const std::vector<int> &cpus) {
  if (identity.pid <= 0) {
    return {SchedulingStatus::InvalidPid, 0};
  }
  if (cpus.empty()) {
    return {SchedulingStatus::EmptyAffinity, 0};
  }

  // Validate every CPU id before the syscall (never pass an empty or
  // out-of-range mask to the kernel).
  std::vector<int> selected = cpus;
  std::sort(selected.begin(), selected.end());
  selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
  const int online = systemCpuCount();
  for (const int cpu : selected) {
    if (cpu < 0 || cpu >= online) {
      return {SchedulingStatus::InvalidCpu, 0};
    }
  }

  // Identity check: never modify a PID that was reused by another process.
  const std::optional<ProcessIdentity> current =
      ProcessIdentity::current(identity.pid);
  if (!current) {
    return {SchedulingStatus::IdentityUnknown, 0};
  }
  if (current->starttime_ticks != identity.starttime_ticks) {
    return {SchedulingStatus::ProcessReused, 0};
  }

  // Avoid the syscall when the requested mask is already in effect.
  const std::optional<std::vector<int>> existing = getCpuAffinity(identity.pid);
  if (existing && *existing == selected) {
    Logger::info("CPU affinity request skipped: PID " +
                 std::to_string(identity.pid) + " already runs on " +
                 formatCpuList(selected));
    return {SchedulingStatus::Success, 0};
  }

  Logger::info("CPU affinity change requested PID: " +
               std::to_string(identity.pid) +
               " Current CPUs: " +
               (existing ? formatCpuList(*existing) : std::string("N/A")) +
               " New CPUs: " + formatCpuList(selected));

  const long configured = configuredCpuCount();
  std::size_t setsize = CPU_ALLOC_SIZE(configured);
  cpu_set_t *mask = CPU_ALLOC(configured);
  if (mask == nullptr) {
    return {SchedulingStatus::Failed, ENOMEM};
  }
  CPU_ZERO_S(setsize, mask);
  for (const int cpu : selected) {
    CPU_SET_S(cpu, setsize, mask);
  }
  const int result = ::sched_setaffinity(identity.pid, setsize, mask);
  CPU_FREE(mask);
  if (result == 0) {
    Logger::info("CPU affinity changed successfully (PID " +
                 std::to_string(identity.pid) + " to " +
                 formatCpuList(selected) + ")");
    return {SchedulingStatus::Success, 0};
  }
  const SchedulingResult error = schedulingError(errno);
  Logger::warn("CPU affinity change failed (PID " +
               std::to_string(identity.pid) +
               "): " + std::strerror(errno));
  return error;
}

int ProcessSchedulingManager::systemCpuCount() {
  const long count = ::sysconf(_SC_NPROCESSORS_ONLN);
  return count > 0 ? static_cast<int>(count) : 1;
}

}  // namespace atm