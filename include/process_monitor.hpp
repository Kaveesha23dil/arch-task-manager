#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace atm {

/// Execution state of a process, mapped from the single-character state field
/// of /proc/<pid>/stat. Unknown covers unreadable or unrecognized characters.
enum class ProcessState {
  Unknown,
  Running,
  Sleeping,
  DiskSleep,
  Stopped,
  Zombie,
  Idle,
};

/**
 * A single point-in-time snapshot of one process, read from /proc/<pid>.
 *
 * All raw values keep their reported units: memory in kB (kibibytes), CPU
 * times in USER_HZ ticks. Derived percentages (`cpu_percent`,
 * `memory_percent`) are dimensionless and ready for display.
 */
struct Process {
  int pid = 0;
  int parent_pid = 0;
  std::string name;            // comm from /proc/<pid>/stat
  char state_char = '?';       // raw state char (e.g. 'R') from /proc/<pid>/stat
  ProcessState state = ProcessState::Unknown;
  std::uint32_t uid = 0;       // real UID from /proc/<pid>/status
  std::uint32_t thread_count = 0;  // threads from status (or stat, as fallback)
  std::uint64_t memory_kib = 0;    // VmRSS from /proc/<pid>/status, in kB
  double memory_percent = 0.0;     // RSS / total system RAM × 100
  std::uint64_t cpu_ticks = 0;     // utime + stime, in USER_HZ ticks
  double cpu_percent = 0.0;        // usage since the previous scan
  std::string command_line;        // argv from cmdline; empty for kernel threads
};

/// Maps a /proc/s state character (e.g. 'R', 'S', 'D') to a ProcessState.
[[nodiscard]] ProcessState processStateFromChar(char state);

/// Human-readable label for a process state (e.g. "Running").
[[nodiscard]] const char *processStateName(ProcessState state);

/// Aggregate counters for the process population of one scan.
struct ProcessStats {
  std::size_t total = 0;
  std::size_t running = 0;
  std::size_t sleeping = 0;
  std::size_t stopped = 0;
  std::size_t zombie = 0;
};

/// Result of one full scan of /proc.
struct ProcessSnapshot {
  std::vector<Process> processes;
  ProcessStats stats;
};

/// Sort key for the process table.
enum class ProcessSort {
  Cpu,     // CPU usage, descending (default)
  Memory,  // resident memory, descending
  Pid,     // PID, ascending
  Name,    // name, ascending
};

/// Sorts `processes` in place according to `sort`.
void sortProcesses(std::vector<Process> &processes, ProcessSort sort);

/// Short label of a sort key, for the on-screen hint.
[[nodiscard]] const char *processSortName(ProcessSort sort);

/**
 * Monitors the process table by scanning /proc once per refresh.
 *
 * The object keeps the previous scan's CPU sample (per-process ticks and the
 * system-wide total), so each read reports per-process CPU usage as the delta
 * over the elapsed window. The first read only records a baseline and reports
 * 0.0% for every process.
 */
class ProcessMonitor {
 public:
  ProcessMonitor() = default;
  ~ProcessMonitor() = default;

  // Monitors hold sample state; copying/moving one would duplicate baselines.
  ProcessMonitor(const ProcessMonitor &) = delete;
  ProcessMonitor &operator=(const ProcessMonitor &) = delete;

  /**
   * Scans /proc and returns a snapshot of currently visible processes.
   *
   * `system_total_kib` is the current total RAM in kB (from MemoryMonitor);
   * it is used only to compute each process's memory percentage. Processes
   * that terminate or become unreadable mid-scan are silently skipped rather
   * than crashing the program.
   */
  [[nodiscard]] ProcessSnapshot read(std::uint64_t system_total_kib);

 private:
  std::unordered_map<int, std::uint64_t> previous_ticks_;
  std::optional<std::uint64_t> previous_total_ticks_;
};

}  // namespace atm