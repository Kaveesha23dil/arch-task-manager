#pragma once

#include <cstdint>
#include <optional>

namespace atm {

/**
 * Raw CPU time counters from the aggregate "cpu" line of /proc/stat.
 *
 * Every value counts CPU ticks spent in that state since boot, in units of
 * USER_HZ (normally 1/100 of a second). Newer kernels append additional
 * fields ("guest", "guest_nice") which are intentionally not captured here.
 */
struct CpuTimes {
  std::uint64_t user = 0;
  std::uint64_t nice = 0;
  std::uint64_t system = 0;
  std::uint64_t idle = 0;
  std::uint64_t iowait = 0;
  std::uint64_t irq = 0;
  std::uint64_t softirq = 0;
  std::uint64_t steal = 0;

  /// Total ticks across all states. Sum of all captured counters.
  [[nodiscard]] std::uint64_t total() const;

  /// Idle-like ticks. Both `idle` and `iowait` classify as idle because
  /// iowait is time the CPU spends idle while tasks wait on I/O.
  [[nodiscard]] std::uint64_t idleTime() const;

  /// Busy ticks: everything that is not idle-like.
  [[nodiscard]] std::uint64_t busy() const;
};

/**
 * Reads and parses the aggregate CPU line from /proc/stat.
 *
 * Returns std::nullopt when the file cannot be opened, contains no "cpu "
 * (aggregate) line, or the line is malformed. All-zero "guest"/"guest_nice"
 * columns beyond `steal` are ignored.
 */
[[nodiscard]] std::optional<CpuTimes> readCpuTimes();

/**
 * Computes overall CPU utilization in percent (0.0 - 100.0) between two
 * samples taken from /proc/stat.
 *
 * Returns std::nullopt if the delta is not computable: zero elapsed ticks
 * (samples taken with no progress of the counters) or a counter reset
 * (previous sample reports more ticks than the current one, e.g. after a VM
 * migration or counter overflow).
 */
[[nodiscard]] std::optional<double> cpuUtilization(const CpuTimes &previous,
                                                   const CpuTimes &current);

/**
 * Monitors overall CPU utilization by continuously comparing successive
 * samples from /proc/stat.
 *
 * The object keeps the previous sample; each read yields the utilization of
 * the elapsed window compared to that sample. The first read only records a
 * baseline and returns std::nullopt.
 */
class CpuMonitor {
 public:
  CpuMonitor() = default;
  ~CpuMonitor() = default;

  // Monitors hold sample state; copying/moving one would duplicate baselines.
  CpuMonitor(const CpuMonitor &) = delete;
  CpuMonitor &operator=(const CpuMonitor &) = delete;

  /// Takes a fresh /proc/stat sample and returns the utilization since the
  /// previous sample, or std::nullopt on a read/parse error (or when no
  /// baseline sample exists yet, i.e. the very first call).
  [[nodiscard]] std::optional<double> readUsage();

 private:
  std::optional<CpuTimes> previous_;
};

}  // namespace atm