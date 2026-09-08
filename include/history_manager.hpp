#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "resource_history.hpp"

namespace atm {

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

  const std::vector<GpuHistory>& gpuHistories() const { return gpus_; }
  const std::vector<SensorHistory>& sensorHistories() const { return sensors_; }

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

  std::vector<GpuHistory> gpus_;
  std::vector<SensorHistory> sensors_;

  bool paused_ = false;

  /// Returns true if the device/history set matches; otherwise rebuilds it.
  void syncGpuHistories(
      const std::vector<std::pair<std::string, double>>& utilizations);
  void syncSensorHistories(
      const std::vector<std::pair<std::string, double>>& temperatures);
};

}  // namespace atm
