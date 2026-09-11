#include "history_manager.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "process_statistics.hpp"

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
      network_tx_(max_samples),
      process_count_(max_samples),
      running_count_(max_samples),
      zombie_count_(max_samples),
      thread_count_(max_samples),
      aggregate_cpu_(max_samples),
      aggregate_rss_(max_samples),
      creation_rate_(max_samples),
      exit_rate_(max_samples),
      memory_available_percent_(max_samples),
      cached_bytes_(max_samples),
      reclaimable_bytes_(max_samples),
      commitment_percent_(max_samples),
      cpu_pressure_some_(max_samples),
      cpu_pressure_full_(max_samples),
      memory_pressure_some_(max_samples),
      memory_pressure_full_(max_samples),
      io_pressure_some_(max_samples),
      io_pressure_full_(max_samples) {}

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

void HistoryManager::syncCpuHistories(const std::vector<int>& cpu_ids) {
  // Rebuild only when the CPU set changes; existing samples are preserved.
  bool changed = false;
  if (cpu_ids.size() != cpus_.size()) {
    changed = true;
  } else {
    for (std::size_t i = 0; i < cpu_ids.size(); ++i) {
      if (cpus_[i].cpu_id != cpu_ids[i]) {
        changed = true;
        break;
      }
    }
  }

  if (changed) {
    cpus_.clear();
    cpus_.reserve(cpu_ids.size());
    for (int cpu_id : cpu_ids) {
      CpuHistory h;
      h.cpu_id = cpu_id;
      h.utilization = ResourceHistory<TimedSample>(max_samples_);
      cpus_.push_back(std::move(h));
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
  process_count_.clear();
  running_count_.clear();
  zombie_count_.clear();
  thread_count_.clear();
  aggregate_cpu_.clear();
  aggregate_rss_.clear();
  creation_rate_.clear();
  exit_rate_.clear();
  memory_available_percent_.clear();
  cached_bytes_.clear();
  reclaimable_bytes_.clear();
  commitment_percent_.clear();
  cpu_pressure_some_.clear();
  cpu_pressure_full_.clear();
  memory_pressure_some_.clear();
  memory_pressure_full_.clear();
  io_pressure_some_.clear();
  io_pressure_full_.clear();
  for (auto& g : gpus_) {
    g.utilization.clear();
    g.vram_usage.clear();
  }
  for (auto& s : sensors_) {
    s.temperature.clear();
  }
  for (auto& c : cpus_) {
    c.utilization.clear();
  }
}

void HistoryManager::setMaxSamples(std::size_t max_samples) {
  max_samples_ = max_samples;
  clearAll();
  // Rebuild per-device histories seeded by the next update() call.
  gpus_.clear();
  sensors_.clear();
  cpus_.clear();
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

void HistoryManager::updateCpuHistories(
    const std::vector<std::pair<int, double>>& cpu_utilizations) {
  if (paused_) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();

  std::vector<int> cpu_ids;
  cpu_ids.reserve(cpu_utilizations.size());
  for (const auto& util : cpu_utilizations) {
    cpu_ids.push_back(util.first);
  }
  syncCpuHistories(cpu_ids);
  for (std::size_t i = 0; i < cpu_utilizations.size() && i < cpus_.size(); ++i) {
    if (std::isfinite(cpu_utilizations[i].second)) {
      cpus_[i].utilization.addSample(
          TimedSample{now, cpu_utilizations[i].second});
    }
  }
}

void HistoryManager::updateAdvancedMemory(
    const AdvancedMemoryMetrics &metrics) {
  if (paused_) {
    return;
  }
  if (!metrics.available_percent.has_value() &&
      !metrics.cached_bytes.has_value() &&
      !metrics.reclaimable_bytes.has_value() &&
      !metrics.commitment_percent.has_value()) {
    return;  // nothing available this refresh — no empty samples
  }

  const auto now = std::chrono::steady_clock::now();

  if (metrics.available_percent.has_value() &&
      std::isfinite(*metrics.available_percent)) {
    memory_available_percent_.addSample(
        TimedSample{now, *metrics.available_percent});
  }
  if (metrics.cached_bytes.has_value() &&
      std::isfinite(*metrics.cached_bytes)) {
    cached_bytes_.addSample(TimedSample{now, *metrics.cached_bytes});
  }
  if (metrics.reclaimable_bytes.has_value() &&
      std::isfinite(*metrics.reclaimable_bytes)) {
    reclaimable_bytes_.addSample(
        TimedSample{now, *metrics.reclaimable_bytes});
  }
  if (metrics.commitment_percent.has_value() &&
      std::isfinite(*metrics.commitment_percent)) {
    commitment_percent_.addSample(
        TimedSample{now, *metrics.commitment_percent});
  }
}

void HistoryManager::updatePressure(const PressureMetrics &metrics) {
  if (paused_) {
    return;
  }
  if (!metrics.cpu_some.has_value() && !metrics.cpu_full.has_value() &&
      !metrics.memory_some.has_value() && !metrics.memory_full.has_value() &&
      !metrics.io_some.has_value() && !metrics.io_full.has_value()) {
    return;  // nothing available this refresh — no empty samples
  }

  const auto now = std::chrono::steady_clock::now();

  if (metrics.cpu_some.has_value() && std::isfinite(*metrics.cpu_some)) {
    cpu_pressure_some_.addSample(TimedSample{now, *metrics.cpu_some});
  }
  if (metrics.cpu_full.has_value() && std::isfinite(*metrics.cpu_full)) {
    cpu_pressure_full_.addSample(TimedSample{now, *metrics.cpu_full});
  }
  if (metrics.memory_some.has_value() && std::isfinite(*metrics.memory_some)) {
    memory_pressure_some_.addSample(TimedSample{now, *metrics.memory_some});
  }
  if (metrics.memory_full.has_value() && std::isfinite(*metrics.memory_full)) {
    memory_pressure_full_.addSample(TimedSample{now, *metrics.memory_full});
  }
  if (metrics.io_some.has_value() && std::isfinite(*metrics.io_some)) {
    io_pressure_some_.addSample(TimedSample{now, *metrics.io_some});
  }
  if (metrics.io_full.has_value() && std::isfinite(*metrics.io_full)) {
    io_pressure_full_.addSample(TimedSample{now, *metrics.io_full});
  }
}

void HistoryManager::updateProcessStats(const SystemProcessStatistics &stats) {
  if (paused_) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();

  process_count_.addSample(
      TimedSample{now, static_cast<double>(stats.total)});
  running_count_.addSample(
      TimedSample{now, static_cast<double>(stats.running)});
  zombie_count_.addSample(
      TimedSample{now, static_cast<double>(stats.zombie)});
  thread_count_.addSample(
      TimedSample{now, static_cast<double>(stats.total_threads)});
  aggregate_cpu_.addSample(
      TimedSample{now, stats.aggregate_cpu_percent});
  aggregate_rss_.addSample(
      TimedSample{now, static_cast<double>(stats.total_rss_kib)});
  creation_rate_.addSample(
      TimedSample{now, stats.creation_rate_per_second});
  exit_rate_.addSample(
      TimedSample{now, stats.exit_rate_per_second});
}

}  // namespace atm
