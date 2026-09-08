#include "alert_manager.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>

namespace atm {

namespace {

std::string formatPercent(double value) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(0) << value << '%';
  return out.str();
}

std::string formatCelsius(double value) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(0) << value << "\u00b0C";
  return out.str();
}

std::string formatRate(double bytes_per_second) {
  static constexpr std::array kSuffixes = {"B/s", "kB/s", "MB/s", "GB/s", "TB/s"};
  double v = std::abs(bytes_per_second);
  std::size_t suffix = 0;
  constexpr double kKilobyte = 1024.0;
  while (v >= kKilobyte && suffix + 1 < kSuffixes.size()) {
    v /= kKilobyte;
    ++suffix;
  }
  std::ostringstream out;
  out << std::fixed << std::setprecision(v >= 100.0 ? 0 : 1) << v << ' '
      << kSuffixes[suffix];
  return out.str();
}

}  // namespace

const char *alertTypeName(AlertType type) {
  switch (type) {
    case AlertType::CpuUsage:         return "CPU";
    case AlertType::MemoryUsage:      return "Memory";
    case AlertType::SwapUsage:        return "Swap";
    case AlertType::DiskUsage:        return "Disk capacity";
    case AlertType::DiskReadActivity: return "Disk read";
    case AlertType::DiskWriteActivity:return "Disk write";
    case AlertType::NetworkReceive:   return "Network receive";
    case AlertType::NetworkTransmit:  return "Network transmit";
    case AlertType::GpuUsage:         return "GPU usage";
    case AlertType::GpuMemoryUsage:   return "GPU memory";
    case AlertType::Temperature:      return "Temperature";
  }
  return "Unknown";
}

const char *alertSeverityName(AlertSeverity severity) {
  switch (severity) {
    case AlertSeverity::Normal:   return "Normal";
    case AlertSeverity::Warning:  return "Warning";
    case AlertSeverity::Critical: return "Critical";
  }
  return "Normal";
}

AlertThreshold AlertDefaults::forType(AlertType type) {
  AlertThreshold t;
  switch (type) {
    case AlertType::CpuUsage:
      t.warning = 80.0; t.critical = 95.0; t.recovery = 75.0; t.enabled = true;
      break;
    case AlertType::MemoryUsage:
      t.warning = 80.0; t.critical = 95.0; t.recovery = 75.0; t.enabled = true;
      break;
    case AlertType::SwapUsage:
      t.warning = 70.0; t.critical = 90.0; t.recovery = 65.0; t.enabled = true;
      break;
    case AlertType::DiskUsage:
      t.warning = 85.0; t.critical = 95.0; t.recovery = 80.0; t.enabled = true;
      break;
    case AlertType::DiskReadActivity:
      // Throughput alerts are optional and disabled by default.
      t.warning = 500.0;   // MB/s
      t.critical = 1000.0; // MB/s
      t.recovery = 400.0;
      t.enabled = false;
      break;
    case AlertType::DiskWriteActivity:
      t.warning = 500.0; t.critical = 1000.0; t.recovery = 400.0;
      t.enabled = false;
      break;
    case AlertType::NetworkReceive:
      // Optional, disabled by default (high throughput is not an error).
      t.warning = 125.0;   // MB/s (~1 Gbps)
      t.critical = 250.0;  // MB/s (~2 Gbps)
      t.recovery = 100.0;
      t.enabled = false;
      break;
    case AlertType::NetworkTransmit:
      t.warning = 125.0; t.critical = 250.0; t.recovery = 100.0;
      t.enabled = false;
      break;
    case AlertType::GpuUsage:
      t.warning = 85.0; t.critical = 95.0; t.recovery = 80.0; t.enabled = true;
      break;
    case AlertType::GpuMemoryUsage:
      t.warning = 85.0; t.critical = 95.0; t.recovery = 80.0; t.enabled = true;
      break;
    case AlertType::Temperature:
      t.warning = 75.0; t.critical = 90.0; t.recovery = 70.0; t.enabled = true;
      break;
  }
  return t;
}

AlertManager::AlertManager(std::size_t max_history) : max_history_(max_history) {
  // Initialise thresholds for every alert type.
  for (int i = 0; i <= static_cast<int>(AlertType::Temperature); ++i) {
    const AlertType type = static_cast<AlertType>(i);
    thresholds_[type] = AlertDefaults::forType(type);
  }
}

AlertThreshold &AlertManager::threshold(AlertType type) {
  return thresholds_[type];
}

const AlertThreshold &AlertManager::threshold(AlertType type) const {
  return thresholds_.at(type);
}

std::string AlertManager::subjectKey(AlertType type, const std::string &source) {
  return std::to_string(static_cast<int>(type)) + "|" + source;
}

void AlertManager::pushEvent(const AlertEvent &event) {
  history_.push_back(event);
  if (history_.size() > max_history_) {
    history_.pop_front();
  }
}

std::optional<AlertEvent> AlertManager::evaluate(
    AlertType type, const std::string &source, double value, bool value_available,
    double warning, double critical, double recovery,
    const std::string &message_prefix) {
  const std::string key = subjectKey(type, source);
  SubjectState &state = subjects_[key];
  const AlertSeverity previous = state.severity;

  // Unavailable values never change state and never generate alerts.
  if (!value_available) {
    return std::nullopt;
  }
  state.value = value;

  AlertSeverity next;
  if (value >= critical) {
    next = AlertSeverity::Critical;
  } else if (value >= warning) {
    next = AlertSeverity::Warning;
  } else if (value < recovery) {
    next = AlertSeverity::Normal;
  } else {
    next = previous;  // within the hysteresis band: keep the current state
  }

  // No state change -> no new alert (prevents alert spam).
  if (next == previous) {
    return std::nullopt;
  }

  AlertEvent event;
  event.type = type;
  event.severity = next;
  event.source = source;
  event.value = value;
  event.threshold = next == AlertSeverity::Critical
                        ? critical
                        : next == AlertSeverity::Warning ? warning : recovery;
  event.is_recovery = (next == AlertSeverity::Normal);
  event.timestamp = std::chrono::system_clock::now();

  const std::string value_text = [&]() {
    if (type == AlertType::Temperature) return formatCelsius(value);
    if (type == AlertType::DiskReadActivity || type == AlertType::DiskWriteActivity ||
        type == AlertType::NetworkReceive || type == AlertType::NetworkTransmit) {
      return formatRate(value);
    }
    return formatPercent(value);
  }();
  event.message = message_prefix + value_text;

  state.severity = next;
  state.threshold = event.threshold;
  pushEvent(event);
  if (notification_sink_ != nullptr) {
    notification_sink_(*this, event);
  }
  return event;
}

bool AlertManager::hasActiveAlerts() const {
  for (const auto &kv : subjects_) {
    if (kv.second.severity != AlertSeverity::Normal) {
      return true;
    }
  }
  return false;
}

AlertSeverity AlertManager::worstActiveSeverity() const {
  AlertSeverity worst = AlertSeverity::Normal;
  for (const auto &kv : subjects_) {
    if (kv.second.severity > worst) {
      worst = kv.second.severity;
    }
  }
  return worst;
}

AlertSeverity AlertManager::currentSeverity(AlertType type,
                                            const std::string &source) const {
  const auto it = subjects_.find(subjectKey(type, source));
  if (it == subjects_.end()) {
    return AlertSeverity::Normal;
  }
  return it->second.severity;
}

void AlertManager::reset() {
  history_.clear();
  subjects_.clear();
}

void AlertManager::setNotificationSink(NotificationSink sink) {
  notification_sink_ = sink;
}

void AlertManager::updateCpu(double usage_percent) {
  const AlertThreshold &t = threshold(AlertType::CpuUsage);
  if (!t.enabled) return;
  evaluate(AlertType::CpuUsage, "system", usage_percent, std::isfinite(usage_percent),
           t.warning, t.critical, t.recovery < 0 ? t.warning : t.recovery,
           "CPU usage is high: ");
}

void AlertManager::updateMemory(double ram_usage_percent, double swap_usage_percent) {
  {
    const AlertThreshold &t = threshold(AlertType::MemoryUsage);
    if (t.enabled) {
      evaluate(AlertType::MemoryUsage, "system", ram_usage_percent,
               std::isfinite(ram_usage_percent), t.warning, t.critical,
               t.recovery < 0 ? t.warning : t.recovery,
               "Memory usage is high: ");
    }
  }
  {
    const AlertThreshold &t = threshold(AlertType::SwapUsage);
    if (t.enabled) {
      evaluate(AlertType::SwapUsage, "system", swap_usage_percent,
               std::isfinite(swap_usage_percent), t.warning, t.critical,
               t.recovery < 0 ? t.warning : t.recovery,
               "Swap usage is high: ");
    }
  }
}

void AlertManager::updateDiskUsage(const std::string &mount_point,
                                   double usage_percent) {
  const AlertThreshold &t = threshold(AlertType::DiskUsage);
  if (!t.enabled) return;
  evaluate(AlertType::DiskUsage, mount_point, usage_percent,
           std::isfinite(usage_percent), t.warning, t.critical,
           t.recovery < 0 ? t.warning : t.recovery,
           "Disk usage is high: ");
}

void AlertManager::updateDiskActivity(double read_bps, double write_bps) {
  {
    const AlertThreshold &t = threshold(AlertType::DiskReadActivity);
    if (t.enabled) {
      evaluate(AlertType::DiskReadActivity, "system", read_bps,
               std::isfinite(read_bps), t.warning, t.critical,
               t.recovery < 0 ? t.warning : t.recovery,
               "High disk read activity: ");
    }
  }
  {
    const AlertThreshold &t = threshold(AlertType::DiskWriteActivity);
    if (t.enabled) {
      evaluate(AlertType::DiskWriteActivity, "system", write_bps,
               std::isfinite(write_bps), t.warning, t.critical,
               t.recovery < 0 ? t.warning : t.recovery,
               "High disk write activity: ");
    }
  }
}

void AlertManager::updateNetwork(double rx_bps, double tx_bps) {
  {
    const AlertThreshold &t = threshold(AlertType::NetworkReceive);
    if (t.enabled) {
      evaluate(AlertType::NetworkReceive, "system", rx_bps,
               std::isfinite(rx_bps), t.warning, t.critical,
               t.recovery < 0 ? t.warning : t.recovery,
               "High network receive rate: ");
    }
  }
  {
    const AlertThreshold &t = threshold(AlertType::NetworkTransmit);
    if (t.enabled) {
      evaluate(AlertType::NetworkTransmit, "system", tx_bps,
               std::isfinite(tx_bps), t.warning, t.critical,
               t.recovery < 0 ? t.warning : t.recovery,
               "High network transmit rate: ");
    }
  }
}

void AlertManager::updateGpu(const std::string &id, double usage_percent,
                             double vram_percent) {
  {
    const AlertThreshold &t = threshold(AlertType::GpuUsage);
    if (t.enabled) {
      evaluate(AlertType::GpuUsage, id, usage_percent,
               std::isfinite(usage_percent), t.warning, t.critical,
               t.recovery < 0 ? t.warning : t.recovery,
               "GPU usage is high: ");
    }
  }
  {
    const AlertThreshold &t = threshold(AlertType::GpuMemoryUsage);
    if (t.enabled) {
      evaluate(AlertType::GpuMemoryUsage, id, vram_percent,
               std::isfinite(vram_percent), t.warning, t.critical,
               t.recovery < 0 ? t.warning : t.recovery,
               "GPU memory usage is high: ");
    }
  }
}

std::vector<AlertManager::ActiveAlert> AlertManager::activeAlerts() const {
  std::vector<ActiveAlert> result;
  for (const auto &kv : subjects_) {
    if (kv.second.severity == AlertSeverity::Normal) {
      continue;
    }
    // Decode the "type|source" key.
    const std::size_t sep = kv.first.find('|');
    const AlertType type =
        static_cast<AlertType>(std::stoi(kv.first.substr(0, sep)));
    ActiveAlert alert;
    alert.type = type;
    alert.severity = kv.second.severity;
    alert.source = kv.first.substr(sep + 1);
    alert.value = kv.second.value;
    alert.threshold = kv.second.threshold;
    result.push_back(alert);
  }
  return result;
}

void AlertManager::updateTemperature(const std::string &label, double celsius,
                                     double hardware_critical_celsius) {
  const AlertThreshold &t = threshold(AlertType::Temperature);
  if (!t.enabled) return;

  // Prefer the hardware's critical limit when exposed; otherwise use the
  // configured default threshold.
  const double warning = t.warning;
  double critical = t.critical;
  if (hardware_critical_celsius > 0 &&
      hardware_critical_celsius < t.critical) {
    critical = hardware_critical_celsius;
  }
  const double recovery = t.recovery < 0 ? warning : t.recovery;

  evaluate(AlertType::Temperature, label, celsius, std::isfinite(celsius),
           warning, critical, recovery, "Temperature is high: ");
}

}  // namespace atm
