#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace atm {

/// Overall health classification of one physical disk.
///
/// A disk is only ever classified `Healthy` when a real read-only health
/// source reported a passing result. Unsupported devices, permission-denied
/// accesses and unreadable devices are never labelled healthy.
enum class DiskHealthStatus {
  Healthy,
  Warning,
  Failing,
  Unsupported,
  PermissionDenied,
  Unavailable,
  Unknown,
};

/// Human-readable name of a DiskHealthStatus ("HEALTHY", "UNSUPPORTED", ...).
[[nodiscard]] const char *diskHealthStatusName(DiskHealthStatus status);

/// Which read-only health source produced a result (or that none could).
enum class DiskHealthSource {
  None,      // no source applies / result was not produced
  AtaSmart,  // ATA/SATA SMART log via libatasmart
  Nvme,      // NVMe SMART/Health Information log via the kernel ioctl
};

[[nodiscard]] const char *diskHealthSourceName(DiskHealthSource source);

/// Coarse, non-sensitive error category for a health refresh attempt. Details
/// never include device paths, serial numbers or raw payload bytes.
enum class HealthErrorCategory {
  None,
  Unsupported,
  PermissionDenied,
  DeviceUnavailable,
  TransportError,
  ParseError,
  Timeout,
  Unknown,
};

[[nodiscard]] const char *healthErrorCategoryName(HealthErrorCategory category);

/// Maps a health error category onto the status shown for a device (e.g.
/// device gone -> Unavailable, permission denied -> PermissionDenied,
/// unsupported transport -> Unsupported).
[[nodiscard]] DiskHealthStatus healthStatusForError(HealthErrorCategory category);

/// An ATA SMART raw value is six bytes (48 bits).
inline constexpr std::size_t kSmartRawLength = 6;

/// A standard SMART attribute log is bounded to 30 attribute entries.
inline constexpr std::size_t kMaxSmartAttributes = 30;

/// Parses the conventional least-significant-first 48-bit integer out of the
/// six raw attribute bytes. Most vendors store the value low byte first; when
/// the vendor's encoding is unknown the raw bytes are preserved (`raw` /
/// `raw_hex`) and this conventional parse must not be treated as authority.
[[nodiscard]] std::optional<std::uint64_t> parseSmartRawValue(
    const std::array<std::uint8_t, kSmartRawLength> &raw);

/// Formats the six raw bytes as uppercased hex text ("0x0123456789AB").
[[nodiscard]] std::string formatSmartRawHex(
    const std::array<std::uint8_t, kSmartRawLength> &raw);

/// One normalized ATA/SATA SMART attribute parsed from a device log.
struct SmartAttribute {
  std::uint8_t id = 0;
  std::string name;                          // empty when the vendor ID is unknown
  bool name_known = false;
  std::optional<std::uint8_t> current;       // normalized current value
  std::optional<std::uint8_t> worst;         // normalized worst (historical) value
  std::optional<std::uint8_t> threshold;     // failing threshold
  std::array<std::uint8_t, kSmartRawLength> raw{};  // raw 48-bit payload, verbatim
  std::string raw_hex;                       // "0x..." rendering of `raw`
  std::optional<std::uint64_t> raw_value;    // conventional LE parse of `raw`
  std::optional<std::uint64_t> pretty_value; // interpreter's unit-aware value
  std::string unit;                          // e.g. "sectors", "mseconds"; empty = unknown
  bool normalized = true;                    // false when current/worst are not meaningful
  bool warning = false;                      // attribute currently crossing its limit
  bool prefailure = false;                   // pre-failure (vs. advisory) flag
  bool online = false;                       // online (vs. offline) test flag
};

/// Builds a SmartAttribute from fully decoded parts. Pure so ATA attribute
/// handling is unit-testable without a real device.
[[nodiscard]] SmartAttribute makeSmartAttribute(
    std::uint8_t id, std::string name, bool name_known,
    std::optional<std::uint8_t> current, std::optional<std::uint8_t> worst,
    std::optional<std::uint8_t> threshold,
    std::array<std::uint8_t, kSmartRawLength> raw,
    std::optional<std::uint64_t> pretty_value, std::string unit,
    bool normalized, bool warning, bool prefailure, bool online);

/// NVMe SMART/Health Information log page size in bytes (log ID 0x02).
inline constexpr std::size_t kNvmeSmartLogSize = 512;

/// Reads the conventional little-endian 128-bit value from a 16-byte NVMe log
/// field. Returns std::nullopt when the value does not fit in 64 bits (rare in
/// practice) so callers degrade gracefully instead of truncating silently.
[[nodiscard]] std::optional<std::uint64_t> nvmeReadU128(
    const std::array<std::uint8_t, 16> &data);

/// NVMe "data units" are reported as counts of 1000 x 512-byte units, i.e.
/// each unit is 512 * 1000 = 512000 bytes. Conversion saturates at UINT64_MAX.
[[nodiscard]] std::uint64_t nvmeDataUnitsToBytes(std::uint64_t units);

/// NVMe composite temperature is a 16-bit value in Kelvin.
[[nodiscard]] double nvmeKelvinToCelsius(std::uint16_t kelvin);

/// Human-readable names of the NVMe critical-warning flags set in `flags`
/// (spare low, reliability degraded, ...). Empty when no flags are set.
[[nodiscard]] std::vector<std::string> nvmeCriticalWarnings(std::uint8_t flags);

/// Fields of the NVMe SMART/Health log (LID 0x02). Every value uses the exact
/// NVMe specification units; conversions are documented on the field. A value
/// of std::nullopt means the field was absent, unsupported or invalid.
struct NvmeHealthLog {
  std::optional<std::uint8_t> critical_warning;        // bitmask, see nvmeCriticalWarnings
  std::optional<double> composite_temperature_celsius; // Kelvin -> Celsius
  std::optional<std::uint8_t> available_spare;         // percent of spare capacity (not free space)
  std::optional<std::uint8_t> available_spare_threshold;  // percent
  std::optional<std::uint8_t> percentage_used;         // life consumed in percent (not remaining)
  std::optional<std::uint64_t> data_units_read;    // thousands of 512-byte units (see nvmeDataUnitsToBytes)
  std::optional<std::uint64_t> data_units_written; //         "              "
  std::optional<std::uint64_t> host_read_commands;
  std::optional<std::uint64_t> host_write_commands;
  std::optional<std::uint64_t> controller_busy_time_minutes;  // I/O busy indicator, never CPU usage
  std::optional<std::uint64_t> power_cycles;
  std::optional<std::uint64_t> power_on_hours;
  std::optional<std::uint64_t> unsafe_shutdowns;
  std::optional<std::uint64_t> media_and_data_integrity_errors;  // controller media errors
  std::optional<std::uint64_t> error_information_log_entries;
  std::optional<std::uint64_t> warning_temperature_time_minutes;  // minutes (spec)
  std::optional<std::uint64_t> critical_temperature_time_minutes; // minutes (spec)
};

/// Outcome of parsing a raw NVMe health log page.
struct NvmeParseResult {
  bool valid = false;
  HealthErrorCategory error = HealthErrorCategory::ParseError;
  NvmeHealthLog log;
};

/// Parses a raw NVMe SMART/Health log page. `valid` is false (with the error
/// category set to ParseError) when the buffer is shorter than 512 bytes or a
/// mandatory field is out of range; readable fields are still populated.
[[nodiscard]] NvmeParseResult parseNvmeHealthLog(const std::uint8_t *data,
                                                 std::size_t size);

/// Stable device identity captured from sysfs, used to detect a device
/// disappearing or being replaced between discovery and health retrieval.
struct DeviceIdentity {
  std::uint64_t major = 0;
  std::uint64_t minor = 0;
  std::string sysfs_id;  // stable driver topology identity where available

  bool valid() const { return major != 0 || minor != 0 || !sysfs_id.empty(); }
  bool operator==(const DeviceIdentity &other) const {
    return major == other.major && minor == other.minor &&
           sysfs_id == other.sysfs_id;
  }
  bool operator!=(const DeviceIdentity &other) const {
    return !(*this == other);
  }
};

/// Reads major/minor from `/sys/class/block/<name>/dev` (format "8:0") and the
/// stable `device` topology symlink target under `root`. Returns std::nullopt
/// when the device vanished between discovery and retrieval.
[[nodiscard]] std::optional<DeviceIdentity> readDeviceIdentity(
    const std::string &name, const std::filesystem::path &root = "/");

/// Confirms the device at `name` still matches `expected` by re-reading its
/// identity. Returns false when the device disappeared or was replaced.
[[nodiscard]] bool deviceIdentityUnchanged(
    const std::string &name, const DeviceIdentity &expected,
    const std::filesystem::path &root = "/");

/// Non-sensitive model/vendor metadata from sysfs. Serial numbers are never
/// returned — only whether a serial is present, so device identity is not
/// logged or exported by default.
struct DiskMetadata {
  std::string vendor;
  std::string model;
  std::string firmware_revision;
  bool serial_available = false;
  std::uint64_t logical_block_size = 0;
  std::uint64_t physical_block_size = 0;
};

/// Reads `/sys/block/<name>/device/{model,vendor,rev,serial}` and the queue
/// logical/physical block sizes under `root`. Missing files leave fields empty.
[[nodiscard]] DiskMetadata readDeviceMetadata(
    const std::string &name, const std::filesystem::path &root = "/");

/// Selects the health source for a whole-disk kernel name: NVMe controllers
/// use the NVMe health log, everything else is attempted as ATA/SATA.
[[nodiscard]] DiskHealthSource diskHealthSourceForDevice(const std::string &name);

/// Complete read-only health snapshot of one physical disk.
///
/// Health values are optional; a field the source does not provide or cannot
/// read stays std::nullopt. Nothing is ever fabricated, and no disk is
/// classified healthy merely because SMART could not be read.
struct DiskHealthSnapshot {
  bool ok = false;             // true when a real health log was read and parsed
  std::string device_name;     // kernel name (sda, nvme0n1, ...)
  std::string device_path;     // /dev node used by the provider
  std::string vendor;
  std::string model;
  std::string firmware_revision;
  bool serial_available = false;  // serial exists but is never persisted/logged
  std::uint64_t major = 0;
  std::uint64_t minor = 0;
  DiskHealthSource source = DiskHealthSource::None;
  DiskHealthStatus status = DiskHealthStatus::Unknown;
  HealthErrorCategory error = HealthErrorCategory::None;
  std::string detail;  // short, non-sensitive explanation of status/error
  std::chrono::system_clock::time_point refreshed_at{};

  // --- ATA/SATA SMART ----------------------------------------------------
  std::optional<bool> smart_self_assessment;  // SMART overall self-assessment pass/fail
  std::string smart_overall;                  // interpreter's overall status text
  std::optional<double> temperature_celsius;
  std::optional<std::uint64_t> power_on_hours;
  std::optional<std::uint64_t> power_cycles;
  std::optional<std::uint64_t> reallocated_sectors;
  std::optional<std::uint64_t> current_pending_sectors;
  std::optional<std::uint64_t> offline_uncorrectable_sectors;
  std::optional<std::uint64_t> reported_uncorrectable_errors;
  std::optional<std::uint64_t> unsafe_shutdowns;
  bool attributes_truncated = false;  // true when the attribute list was capped
  std::vector<SmartAttribute> attributes;

  // --- NVMe SMART/Health -------------------------------------------------
  std::optional<NvmeHealthLog> nvme;
};

/// Read-only health provider interface.
///
/// Providers never modify the device: no SMART enable/disable, no self-test
/// start, no firmware update, no format, no secure erase, no power-state
/// change. `base` carries the identity/metadata the coordinator already filled
/// in; the provider adds the health-specific fields and returns it.
class DiskHealthProvider {
 public:
  virtual ~DiskHealthProvider() = default;
  virtual DiskHealthSnapshot readHealth(const DiskHealthSnapshot &base) = 0;
  virtual const char *name() const = 0;
};

/// ATA/SATA provider backed by libatasmart (read-only by design).
class AtaSmartProvider : public DiskHealthProvider {
 public:
  DiskHealthSnapshot readHealth(const DiskHealthSnapshot &base) override;
  const char *name() const override { return "ata-smart"; }
};

/// NVMe provider using the kernel's NVMe admin "Get Log Page" ioctl for the
/// SMART/Health log (LID 0x02) on the controller char device.
class NvmeHealthProvider : public DiskHealthProvider {
 public:
  DiskHealthSnapshot readHealth(const DiskHealthSnapshot &base) override;
  const char *name() const override { return "nvme-health"; }
};

/// Creates the default provider for a whole-disk name (NVMe for `nvme*`
/// devices, ATA/SATA otherwise). Always returns a provider; never null.
[[nodiscard]] std::unique_ptr<DiskHealthProvider> defaultDiskHealthProviderFactory(
    const std::string &name);

/// The backoff window during which devices that recently reported
/// Unsupported/PermissionDenied are not retried on non-forced refreshes.
inline constexpr auto kHealthRetryBackoff = std::chrono::seconds(30);

/// True when a non-forced refresh should skip `category` because it was
/// attempted recently (unsupported/permission-denied are not hammered).
[[nodiscard]] bool withinRetryBackoff(
    HealthErrorCategory category,
    std::chrono::steady_clock::time_point last_attempt,
    std::chrono::steady_clock::time_point now);

/// Cached, UI-facing state of one device held by DiskHealthMonitor.
struct DiskHealthCacheEntry {
  std::string device_name;
  DiskHealthSnapshot state;  // the snapshot to render
  std::shared_ptr<const DiskHealthSnapshot> last_success;  // last fully-successful result
  std::optional<std::chrono::system_clock::time_point> last_success_at;
  std::optional<std::chrono::system_clock::time_point> last_attempt_at;
  std::string last_error;                       // short safe failure text
  HealthErrorCategory last_error_category = HealthErrorCategory::None;
  std::string provider;                         // provider name used, if any
};

/// Monitors disk health entirely off the UI thread.
///
/// Device discovery is reused from DiskMonitor: the application feeds the
/// whole-disk list through `setDevices()` and health is only refreshed on
/// request (startup once, plus explicit manual refreshes). SMART/NVMe reads
/// never run on a monitoring tick, duplicate concurrent requests are refused,
/// stale results are discarded when a device disappears or is replaced, and
/// the last successful result is preserved while a refresh runs.
class DiskHealthMonitor {
 public:
  using ProviderFactory = std::function<std::unique_ptr<DiskHealthProvider>(
      const std::string &name)>;
  using IdentityReader =
      std::function<std::optional<DeviceIdentity>(const std::string &name)>;
  using IdentityCheck =
      std::function<bool(const std::string &name, const DeviceIdentity &expected)>;

  /// Uses the default provider factory and a live-sysfs identity reader when
  /// not supplied (tests inject fakes).
  explicit DiskHealthMonitor(ProviderFactory provider_factory = {},
                             IdentityReader identity_reader = {},
                             IdentityCheck identity_check = {});

  ~DiskHealthMonitor();

  DiskHealthMonitor(const DiskHealthMonitor &) = delete;
  DiskHealthMonitor &operator=(const DiskHealthMonitor &) = delete;

  /// Replaces the monitored whole-disk device set (from DiskMonitor). Cheap;
  /// no health reads are issued here. Caches for vanished devices are pruned.
  void setDevices(std::vector<std::string> devices);

  /// Requests one full background refresh cycle over all configured devices.
  /// Returns false when a refresh is already running (no overlapping
  /// requests). `force` bypasses the unsupported/permission-denied backoff.
  bool requestRefresh(bool force = true);

  /// True while a refresh cycle runs in the background worker.
  bool refreshing() const;

  /// Devices currently monitored, sorted by name.
  [[nodiscard]] std::vector<std::string> deviceNames() const;

  /// Cached state for one device (std::nullopt when not monitored).
  [[nodiscard]] std::optional<DiskHealthCacheEntry> entryFor(
      const std::string &name) const;

  /// Cached states for every monitored device, sorted by name. Never blocks on
  /// a running refresh and never issues health reads.
  [[nodiscard]] std::vector<DiskHealthCacheEntry> entries() const;

  /// Blocks until any in-flight refresh cycle completes (used by tests).
  void waitForIdle() const;

 private:
  void workerLoop();
  void runCycle(const std::vector<std::string> &devices, bool force);
  DiskHealthSnapshot readOne(const std::string &name);

  mutable std::mutex mutex_;
  mutable std::condition_variable cv_;
  std::thread worker_;
  bool stop_ = false;
  bool refresh_pending_ = false;
  bool forced_ = true;
  bool refreshing_ = false;

  std::vector<std::string> devices_;
  std::unordered_map<std::string, DiskHealthCacheEntry> results_;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point>
      attempts_;
  std::unordered_map<std::string, HealthErrorCategory> attempt_category_;

  ProviderFactory provider_factory_;
  IdentityReader identity_reader_;
  IdentityCheck identity_check_;
};

}  // namespace atm