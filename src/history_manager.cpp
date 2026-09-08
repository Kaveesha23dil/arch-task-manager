#include "history_manager.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace atm {

HistoryManager::HistoryManager(std::size_t max_samples)
    : max_samples_(max_samples),
      cpu_(max_samples),
      memory_(max_samples),
      used_ram_(max_samples),
      available_ram_(max_samples),
      swap_(max_samples),
      disk_read_(max_samples),
      disk_write_(max_samples),
      network_rx_(max_samples),
      network_tx_(max_samples) {}

void HistoryManager::syncGpuHistories(
    const std::vector<std::pair<std::string, double>>& utilizations) {
  bool changed = false;

  if (utilizations.size() != gpus_.size()) {
    changed = true;
  } else {
    for (std::size_t i = 0; i < utilizations.size(); ++i) {
      if (gpus_[i].name != utilizations[i].first) {
        changed = true;
        break;
      }
    }
  }

  if (changed) {
    gpus_.clear();
    gpus_.reserve(utilizations.size());
    for (const auto& util : utilizations) {
      GpuHistory h;
      h.name = util.first;
      h.utilization = ResourceHistory<TimedSample>(max_samples_);
      h.vram_usage = ResourceHistory<TimedSample>(max_samples_);
      gpus_.push_back(std::move(h));
    }
  }
}

void HistoryManager::syncSensorHistories(
    const std::vector<std::pair<std::string, double>>& temperatures) {
  // Rebuild only when the sensor set changes.
  bool changed = false;
  if (temperatures.size() != sensors_.size()) {
    changed = true;
  } else {
    for (std::size_t i = 0; i < temperatures.size(); ++i) {
      if (sensors_[i].label != temperatures[i].first) {
        changed = true;
        break;
      }
    }
  }

  if (changed) {
    sensors_.clear();
    sensors_.reserve(temperatures.size());
    for (const auto& temp : temperatures) {
      SensorHistory h;
      h.label = temp.first;
      h.temperature = ResourceHistory<TimedSample>(max_samples_);
      sensors_.push_back(std::move(h));
    }
  }
}

void HistoryManager::setPaused(bool paused) { paused_ = paused; }

void HistoryManager::clearAll() {
  cpu_.clear();
  memory_.clear();
  used_ram_.clear();
  available_ram_.clear();
  swap_.clear();
  disk_read_.clear();
  disk_write_.clear();
  network_rx_.clear();
  network_tx_.clear();
  for (auto& g : gpus_) {
    g.utilization.clear();
    g.vram_usage.clear();
  }
  for (auto& s : sensors_) {
    s.temperature.clear();
  }
}

void HistoryManager::update(
    double cpu_usage, double memory_usage, double used_ram_kib,
    double available_ram_kib, double swap_usage, double disk_read_bps,
    double disk_write_bps, double network_rx_bps, double network_tx_bps,
    const std::vector<std::pair<std::string, double>>& gpu_utilizations,
    const std::vector<std::pair<std::string, double>>& gpu_vram_usages,
    const std::vector<std::pair<std::string, double>>& temperatures) {
  if (paused_) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();

  cpu_.addSample(TimedSample{now, cpu_usage});
  memory_.addSample(TimedSample{now, memory_usage});
  used_ram_.addSample(TimedSample{now, used_ram_kib});
  available_ram_.addSample(TimedSample{now, available_ram_kib});
  swap_.addSample(TimedSample{now, swap_usage});
  disk_read_.addSample(TimedSample{now, disk_read_bps});
  disk_write_.addSample(TimedSample{now, disk_write_bps});
  network_rx_.addSample(TimedSample{now, network_rx_bps});
  network_tx_.addSample(TimedSample{now, network_tx_bps});

  syncGpuHistories(gpu_utilizations);
  for (std::size_t i = 0; i < gpu_utilizations.size() && i < gpus_.size(); ++i) {
    if (!std::isnan(gpu_utilizations[i].second)) {
      gpus_[i].utilization.addSample(TimedSample{now, gpu_utilizations[i].second});
    }
  }
  for (std::size_t i = 0; i < gpu_vram_usages.size() && i < gpus_.size(); ++i) {
    if (!std::isnan(gpu_vram_usages[i].second)) {
      gpus_[i].vram_usage.addSample(TimedSample{now, gpu_vram_usages[i].second});
    }
  }

  syncSensorHistories(temperatures);
  for (std::size_t i = 0; i < temperatures.size() && i < sensors_.size(); ++i) {
    if (!std::isnan(temperatures[i].second)) {
      sensors_[i].temperature.addSample(TimedSample{now, temperatures[i].second});
    }
  }
}

}  // namespace atm
