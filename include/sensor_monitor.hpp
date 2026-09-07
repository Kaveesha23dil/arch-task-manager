#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace atm {

/// Broad category of a discovered temperature sensor.
///
/// The type is derived at discovery time from the hwmon device name *and* the
/// sensor label (when present) so a sensor is never classified by its label
/// alone. `Other` covers battery/AC-dapter/PSU sensors and anything the
/// heuristics do not recognize.
enum class SensorType {
  CPU,
  GPU,
  Storage,
  Motherboard,
  Other,
};

/// Human-readable name of a SensorType ("CPU", "GPU", ...).
[[nodiscard]] const char *sensorTypeName(SensorType type);

/// Simple temperature assessment derived from the available limits.
///
/// `Normal` is below the warm fraction of the nearest operating limit, `Warm`
/// between that fraction and the maximum, `High` at/above the maximum but
/// below critical, and `Critical` at/above the critical temperature. When no
/// limit is exposed the sensor is classified `Unknown` ("N/A"), never judged.
enum class SensorStatus {
  Unknown,  // no maximum/critical exposure — cannot assess
  Normal,
  Warm,
  High,
  Critical,
};

/// Human-readable name of a SensorStatus ("NORMAL", "WARM", "HIGH",
/// "CRITICAL", "N/A").
[[nodiscard]] const char *sensorStatusName(SensorStatus status);

/// One live temperature reading from a single hwmon channel.
///
/// Identity (device, label, type) is cached at discovery time; only the values
/// are refreshed every tick. `max_temperature_celsius` and
/// `critical_temperature_celsius` stay unset when the driver exposes no such
/// limit, so an unavailable limit is never faked.
struct TemperatureSensor {
  std::string device;  // hwmon device name, e.g. "coretemp", "amdgpu", "nvme"
  std::string label;   // e.g. "Package id 0", "Core 0"; fallback "Temperature N"
  SensorType type = SensorType::Other;
  double temperature_celsius = 0.0;
  std::optional<double> max_temperature_celsius;
  std::optional<double> critical_temperature_celsius;
  SensorStatus status = SensorStatus::Unknown;  // derived from the limits above
};

/// One live fan reading (RPM) from a single hwmon channel. A fan whose sensor
/// is broken or stops being reported is simply omitted from the snapshot.
struct FanSensor {
  std::string device;  // hwmon device name owning the fan
  std::string label;   // e.g. "CPU fan"; fallback "Fan N"
  std::uint64_t rpm = 0;
};

/// One complete sensor snapshot, produced by SensorMonitor::read().
struct SensorSnapshot {
  std::vector<TemperatureSensor> temperatures;  // sorted by device, then temp index
  std::vector<FanSensor> fans;                  // sorted by device, then fan index
  /// True when /sys/class/hwmon exists on this machine (a battery/AC-only
  /// directory with no temperature channels still counts as "available").
  bool available = false;
};

/**
 * Monitors hardware temperature and fan sensors exposed by the Linux hwmon
 * subsystem.
 *
 * Discovery enumerates /sys/class/hwmon and inspects each hwmon device's
 * `temp*`/`fan*` channels. Sensor *metadata* — device name, resolved label,
 * channel type and the paths of the dynamic files — is read once and cached;
 * only the live values (temperature/RPM) are refreshed on every read. The
 * read re-lists the hwmon directory cheaply each tick and re-discovers only
 * when the set of hwmon devices changes, so a sensor that appears or
 * disappears is picked up automatically.
 *
 * Everything is read directly from the kernel's sysfs interface through
 * single-line file reads — no `lm_sensors`/`sensors(1)` process, no shell
 * commands, no fan or voltage control, and no writes to /sys. A missing,
 * malformed, unreadable or implausible value only skips that sensor for that
 * refresh; it never throws or terminates the application.
 *
 * The refresh cadence is owned by the application's main loop — this class
 * never starts its own thread.
 */
class SensorMonitor {
 public:
  SensorMonitor() = default;
  ~SensorMonitor() = default;

  // Monitors hold discovery/refresh state; copying/moving would duplicate it.
  SensorMonitor(const SensorMonitor &) = delete;
  SensorMonitor &operator=(const SensorMonitor &) = delete;

  /// (Re)discovers every hwmon device and caches its sensor channels. Called
  /// once at startup; read() also re-discovers automatically whenever the set
  /// of hwmon devices changes, so a manual call is optional.
  void discover();

  /// Refreshes every cached channel's dynamic value and returns the snapshot.
  /// Sensors whose value is missing/malformed/implausible are skipped.
  [[nodiscard]] SensorSnapshot read();

 private:
  /// Cached, static metadata of one temperature channel (tempN_*). Identity,
  /// label, type and file paths are fixed at discovery; only the values move.
  struct TemperatureChannel {
    std::string device;
    std::string label;
    std::string input_path;  // tempN_input
    std::string max_path;    // tempN_max ("" when not exposed)
    std::string crit_path;   // tempN_crit ("" when not exposed)
    SensorType type = SensorType::Other;
    std::size_t index = 0;
  };

  /// Cached, static metadata of one fan channel (fanN_*).
  struct FanChannel {
    std::string device;
    std::string label;
    std::string input_path;  // fanN_input
    std::size_t index = 0;
  };

  std::vector<std::string> discovered_hwmon_;  // "hwmonN" dirs at last discovery
  std::vector<TemperatureChannel> temp_channels_;
  std::vector<FanChannel> fan_channels_;
  bool hwmon_present_ = false;  // /sys/class/hwmon existed at last discovery
};

}  // namespace atm