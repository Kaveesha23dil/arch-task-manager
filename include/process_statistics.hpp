#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <unordered_set>

#include "process_monitor.hpp"

namespace atm {

/**
 * Aggregated, system-wide view of the process population taken from a single
 * ProcessSnapshot. Everything here is computed in one O(N) pass over the
 * snapshot's processes; the module never scans /proc itself.
 *
 * Counts are grouped by ProcessState. "Unknown" includes processes whose
 * state character is unreadable or unrecognized (ProcessState::Unknown).
 *
 * CPU time aggregates sum the per-process USER_HZ ticks reported by
 * /proc/<pid>/stat (utime = user, stime = system). `aggregate_cpu_percent`
 * is the sum of the per-process usage percentages already computed by
 * ProcessMonitor over its last scan window.
 *
 * Memory aggregates sum the resident and resident-shared (RssShmem + RssFile)
 * kilobytes reported by /proc/<pid>/status.
 *
 * Creation/exit counts are deltas against the previous snapshot of the
 * aggregator that produced this structure; the corresponding per-second rates
 * are derived from the wall time between the two snapshots. On the very first
 * snapshot there is no baseline, so both counts and rates are zero.
 */
struct SystemProcessStatistics {
  std::size_t total = 0;

  // Process counts by state.
  std::size_t running = 0;
  std::size_t sleeping = 0;
  std::size_t disk_sleep = 0;
  std::size_t stopped = 0;
  std::size_t zombie = 0;
  std::size_t idle = 0;
  std::size_t unknown = 0;

  // Thread count aggregation.
  std::size_t total_threads = 0;
  std::size_t processes_with_thread_data = 0;

  // CPU percentage aggregation (sum of per-process percentages).
  double aggregate_cpu_percent = 0.0;
  std::size_t processes_with_cpu_data = 0;

  // CPU time aggregation (USER_HZ ticks from /proc/<pid>/stat).
  std::uint64_t total_user_cpu_ticks = 0;
  std::uint64_t total_system_cpu_ticks = 0;
  std::uint64_t total_cpu_ticks = 0;

  // Memory aggregation (kilobytes).
  std::uint64_t total_rss_kib = 0;
  std::uint64_t total_shared_kib = 0;
  double aggregate_memory_percent = 0.0;
  std::size_t processes_with_memory_data = 0;

  // I/O throughput aggregation (bytes per second).
  double total_read_rate = 0.0;
  double total_write_rate = 0.0;
  std::size_t processes_with_io_data = 0;

  // Process activity since the aggregator's previous snapshot.
  std::size_t process_creations = 0;
  std::size_t process_exits = 0;
  double creation_rate_per_second = 0.0;
  double exit_rate_per_second = 0.0;
};

/**
 * Builds the system-wide statistics for the processes of `snapshot`.
 *
 * The snapshot is fully owned by the caller; aggregation reads it without
 * filtering or mutating. Per-process values with non-zero totals (CPU ticks,
 * resident memory, thread counts, I/O rates) are always accumulated; the
 * `processes_with_*_data` counters record how many processes contributed, so
 * a caller can distinguish "zero" from "no data".
 */
[[nodiscard]] SystemProcessStatistics
aggregateSystemProcessStatistics(const ProcessSnapshot &snapshot);

/**
 * Tracks the process population across successive snapshots.
 *
 * Holds the previous snapshot's process identities (PID + start-time tick
 * from /proc/<pid>/stat, the same identity ProcessMonitor keys its historical
 * deltas on) together with the previous snapshot time. Each update() returns
 * the aggregated statistics plus process creation/exit counts and per-second
 * rates measured against that previous snapshot.
 */
class ProcessStatisticsAggregator {
 public:
  ProcessStatisticsAggregator() = default;
  ~ProcessStatisticsAggregator() = default;

  // Aggregators hold previous-snapshot state; copying/moving one would
  // duplicate the baseline used to derive creation/exit rates.
  ProcessStatisticsAggregator(const ProcessStatisticsAggregator &) = delete;
  ProcessStatisticsAggregator &operator=(const ProcessStatisticsAggregator &) = delete;

  /**
   * Aggregates `snapshot` and records creation/exit activity against the
   * previous snapshot. The very first call only establishes a baseline: the
   * returned statistics carry all aggregates but zero activity values.
   *
   * A PID whose /proc/<pid>/stat start-time differs from the previous
   * snapshot is treated as one exit (the old incarnation) plus one creation
   * (the new one), mirroring how ProcessMonitor guards its own deltas.
   */
  [[nodiscard]] SystemProcessStatistics update(const ProcessSnapshot &snapshot);

  /// Discards the retained baseline so the next update() starts fresh.
  void reset();

 private:
  /// Identity of one process incarnation (PID + start time).
  struct Identity {
    int pid = 0;
    std::uint64_t starttime_ticks = 0;

    bool operator==(const Identity &) const = default;
  };

  /// Empty specialization so Identity works as an unordered_set key.
  struct IdentityHash {
    std::size_t operator()(const Identity &identity) const {
      std::size_t h1 = std::hash<int>{}(identity.pid);
      std::size_t h2 = std::hash<std::uint64_t>{}(identity.starttime_ticks);
      return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
  };

  std::unordered_set<Identity, IdentityHash> previous_identities_;
  std::chrono::steady_clock::time_point previous_time_;
  bool has_previous_ = false;
};

}  // namespace atm