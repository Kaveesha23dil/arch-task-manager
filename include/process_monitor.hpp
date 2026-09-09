#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "process_resources.hpp"

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

  // Process identity: the start-time tick counter from /proc/<pid>/stat. It
  // distinguishes a given PID across a restart/reuse, so historical deltas
  // (CPU and I/O rates) are never computed against a different process.
  std::uint64_t starttime_ticks = 0;

  // Resident-shared portion of RSS from /proc/<pid>/status (RssShmem +
  // RssFile), in kB. Zero when the kernel does not report it.
  std::uint64_t shared_memory_kib = 0;

  // I/O counters and throughput from /proc/<pid>/io. `io_available` is false
  // when the counters could not be read (Linux permissions / process gone);
  // rates are bytes-per-second deltas over the last scan window.
  bool io_available = false;
  std::uint64_t read_bytes = 0;
  std::uint64_t write_bytes = 0;
  std::uint64_t read_syscalls = 0;
  std::uint64_t write_syscalls = 0;
  double read_rate = 0.0;
  double write_rate = 0.0;
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
  Cpu,        // CPU usage, descending (default)
  Memory,     // resident memory, descending
  Pid,        // PID, ascending
  Name,       // name, ascending
  Threads,    // thread count, descending
  ReadRate,   // I/O read rate, descending
  WriteRate,  // I/O write rate, descending
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
 *
 * Per-process I/O rates are computed the same way: the previous /proc/<pid>/io
 * counters are diffed against the current ones. Because Linux PIDs can be
 * reused, historical deltas are keyed by the process identity (PID + process
 * start time); when the start time differs the baseline is reset so a reused
 * PID never produces a spurious huge rate.
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
  /// Identity of one process used to gate historical deltas (PID + starttime).
  struct Identity {
    int pid = 0;
    std::uint64_t starttime_ticks = 0;

    bool operator==(const Identity &) const = default;
  };

  /// Empty specialization so Identity works as an unordered_map key.
  struct IdentityHash {
    std::size_t operator()(const Identity &identity) const {
      auto h1 = std::hash<int>{}(identity.pid);
      auto h2 = std::hash<std::uint64_t>{}(identity.starttime_ticks);
      // Combine without reordering collisions: shift one field first.
      return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
  };

  std::unordered_map<int, std::uint64_t> previous_ticks_;
  std::optional<std::uint64_t> previous_total_ticks_;

  // Previous scan's I/O per process identity, used to derive rates.
  std::unordered_map<Identity, ProcessIoCounters, IdentityHash> previous_io_;
  std::chrono::steady_clock::time_point previous_scan_time_;
  bool has_previous_scan_ = false;
};

}  // namespace atm