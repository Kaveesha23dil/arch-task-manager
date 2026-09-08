#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace atm {

/// The kind of metric an alert can monitor. Each maps to an existing monitor's
/// output; the manager never reads /proc or /sys itself.
enum class AlertType {
  CpuUsage,
  MemoryUsage,
  SwapUsage,
  DiskUsage,          // filesystem capacity (per mount point)
  DiskReadActivity,   // aggregate read throughput
  DiskWriteActivity,  // aggregate write throughput
  NetworkReceive,     // aggregate RX throughput
  NetworkTransmit,    // aggregate TX throughput
  GpuUsage,           // per-GPU utilization
  GpuMemoryUsage,     // per-GPU VRAM usage
  Temperature,        // per-sensor temperature
};

/// Current severity of a monitored metric, derived from its thresholds.
enum class AlertSeverity {
  Normal,
  Warning,
  Critical,
};

/// One transition/recovery event recorded in the bounded alert history.
struct AlertEvent {
  AlertType type;
  AlertSeverity severity;  // Warning / Critical / Recovery(Normal)
  std::string source;      // human label of the subject (mount, GPU, sensor...)
  std::string message;     // e.g. "CPU usage is high: 87%"
  double value = 0.0;
  double threshold = 0.0;
  bool is_recovery = false;
  std::chrono::system_clock::time_point timestamp;
};

/// Human-readable name of an AlertType.
[[nodiscard]] const char *alertTypeName(AlertType type);

/// Human-readable name of an AlertSeverity ("Normal", "Warning", "Critical").
[[nodiscard]] const char *alertSeverityName(AlertSeverity severity);

/// Configurable thresholds for one alert type.
struct AlertThreshold {
  double warning = 0.0;   // at/above this -> Warning (percentages or units)
  double critical = 0.0;  // at/above this -> Critical
  double recovery = -1.0; // below this -> back to Normal (-1 = warning-based default)
  bool enabled = true;
};

/// Namespace-like helper: default thresholds for every alert type.
struct AlertDefaults {
  /// Returns the default threshold for an alert type.
  [[nodiscard]] static AlertThreshold forType(AlertType type);
};

/// Monitors existing metrics against configurable thresholds and records
/// state transitions into a bounded alert history.
///
/// The AlertManager consumes values already produced by the CPU / memory /
/// disk / network / GPU / sensor monitors (and the history manager). It never
/// reads /proc or /sys itself. State transitions follow a Normal -> Warning ->
/// Critical state machine with a recovery threshold plus simple hysteresis so
/// a metric hovering near a boundary does not oscillate. Only a single event
/// is produced per state transition.
class AlertManager {
 public:
  explicit AlertManager(std::size_t max_history = 100);

  /// Returns true if any monitored metric is currently Warning or Critical.
  [[nodiscard]] bool hasActiveAlerts() const;

  /// The most severe active severity across all metrics (Normal if none).
  [[nodiscard]] AlertSeverity worstActiveSeverity() const;

  /// Snapshot of one subject's current alert state, used by the dashboard.
  struct ActiveAlert {
    AlertType type;
    AlertSeverity severity;
    std::string source;
    double value = 0.0;
    double threshold = 0.0;
  };

  /// All subjects currently in a Warning or Critical state.
  [[nodiscard]] std::vector<ActiveAlert> activeAlerts() const;

  /// Bounded history of alert events (oldest first).
  [[nodiscard]] const std::deque<AlertEvent>& history() const { return history_; }

  /// Current severity of a specific (type, source) subject.
  [[nodiscard]] AlertSeverity currentSeverity(AlertType type,
                                              const std::string& source) const;

  /// Clears all state: history and current severities.
  void reset();

  /// Access to the mutable threshold configuration.
  [[nodiscard]] AlertThreshold& threshold(AlertType type);
  [[nodiscard]] const AlertThreshold& threshold(AlertType type) const;

  /// Feeds one CPU usage value (percent). Called once per refresh.
  void updateCpu(double usage_percent);

  /// Feeds RAM usage (percent) and swap usage (percent).
  void updateMemory(double ram_usage_percent, double swap_usage_percent);

  /// Feeds one filesystem's usage percentage.
  /// @param mount_point the mount point, e.g. "/home".
  void updateDiskUsage(const std::string& mount_point, double usage_percent);

  /// Feeds aggregate disk read/write throughput in bytes per second.
  void updateDiskActivity(double read_bps, double write_bps);

  /// Feeds aggregate network RX/TX throughput in bytes per second.
  void updateNetwork(double rx_bps, double tx_bps);

  /// Feeds one GPU's utilization (percent) and VRAM usage (percent).
  /// Unavailable metrics are passed as NaN and never generate alerts.
  /// @param id human label, e.g. "GPU 0".
  void updateGpu(const std::string& id, double usage_percent, double vram_percent);

  /// Feeds one temperature sensor's reading in Celsius, plus an optional
  /// hardware critical limit (preferred when present).
  /// @param label sensor label, e.g. "CPU Package".
  void updateTemperature(const std::string& label, double celsius,
                         double hardware_critical_celsius = -1.0);

 private:
  struct SubjectState {
    AlertSeverity severity = AlertSeverity::Normal;
    double threshold = 0.0;    // last threshold that triggered the current state
    double value = 0.0;        // most recent value fed for this subject
  };

  std::size_t max_history_;
  std::deque<AlertEvent> history_;
  std::unordered_map<AlertType, AlertThreshold> thresholds_;
  std::unordered_map<std::string, SubjectState> subjects_;  // key = type|source

  void pushEvent(const AlertEvent& event);
  static std::string subjectKey(AlertType type, const std::string& source);

  /// Core state-machine helper. Returns a new event when the severity changed,
  /// otherwise std::nullopt (no alert spam).
  std::optional<AlertEvent> evaluate(AlertType type, const std::string& source,
                                     double value, bool value_available,
                                     double warning, double critical,
                                     double recovery,
                                     const std::string& message_prefix);
};

}  // namespace atm
