#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "resource_history.hpp"

namespace atm {

struct SystemProcessStatistics;

/// Compile-time default history length. With a 1-second refresh interval this
/// holds ~2 minutes (120 samples) of history.
inline constexpr std::size_t kDefaultHistorySamples = 120;

/// Stores a bounded per-GPU history (utilization + VRAM usage).
struct GpuHistory {
  std::string name;
  ResourceHistory<TimedSample> utilization{0};
  ResourceHistory<TimedSample> vram_usage{0};
};

/// Stores a bounded per-sensor temperature history.
struct SensorHistory {
  std::string label;
  ResourceHistory<TimedSample> temperature{0};
};

/// Stores a bounded per-logical-CPU utilization history. CPU identity is the
/// logical CPU ID, so hotplugged/offlined CPUs never alias each other.
struct CpuHistory {
  int cpu_id = -1;
  ResourceHistory<TimedSample> utilization{0};
};

/// Optional advanced-memory metric values for one refresh. `std::nullopt`
/// means the metric is unavailable for that refresh (missing / unsupported
/// kernel field), which simply skips the sample instead of recording garbage.
struct AdvancedMemoryMetrics {
  std::optional<double> available_percent;
  std::optional<double> cached_bytes;
  std::optional<double> reclaimable_bytes;
  std::optional<double> commitment_percent;
};

/// Optional system-pressure values (10-second some/full averages) for one
/// refresh. `std::nullopt` means that pressure line/metric was unavailable for
/// that refresh (PSI unsupported, file unreadable, or the kernel did not
/// report it); such metrics simply skip the sample instead of plotting zeros.
/// 60/300-second windows and cumulative totals stay in the pressure detail
/// table; only the 10-second averages are graphed to avoid graph clutter.
struct PressureMetrics {
  std::optional<double> cpu_some;
  std::optional<double> cpu_full;
  std::optional<double> memory_some;
  std::optional<double> memory_full;
  std::optional<double> io_some;
  std::optional<double> io_full;
};

/// Aggregates all system-level resource histories. It stores already-computed
/// values from the existing monitors; it never reads /proc or /sys itself.
class HistoryManager {
 public:
  explicit HistoryManager(std::size_t max_samples = kDefaultHistorySamples);

  /// Updates all histories from the current metric values. Called once per
  /// refresh of the central update loop.
  void update(double cpu_usage, double memory_usage, double used_ram_kib,
              double available_ram_kib, double swap_usage,
              double disk_read_bps, double disk_write_bps,
              double network_rx_bps, double network_tx_bps,
              const std::vector<std::pair<std::string, double>>& gpu_utilizations,
              const std::vector<std::pair<std::string, double>>& gpu_vram_usages,
              const std::vector<std::pair<std::string, double>>& temperatures);

  /// Appends the system-wide process statistics to the process histories.
  /// Respects the same pause / max-samples behavior as update().
  void updateProcessStats(const SystemProcessStatistics &stats);

  /// Appends one per-CPU utilization sample each for every CPU in the vector,
  /// keyed by logical CPU ID. CPUs not in the vector (offline/newly appearing)
  /// keep their existing history; a vanished CPU keeps its final sample until
  /// idleness/removal. Called once per refresh alongside update().
  void updateCpuHistories(
      const std::vector<std::pair<int, double>>& cpu_utilizations);

  /// Appends one sample for each available advanced-memory metric (available
  /// percent, cached, reclaimable, commitment). Unavailable metrics contribute
  /// no sample, so a metric that disappears mid-run simply stops plotting.
  /// Called once per refresh alongside update(); timestamped consistently.
  void updateAdvancedMemory(const AdvancedMemoryMetrics& metrics);

  /// Appends one sample for each available system-pressure 10-second average
  /// (some/full per category). A missing metric contributes no sample, so a
  /// metric that disappears mid-run simply stops plotting instead of drawing a
  /// fake zero. Called once per refresh alongside update(); timestamped
  /// consistently with a single clock read for the whole batch.
  void updatePressure(const PressureMetrics& metrics);

  /// Resets all history buffers.
  void clearAll();

  /// Changes the number of samples kept and discards existing history (used
  /// when the configured max_samples changes at runtime).
  void setMaxSamples(std::size_t max_samples);

  /// Temporarily stops collecting samples. When resumed, no fake samples are
  /// created to fill the paused period.
  void setPaused(bool paused);
  bool paused() const { return paused_; }

  const ResourceHistory<TimedSample>& cpuHistory() const { return cpu_; }
  const ResourceHistory<TimedSample>& memoryHistory() const { return memory_; }
  const ResourceHistory<TimedSample>& usedRamHistory() const { return used_ram_; }
  const ResourceHistory<TimedSample>& availableRamHistory() const { return available_ram_; }
  const ResourceHistory<TimedSample>& swapHistory() const { return swap_; }
  const ResourceHistory<TimedSample>& diskReadHistory() const { return disk_read_; }
  const ResourceHistory<TimedSample>& diskWriteHistory() const { return disk_write_; }
  const ResourceHistory<TimedSample>& networkRxHistory() const { return network_rx_; }
  const ResourceHistory<TimedSample>& networkTxHistory() const { return network_tx_; }

  // System-wide process statistics histories.
  const ResourceHistory<TimedSample>& processCountHistory() const { return process_count_; }
  const ResourceHistory<TimedSample>& runningCountHistory() const { return running_count_; }
  const ResourceHistory<TimedSample>& zombieCountHistory() const { return zombie_count_; }
  const ResourceHistory<TimedSample>& threadCountHistory() const { return thread_count_; }
  const ResourceHistory<TimedSample>& aggregateCpuHistory() const { return aggregate_cpu_; }
  const ResourceHistory<TimedSample>& aggregateRssHistory() const { return aggregate_rss_; }
  const ResourceHistory<TimedSample>& creationRateHistory() const { return creation_rate_; }
  const ResourceHistory<TimedSample>& exitRateHistory() const { return exit_rate_; }

  const std::vector<GpuHistory>& gpuHistories() const { return gpus_; }
  const std::vector<SensorHistory>& sensorHistories() const { return sensors_; }
  const std::vector<CpuHistory>& cpuHistories() const { return cpus_; }

  // Advanced memory histories.
  const ResourceHistory<TimedSample>& memoryAvailablePercentHistory() const {
    return memory_available_percent_;
  }
  const ResourceHistory<TimedSample>& cachedBytesHistory() const {
    return cached_bytes_;
  }
  const ResourceHistory<TimedSample>& reclaimableBytesHistory() const {
    return reclaimable_bytes_;
  }
  const ResourceHistory<TimedSample>& commitmentPercentHistory() const {
    return commitment_percent_;
  }

  // System pressure (PSI) 10-second some/full average histories.
  const ResourceHistory<TimedSample>& cpuPressureSomeHistory() const {
    return cpu_pressure_some_;
  }
  const ResourceHistory<TimedSample>& cpuPressureFullHistory() const {
    return cpu_pressure_full_;
  }
  const ResourceHistory<TimedSample>& memoryPressureSomeHistory() const {
    return memory_pressure_some_;
  }
  const ResourceHistory<TimedSample>& memoryPressureFullHistory() const {
    return memory_pressure_full_;
  }
  const ResourceHistory<TimedSample>& ioPressureSomeHistory() const {
    return io_pressure_some_;
  }
  const ResourceHistory<TimedSample>& ioPressureFullHistory() const {
    return io_pressure_full_;
  }

  std::size_t maxSamples() const { return max_samples_; }

 private:
  std::size_t max_samples_;

  ResourceHistory<TimedSample> cpu_;
  ResourceHistory<TimedSample> memory_;
  ResourceHistory<TimedSample> used_ram_;
  ResourceHistory<TimedSample> available_ram_;
  ResourceHistory<TimedSample> swap_;
  ResourceHistory<TimedSample> disk_read_;
  ResourceHistory<TimedSample> disk_write_;
  ResourceHistory<TimedSample> network_rx_;
  ResourceHistory<TimedSample> network_tx_;

  // System-wide process statistics histories.
  ResourceHistory<TimedSample> process_count_;
  ResourceHistory<TimedSample> running_count_;
  ResourceHistory<TimedSample> zombie_count_;
  ResourceHistory<TimedSample> thread_count_;
  ResourceHistory<TimedSample> aggregate_cpu_;
  ResourceHistory<TimedSample> aggregate_rss_;
  ResourceHistory<TimedSample> creation_rate_;
  ResourceHistory<TimedSample> exit_rate_;

  std::vector<GpuHistory> gpus_;
  std::vector<SensorHistory> sensors_;
  std::vector<CpuHistory> cpus_;

  // Advanced memory histories.
  ResourceHistory<TimedSample> memory_available_percent_;
  ResourceHistory<TimedSample> cached_bytes_;
  ResourceHistory<TimedSample> reclaimable_bytes_;
  ResourceHistory<TimedSample> commitment_percent_;

  // System pressure (PSI) histories for the 10-second some/full averages.
  ResourceHistory<TimedSample> cpu_pressure_some_;
  ResourceHistory<TimedSample> cpu_pressure_full_;
  ResourceHistory<TimedSample> memory_pressure_some_;
  ResourceHistory<TimedSample> memory_pressure_full_;
  ResourceHistory<TimedSample> io_pressure_some_;
  ResourceHistory<TimedSample> io_pressure_full_;

  bool paused_ = false;

  /// Returns true if the device/history set matches; otherwise rebuilds it.
  void syncGpuHistories(
      const std::vector<std::pair<std::string, double>>& utilizations);
  void syncSensorHistories(
      const std::vector<std::pair<std::string, double>>& temperatures);
  void syncCpuHistories(const std::vector<int>& cpu_ids);
};

}  // namespace atm
