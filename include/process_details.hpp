#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "process_monitor.hpp"
#include "process_resources.hpp"

namespace atm {

/**
 * A point-in-time detailed snapshot of one process, read directly from its
 * /proc/<pid> entries.
 *
 * This deliberately reuses the existing ProcessMonitor types where possible
 * rather than duplicating the process table: the name and state are carried as
 * the same ProcessState / state-character as Process, and the CPU / memory
 * percentages are those computed by the Process Monitor (or a safe fallback)
 * rather than a second, independent monitoring system.
 *
 * Values that could not be read — because the file is missing, malformed, or
 * access is denied by Linux permissions — are left as std::nullopt or 0 and
 * rendered as "N/A" by the front-end. Reading a process never throws and never
 * crashes even when the process exits mid-inspection.
 */
struct ProcessDetailsInfo {
  pid_t pid = 0;

  std::string name;             // comm from /proc/<pid>/stat
  char state_char = '?';        // raw state char (e.g. 'R')
  ProcessState state = ProcessState::Unknown;
  std::string user;             // resolved from the real UID via getpwuid_r
  pid_t parent_pid = 0;
  std::string executable_path;  // /proc/<pid>/exe, or the (deleted) marker
  std::string working_directory;  // /proc/<pid>/cwd
  std::string command_line;     // argv from /proc/<pid>/cmdline

  std::optional<uid_t> uid;     // real UID from /proc/<pid>/status
  std::optional<gid_t> gid;     // real GID from /proc/<pid>/status

  std::optional<std::uint32_t> thread_count;
  std::optional<int> priority;  // /proc/<pid>/stat priority field
  std::optional<int> nice_value;
  std::optional<int> nice_priority;  // niceness via getpriority(2), when read

  // Memory. VmSize / VmRSS are always knife-reliable; the finer split
  // (shared, text, data, stack) is best-effort and may be optional.
  std::uint64_t virtual_memory_bytes = 0;   // VmSize
  std::uint64_t resident_memory_bytes = 0;  // VmRSS
  std::optional<std::uint64_t> shared_memory_bytes;
  std::optional<std::uint64_t> text_memory_bytes;
  std::optional<std::uint64_t> data_memory_bytes;
  std::optional<std::uint64_t> stack_memory_bytes;
  std::optional<double> memory_percent;  // RSS / system total RAM × 100

  // CPU time in USER_HZ ticks.
  std::uint64_t user_cpu_time = 0;    // utime
  std::uint64_t system_cpu_time = 0;  // stime
  std::uint64_t total_cpu_time = 0;   // utime + stime
  std::optional<double> cpu_usage_percent;

  // I/O from /proc/<pid>/io. Not readable for every process (see the module
  // docstring); each value is therefore optional and shown as "N/A" when the
  // kernel or permissions withhold it.
  std::optional<std::uint64_t> read_bytes;
  std::optional<std::uint64_t> write_bytes;
  std::optional<std::uint64_t> read_syscalls;
  std::optional<std::uint64_t> write_syscalls;
  std::optional<std::uint64_t> cancelled_write_bytes;

  // I/O throughput in bytes/second, derived as the delta between this
  // inspection and the previous one on the same process identity (PID + start
  // time). 0 when the counters are unavailable or the identity changed.
  double read_rate = 0.0;
  double write_rate = 0.0;

  // Context switches from /proc/<pid>/status (best-effort, optional).
  std::optional<std::uint64_t> voluntary_context_switches;
  std::optional<std::uint64_t> nonvoluntary_context_switches;

  // Read-only resource limits parsed from /proc/<pid>/limits. Values are
  // intentionally never modified by the application.
  ProcessResourceLimits limits;

  // Process identity: the kernel start-time tick from /proc/<pid>/stat, used
  // by the scheduling editor to verify the selected process is still the same
  // one before applying a change (guards against PID reuse).
  std::optional<std::uint64_t> starttime_ticks;

  // Scheduling state, read-only observation via ProcessSchedulingManager.
  // `allowed_cpus` is the process's current CPU affinity from
  // sched_getaffinity(2); `system_cpu_count` is the online CPU count used to
  // validate/format the allowed list. Neither is ever modified by the
  // collector.
  std::optional<std::vector<int>> allowed_cpus;
  int system_cpu_count = 0;

  // Start wall-clock time derived from the starttime tick, boot uptime and the
  // current clock. process_uptime_seconds is how long the process has been
  // alive. Both are nullopt when the tick value cannot be resolved.
  std::optional<std::chrono::system_clock::time_point> start_time;
  std::optional<std::uint64_t> process_uptime_seconds;
};

/**
 * Collects detailed information about a single process from its /proc/<pid>
 * directory.
 *
 * The collector is stateless and strictly read-only: it never sends signals,
 * never writes anything, and never executes shell commands. It repeatedly
 * copes with the process disappearing mid-read (a file vanishes between open
 * and read), permission-denied files, malformed lines and broken /proc links
 * — every one of those degrades to an "N/A" field instead of a crash. It never
 * reads /proc/<pid>/environ and never exposes environment variables.
 */
class ProcessDetails {
 public:
  ProcessDetails() = default;
  ~ProcessDetails() = default;

  // Stateless reader; copy/move are harmless.
  ProcessDetails(const ProcessDetails &) = delete;
  ProcessDetails &operator=(const ProcessDetails &) = delete;

  /**
   * Reads a detailed snapshot of `pid`.
   *
   * `system_total_kib` is the total system RAM in kB (from MemoryMonitor) and
   * is used only to compute the process's memory percentage, mirroring
   * ProcessMonitor::read. `cpu_percent`, when provided, is reused from the
   * Process Monitor's last scan so the detail view shows the same CPU figure
   * as the table instead of building a second CPU tracker.
   *
   * The collector keeps the previous /proc/<pid>/io sample per process
   * identity (PID + start time) so I/O rates update across repeated
   * inspections of the same process and reset on a PID restart/reuse.
   *
   * Returns std::nullopt when /proc/<pid> no longer exists (the process has
   * exited) or yields no recognisable identity at all. Never throws.
   */
  [[nodiscard]] std::optional<ProcessDetailsInfo>
  getProcessDetails(pid_t pid, std::uint64_t system_total_kib,
                    std::optional<double> cpu_percent = std::nullopt);

  /// Called when a process is no longer being inspected, discarding its I/O
  /// baseline so a later inspection starts fresh.
  void forgetBaseline(pid_t pid);

 private:
  /// Identity of one process used to gate I/O-rate deltas.
  struct Identity {
    pid_t pid = 0;
    std::uint64_t starttime_ticks = 0;
    bool operator==(const Identity &) const = default;
  };
  struct IdentityHash {
    std::size_t operator()(const Identity &identity) const {
      return std::hash<pid_t>{}(identity.pid) ^
             (std::hash<std::uint64_t>{}(identity.starttime_ticks) +
              0x9e3779b97f4a7c15ULL);
    }
  };

  std::unordered_map<Identity, ProcessIoCounters, IdentityHash> previous_io_;
  std::chrono::steady_clock::time_point previous_scan_time_;
  bool has_previous_scan_ = false;
};

}  // namespace atm
