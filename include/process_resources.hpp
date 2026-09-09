#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace atm {

/**
 * One read-only resource limit for a process, from /proc/<pid>/limits.
 *
 * Linux reports a limit that is either a concrete non-negative value or the
 * literal "unlimited". "unlimited" is represented by the explicit *_unlimited
 * flags rather than a sentinel value (e.g. RLIM_INFINITY = 2^64-1), so the
 * UI can render "Unlimited" instead of exposing an enormous integer. A limit
 * that could not be parsed at all leaves soft/hard as std::nullopt.
 */
struct ResourceLimit {
  std::optional<std::uint64_t> soft;
  std::optional<std::uint64_t> hard;
  bool soft_unlimited = false;
  bool hard_unlimited = false;

  /// True when neither the soft nor hard limit is usable.
  [[nodiscard]] bool empty() const {
    return !soft.has_value() && !hard.has_value() && !soft_unlimited &&
           !hard_unlimited;
  }
};

/**
 * The resource-limit block of one process, populated from its
 * /proc/<pid>/limits file. Every field is optional: a limit only present when
 * the kernel reported it and it could be parsed reliably.
 */
struct ProcessResourceLimits {
  std::optional<ResourceLimit> open_files;          // "Max open files"
  std::optional<ResourceLimit> max_processes;       // "Max processes"
  std::optional<ResourceLimit> max_stack_size;      // "Max stack size"
  std::optional<ResourceLimit> locked_memory;       // "Max locked memory"
  std::optional<ResourceLimit> address_space;       // "Max address space"
  std::optional<ResourceLimit> core_file_size;      // "Max core file size"
  std::optional<ResourceLimit> pending_signals;     // "Max pending signals"
  std::optional<ResourceLimit> posix_message_queues;  // "Max POSIX message queues"
  std::optional<ResourceLimit> realtime_priority;   // "Max realtime priority"
  std::optional<ResourceLimit> realtime_timeout;    // "Max realtime timeout"

  /// True when no limit was parsed at all.
  [[nodiscard]] bool empty() const {
    return !open_files && !max_processes && !max_stack_size && !locked_memory &&
           !address_space && !core_file_size && !pending_signals &&
           !posix_message_queues && !realtime_priority && !realtime_timeout;
  }
};

/**
 * Parses the resource limits from the raw text of a /proc/<pid>/limits file.
 * Only the limits listed in ProcessResourceLimits are picked out; unknown or
 * malformed lines are skipped. Never throws, never returns nullopt for the
 * whole set — it always returns a ProcessResourceLimits with whatever fields
 * were recognized.
 */
[[nodiscard]] ProcessResourceLimits
parseProcessLimits(std::string_view contents);

/**
 * A process's I/O counters as reported by /proc/<pid>/io.
 *
 * `available` distinguishes "the file could not be read (permissions / the
 * process vanished)" from "read successfully". The other fields default to 0
 * when the kernel did not report them.
 */
struct ProcessIoCounters {
  bool available = false;
  std::uint64_t read_bytes = 0;
  std::uint64_t write_bytes = 0;
  std::uint64_t read_syscalls = 0;
  std::uint64_t write_syscalls = 0;
  std::uint64_t cancelled_write_bytes = 0;
};

/// Parses /proc/<pid>/io text into ProcessIoCounters. Only recognizes the
/// standard keys; unknown lines are skipped. Never throws.
[[nodiscard]] ProcessIoCounters parseProcessIo(std::string_view contents);

/// I/O throughput, in bytes per second.
struct IoRates {
  double read_rate = 0.0;
  double write_rate = 0.0;
};

/**
 * Fields of /proc/<pid>/stat needed to reason about a process's CPU usage,
 * thread count and identity. `comm` is the raw comm string; `state` is the
 * single-character state; `ppid` the parent PID; `utime`/`stime` the user and
 * system CPU times in USER_HZ ticks; `num_threads` the kernel-reported thread
 * count; `starttime_ticks` the process start-time tick counter (part of the
 * process identity used to guard historical deltas).
 */
struct ProcessStatParse {
  std::string comm;
  char state = '?';
  int ppid = 0;
  std::uint64_t utime = 0;
  std::uint64_t stime = 0;
  std::uint32_t num_threads = 0;
  std::uint64_t starttime_ticks = 0;
};

/**
 * Parses a single /proc/<pid>/stat line. The comm field is wrapped in
 * parentheses and may itself contain spaces or parentheses, so it is located
 * between the first '(' and the last ')'. Returns std::nullopt for a truncated
 * or malformed line (no recognisable comm/fields). Never throws.
 */
[[nodiscard]] std::optional<ProcessStatParse>
parseProcessStat(std::string_view line);

/**
 * Fields of /proc/<pid>/status needed by the monitor and tests: state, real
 * UID, thread count, VmRSS (resident memory, kB), VmSize (virtual memory, kB),
 * resident shared memory (RssShmem + RssFile, kB) and the voluntary /
 * nonvoluntary context-switch counters. Missing fields keep their defaults.
 */
struct ProcessStatusParse {
  char state = '?';
  std::uint32_t uid = 0;
  std::uint32_t threads = 0;
  std::uint64_t vm_rss_kib = 0;
  std::uint64_t vm_size_kib = 0;
  std::uint64_t shared_kib = 0;
  std::optional<std::uint64_t> voluntary_context_switches;
  std::optional<std::uint64_t> nonvoluntary_context_switches;
};

/// Parses /proc/<pid>/status for the fields listed in ProcessStatusParse.
/// Unknown lines are skipped; missing values keep their defaults. Never throws.
[[nodiscard]] ProcessStatusParse parseProcessStatus(std::string_view contents);

/**
 * Computes safe I/O rates from a previous and current /proc/<pid>/io sample.
 *
 * `same_process` must be false when the PID was reused/restarted between the
 * two samples (checked by the caller via PID + process start time) or when
 * this is the first observed sample; in both cases the deltas are unknown, so
 * the rates reset to zero instead of fabricating a value. When the process is
 * the same, a decreasing counter (kernel counter reset) also yields zero for
 * that direction rather than a negative rate. `elapsed_seconds` <= 0 is
 * treated as invalid and produces zero. Never throws.
 */
[[nodiscard]] IoRates computeIoRates(bool same_process,
                                     const ProcessIoCounters &previous,
                                     const ProcessIoCounters &current,
                                     double elapsed_seconds);

}  // namespace atm
