#include "process_statistics.hpp"

#include <chrono>

namespace atm {

SystemProcessStatistics
aggregateSystemProcessStatistics(const ProcessSnapshot &snapshot) {
  SystemProcessStatistics stats;
  stats.total = snapshot.processes.size();

  for (const Process &process : snapshot.processes) {
    switch (process.state) {
      case ProcessState::Running:
        ++stats.running;
        break;
      case ProcessState::Sleeping:
        ++stats.sleeping;
        break;
      case ProcessState::DiskSleep:
        ++stats.disk_sleep;
        break;
      case ProcessState::Stopped:
        ++stats.stopped;
        break;
      case ProcessState::Zombie:
        ++stats.zombie;
        break;
      case ProcessState::Idle:
        ++stats.idle;
        break;
      case ProcessState::Unknown:
        ++stats.unknown;
        break;
    }

    if (process.thread_count > 0) {
      stats.total_threads += process.thread_count;
      ++stats.processes_with_thread_data;
    }

    // The monitor computes cpu_percent against its own baseline; a zero value
    // is indistinguishable from "not yet sampled", so every scanned process
    // counts as having CPU data.
    stats.aggregate_cpu_percent += process.cpu_percent;
    ++stats.processes_with_cpu_data;

    stats.total_user_cpu_ticks += process.user_cpu_ticks;
    stats.total_system_cpu_ticks += process.system_cpu_ticks;
    stats.total_cpu_ticks = stats.total_user_cpu_ticks +
                            stats.total_system_cpu_ticks;

    if (process.memory_kib > 0) {
      stats.total_rss_kib += process.memory_kib;
      ++stats.processes_with_memory_data;
    }
    if (process.shared_memory_kib > 0) {
      stats.total_shared_kib += process.shared_memory_kib;
    }
    stats.aggregate_memory_percent += process.memory_percent;

    if (process.io_available) {
      stats.total_read_rate += process.read_rate;
      stats.total_write_rate += process.write_rate;
      ++stats.processes_with_io_data;
    }
  }

  return stats;
}

SystemProcessStatistics
ProcessStatisticsAggregator::update(const ProcessSnapshot &snapshot) {
  SystemProcessStatistics stats =
      aggregateSystemProcessStatistics(snapshot);

  const auto now = std::chrono::steady_clock::now();

  if (!has_previous_) {
    has_previous_ = true;
    previous_time_ = now;
    previous_identities_.clear();
    previous_identities_.reserve(snapshot.processes.size());
    for (const Process &process : snapshot.processes) {
      previous_identities_.emplace(Identity{process.pid,
                                            process.starttime_ticks});
    }
    return stats;
  }

  // Diff the current incarnation set against the previous one.
  std::unordered_set<Identity, IdentityHash> current;
  current.reserve(snapshot.processes.size());
  for (const Process &process : snapshot.processes) {
    current.emplace(Identity{process.pid, process.starttime_ticks});
  }

  std::size_t creations = 0;
  std::size_t exits = 0;

  for (const Identity &identity : current) {
    if (previous_identities_.find(identity) == previous_identities_.end()) {
      ++creations;
    }
  }
  for (const Identity &identity : previous_identities_) {
    if (current.find(identity) == current.end()) {
      ++exits;
    }
  }

  const double elapsed_seconds =
      std::chrono::duration<double>(now - previous_time_).count();

  stats.process_creations = creations;
  stats.process_exits = exits;
  if (elapsed_seconds > 0.0) {
    stats.creation_rate_per_second = static_cast<double>(creations) / elapsed_seconds;
    stats.exit_rate_per_second = static_cast<double>(exits) / elapsed_seconds;
  }

  previous_time_ = now;
  previous_identities_ = std::move(current);
  return stats;
}

void ProcessStatisticsAggregator::reset() {
  previous_identities_.clear();
  has_previous_ = false;
}

}  // namespace atm