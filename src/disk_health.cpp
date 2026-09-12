#include "disk_health.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <linux/nvme_ioctl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <atasmart.h>

namespace atm {

namespace {

namespace fs = std::filesystem;

/// Maximum attribute entries kept per device (SMART logs are bounded to 30).
constexpr std::size_t kAttributeCap = kMaxSmartAttributes;

/// NVMe admin opcode for the "Get Log Page" command.
constexpr std::uint8_t kNvmeAdminGetLogPage = 0x02;
/// NVMe log identifier of the SMART/Health Information log.
constexpr std::uint32_t kNvmeLogSmart = 0x02;
/// NSID used for the controller-global SMART log (0xFFFFFFFF).
constexpr std::uint32_t kNvmeNsidAll = 0xFFFFFFFFU;
/// Bytes returned for the SMART/Health log.
constexpr std::size_t kNvmeLogLength = kNvmeSmartLogSize;

/// Attempts to map a raw errno value onto a health error category. Values that
/// do not map cleanly become TransportError (device-level failure) — never a
/// success category.
HealthErrorCategory categorizeErrno(int errno_value) {
  switch (errno_value) {
    case EACCES:
    case EPERM:
      return HealthErrorCategory::PermissionDenied;
    case ENOENT:
    case ENODEV:
    case ENXIO:
      return HealthErrorCategory::DeviceUnavailable;
    case ENOTTY:
    case EOPNOTSUPP:
    case EINVAL:
      return HealthErrorCategory::Unsupported;
    case EBUSY:
    case EIO:
    case ENOMEM:
      return HealthErrorCategory::TransportError;
    case ETIMEDOUT:
      return HealthErrorCategory::Timeout;
    default:
      return HealthErrorCategory::Unknown;
  }
}

/// Builds a short, non-sensitive detail message from an error category.
std::string detailForCategory(HealthErrorCategory category) {
  switch (category) {
    case HealthErrorCategory::None:
      return {};
    case HealthErrorCategory::Unsupported:
      return "This device does not support readable health data";
    case HealthErrorCategory::PermissionDenied:
      return "Permission denied (the user needs access to the device node)";
    case HealthErrorCategory::DeviceUnavailable:
      return "The device disappeared or was replaced";
    case HealthErrorCategory::TransportError:
      return "The device did not answer the health request";
    case HealthErrorCategory::ParseError:
      return "The returned health data could not be interpreted";
    case HealthErrorCategory::Timeout:
      return "The health request timed out";
    case HealthErrorCategory::Unknown:
      return "The health request failed for an unknown reason";
  }
  return "The health request failed";
}

/// Trims ASCII whitespace off both ends of a string.
std::string trimAscii(std::string text) {
  const auto not_space = [](unsigned char ch) { return ch > ' '; };
  const auto first = std::find_if(text.begin(), text.end(), not_space);
  if (first == text.end()) {
    return {};
  }
  auto last = std::find_if(text.rbegin(), text.rend(), not_space).base();
  return std::string(first, last);
}

/// Strips trailing NUL bytes off a libatasmart identify field.
std::string identifyField(const char *field, std::size_t max) {
  if (field == nullptr) {
    return {};
  }
  std::string value(field, std::min(max, ::strnlen(field, max)));
  return trimAscii(std::move(value));
}

/// Parses a major:minor string ("8:0") into device numbers.
bool parseDevNodes(const std::string &text, std::uint64_t &major,
                   std::uint64_t &minor) {
  const std::size_t colon = text.find(':');
  if (colon == std::string::npos) {
    return false;
  }
  try {
    std::size_t parsed_major = 0;
    std::size_t parsed_minor = 0;
    const std::string major_text = text.substr(0, colon);
    const std::string minor_text = text.substr(colon + 1);
    const std::size_t major_len = std::stoull(major_text, &parsed_major);
    const std::size_t minor_len = std::stoull(minor_text, &parsed_minor);
    if (parsed_major != major_text.size() || parsed_minor != minor_text.size() ||
        major_text.empty() || minor_text.empty()) {
      return false;
    }
    major = static_cast<std::uint64_t>(major_len);
    minor = static_cast<std::uint64_t>(minor_len);
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

/// Reads a small text file (the first line) into `out`. Returns false on any
/// open/read failure.
bool readTextFile(const fs::path &path, std::string &out) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return false;
  }
  return static_cast<bool>(std::getline(file, out));
}

/// The NVMe controller char device is the block name minus its trailing
/// namespace suffix ("nvme0n1" -> "/dev/nvme0"). Returns empty text for names
/// that do not follow the "nvme<controller>n<namespace>" pattern.
std::string nvmeControllerPath(const std::string &block_name) {
  if (block_name.rfind("nvme", 0) != 0) {
    return {};
  }
  const std::size_t n = block_name.find('n', 4);  // first 'n' after "nvme"
  if (n == std::string::npos || n + 1 >= block_name.size()) {
    return {};
  }
  const std::string controller = block_name.substr(0, n);
  if (controller == "nvme") {
    return {};
  }
  return "/dev/" + controller;
}

/// Marks `snapshot` as failed with a category/status/detail before returning.
DiskHealthSnapshot failSnapshot(DiskHealthSnapshot snap,
                                HealthErrorCategory category,
                                const std::string &detail) {
  snap.ok = false;
  snap.error = category;
  snap.status = healthStatusForError(category);
  snap.detail = detail.empty() ? detailForCategory(category) : detail;
  return snap;
}

/// Converts a libatasmart millikelvin temperature into degrees Celsius.
double mKelvinToCelsius(std::uint64_t mkelvin) {
  return (static_cast<double>(mkelvin) - 273150.0) / 1000.0;
}

}  // namespace

// --- Enums and classifications ------------------------------------------

const char *diskHealthStatusName(DiskHealthStatus status) {
  switch (status) {
    case DiskHealthStatus::Healthy:
      return "Healthy";
    case DiskHealthStatus::Warning:
      return "Warning";
    case DiskHealthStatus::Failing:
      return "Failing";
    case DiskHealthStatus::Unsupported:
      return "Unsupported";
    case DiskHealthStatus::PermissionDenied:
      return "Permission Denied";
    case DiskHealthStatus::Unavailable:
      return "Unavailable";
    case DiskHealthStatus::Unknown:
      return "Unknown";
  }
  return "Unknown";
}

const char *diskHealthSourceName(DiskHealthSource source) {
  switch (source) {
    case DiskHealthSource::None:
      return "N/A";
    case DiskHealthSource::AtaSmart:
      return "ATA/SMART";
    case DiskHealthSource::Nvme:
      return "NVMe";
  }
  return "N/A";
}

const char *healthErrorCategoryName(HealthErrorCategory category) {
  switch (category) {
    case HealthErrorCategory::None:
      return "None";
    case HealthErrorCategory::Unsupported:
      return "Unsupported";
    case HealthErrorCategory::PermissionDenied:
      return "PermissionDenied";
    case HealthErrorCategory::DeviceUnavailable:
      return "DeviceUnavailable";
    case HealthErrorCategory::TransportError:
      return "TransportError";
    case HealthErrorCategory::ParseError:
      return "ParseError";
    case HealthErrorCategory::Timeout:
      return "Timeout";
    case HealthErrorCategory::Unknown:
      return "Unknown";
  }
  return "Unknown";
}

DiskHealthStatus healthStatusForError(HealthErrorCategory category) {
  switch (category) {
    case HealthErrorCategory::None:
      return DiskHealthStatus::Unknown;
    case HealthErrorCategory::Unsupported:
      return DiskHealthStatus::Unsupported;
    case HealthErrorCategory::PermissionDenied:
      return DiskHealthStatus::PermissionDenied;
    case HealthErrorCategory::DeviceUnavailable:
    case HealthErrorCategory::TransportError:
    case HealthErrorCategory::ParseError:
    case HealthErrorCategory::Timeout:
    case HealthErrorCategory::Unknown:
      return DiskHealthStatus::Unavailable;
  }
  return DiskHealthStatus::Unknown;
}

// --- ATA SMART attribute helpers ----------------------------------------

std::optional<std::uint64_t> parseSmartRawValue(
    const std::array<std::uint8_t, kSmartRawLength> &raw) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < kSmartRawLength; ++i) {
    value |= static_cast<std::uint64_t>(raw[i]) << (8 * i);
  }
  return value;
}

std::string formatSmartRawHex(
    const std::array<std::uint8_t, kSmartRawLength> &raw) {
  std::ostringstream out;
  out << "0x" << std::hex << std::uppercase << std::setfill('0');
  for (const std::uint8_t byte : raw) {
    out << std::setw(2) << static_cast<unsigned>(byte);
  }
  return out.str();
}

SmartAttribute makeSmartAttribute(
    std::uint8_t id, std::string name, bool name_known,
    std::optional<std::uint8_t> current, std::optional<std::uint8_t> worst,
    std::optional<std::uint8_t> threshold,
    std::array<std::uint8_t, kSmartRawLength> raw,
    std::optional<std::uint64_t> pretty_value, std::string unit,
    bool normalized, bool warning, bool prefailure, bool online) {
  SmartAttribute attribute;
  attribute.id = id;
  attribute.name = std::move(name);
  attribute.name_known = name_known;
  attribute.current = current;
  attribute.worst = worst;
  attribute.threshold = threshold;
  attribute.raw = raw;
  attribute.raw_hex = formatSmartRawHex(raw);
  attribute.raw_value = parseSmartRawValue(raw);
  attribute.pretty_value = pretty_value;
  attribute.unit = std::move(unit);
  attribute.normalized = normalized;
  attribute.warning = warning;
  attribute.prefailure = prefailure;
  attribute.online = online;
  return attribute;
}

// --- NVMe log parsing ----------------------------------------------------

std::optional<std::uint64_t> nvmeReadU128(
    const std::array<std::uint8_t, 16> &data) {
  // Detect overflow against the upper 64 bits before folding them in.
  for (std::size_t i = 8; i < 16; ++i) {
    if (data[i] != 0) {
      return std::nullopt;  // exceeds 64 bits — do not truncate silently
    }
  }
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(data[i]) << (8 * i);
  }
  return value;
}

std::uint64_t nvmeDataUnitsToBytes(std::uint64_t units) {
  constexpr std::uint64_t kBytesPerUnit = 512 * 1000;  // 512-byte sectors, in thousands
  if (units > static_cast<std::uint64_t>(UINT64_MAX) / kBytesPerUnit) {
    return UINT64_MAX;  // saturate
  }
  return units * kBytesPerUnit;
}

double nvmeKelvinToCelsius(std::uint16_t kelvin) {
  return static_cast<double>(kelvin) - 273.15;
}

std::vector<std::string> nvmeCriticalWarnings(std::uint8_t flags) {
  static constexpr std::array kNames = {
      "Available spare capacity is below the threshold",
      "Temperature has exceeded an over-temperature threshold",
      "Reliability has been degraded",
      "The controller is in a read-only mode",
      "The volatile memory backup device has failed",
  };
  std::vector<std::string> warnings;
  for (std::size_t bit = 0; bit < kNames.size(); ++bit) {
    if ((flags & (1U << bit)) != 0) {
      warnings.push_back(kNames[bit]);
    }
  }
  return warnings;
}

namespace {

/// Offsets of the NVMe SMART/Health log fields (NVMe spec, Figure 233 style).
constexpr std::size_t kNvmeOffsetCriticalWarning = 0;
constexpr std::size_t kNvmeOffsetTemperature = 1;      // 16-bit LE Kelvin
constexpr std::size_t kNvmeOffsetAvailSpare = 3;
constexpr std::size_t kNvmeOffsetSpareThresh = 4;
constexpr std::size_t kNvmeOffsetPercentUsed = 5;
constexpr std::size_t kNvmeOffsetDataUnitsRead = 32;    // 16-byte LE u128
constexpr std::size_t kNvmeOffsetDataUnitsWritten = 48;
constexpr std::size_t kNvmeOffsetHostReads = 64;
constexpr std::size_t kNvmeOffsetHostWrites = 80;
constexpr std::size_t kNvmeOffsetBusyTime = 96;
constexpr std::size_t kNvmeOffsetPowerCycles = 112;
constexpr std::size_t kNvmeOffsetPowerOnHours = 128;
constexpr std::size_t kNvmeOffsetUnsafeShutdowns = 144;
constexpr std::size_t kNvmeOffsetMediaErrors = 160;
constexpr std::size_t kNvmeOffsetErrorLogEntries = 176;
constexpr std::size_t kNvmeOffsetWarningTempTime = 192;  // 32-bit LE minutes
constexpr std::size_t kNvmeOffsetCriticalTempTime = 196; // 32-bit LE minutes

/// Reads a little-endian u16 from a byte buffer.
std::uint16_t readU16LE(const std::uint8_t *data) {
  return static_cast<std::uint16_t>(data[0]) |
         (static_cast<std::uint16_t>(data[1]) << 8);
}

/// Reads a little-endian u32 from a byte buffer.
std::uint32_t readU32LE(const std::uint8_t *data) {
  return static_cast<std::uint32_t>(data[0]) |
         (static_cast<std::uint32_t>(data[1]) << 8) |
         (static_cast<std::uint32_t>(data[2]) << 16) |
         (static_cast<std::uint32_t>(data[3]) << 24);
}

/// Reads a 16-byte little-endian u128 field as a bounded u64.
std::optional<std::uint64_t> readU128Field(const std::uint8_t *data) {
  std::array<std::uint8_t, 16> field{};
  std::copy(data, data + 16, field.begin());
  return nvmeReadU128(field);
}

}  // namespace

NvmeParseResult parseNvmeHealthLog(const std::uint8_t *data, std::size_t size) {
  NvmeParseResult result;
  if (data == nullptr || size < kNvmeSmartLogSize) {
    result.valid = false;
    result.error = HealthErrorCategory::ParseError;
    return result;
  }

  NvmeHealthLog log;
  log.critical_warning = data[kNvmeOffsetCriticalWarning];

  const std::uint16_t kelvin = readU16LE(data + kNvmeOffsetTemperature);
  if (kelvin != 0) {  // 0 Kelvin is reported for unsupported temperature
    log.composite_temperature_celsius = nvmeKelvinToCelsius(kelvin);
  }

  const std::uint8_t avail_spare = data[kNvmeOffsetAvailSpare];
  if (avail_spare <= 100) {
    log.available_spare = avail_spare;
  }
  const std::uint8_t spare_thresh = data[kNvmeOffsetSpareThresh];
  if (spare_thresh <= 100) {
    log.available_spare_threshold = spare_thresh;
  }
  const std::uint8_t percent_used = data[kNvmeOffsetPercentUsed];
  if (percent_used <= 100) {
    log.percentage_used = percent_used;
  }

  log.data_units_read = readU128Field(data + kNvmeOffsetDataUnitsRead);
  log.data_units_written = readU128Field(data + kNvmeOffsetDataUnitsWritten);
  log.host_read_commands = readU128Field(data + kNvmeOffsetHostReads);
  log.host_write_commands = readU128Field(data + kNvmeOffsetHostWrites);
  log.controller_busy_time_minutes =
      readU128Field(data + kNvmeOffsetBusyTime);
  log.power_cycles = readU128Field(data + kNvmeOffsetPowerCycles);
  log.power_on_hours = readU128Field(data + kNvmeOffsetPowerOnHours);
  log.unsafe_shutdowns = readU128Field(data + kNvmeOffsetUnsafeShutdowns);
  log.media_and_data_integrity_errors =
      readU128Field(data + kNvmeOffsetMediaErrors);
  log.error_information_log_entries =
      readU128Field(data + kNvmeOffsetErrorLogEntries);

  log.warning_temperature_time_minutes =
      readU32LE(data + kNvmeOffsetWarningTempTime);
  log.critical_temperature_time_minutes =
      readU32LE(data + kNvmeOffsetCriticalTempTime);

  // A buffer that produced no readable field at all is not a valid health log.
  if (!log.critical_warning.has_value() &&
      !log.composite_temperature_celsius.has_value() &&
      !log.available_spare.has_value() && !log.percentage_used.has_value() &&
      !log.data_units_read.has_value() && !log.data_units_written.has_value()) {
    result.valid = false;
    result.error = HealthErrorCategory::ParseError;
    return result;
  }

  result.valid = true;
  result.error = HealthErrorCategory::None;
  result.log = std::move(log);
  return result;
}

// --- Device identity and metadata -----------------------------------------

std::optional<DeviceIdentity> readDeviceIdentity(
    const std::string &name, const std::filesystem::path &root) {
  DeviceIdentity identity;

  std::string dev_nodes;
  if (readTextFile(root / "sys" / "class" / "block" / name / "dev",
                   dev_nodes)) {
    if (!parseDevNodes(trimAscii(dev_nodes), identity.major, identity.minor)) {
      return std::nullopt;  // malformed "<major>:<minor>" — treat as gone
    }
  }

  // Stable driver-topology identity: /sys/class/block/<name>/device is a
  // symlink such as ../../devices/pci0000:00/.../block/sda.
  try {
    const fs::path device_link =
        root / "sys" / "class" / "block" / name / "device";
    std::error_code ec;
    if (fs::exists(device_link, ec) && fs::is_symlink(device_link)) {
      identity.sysfs_id =
          fs::read_symlink(device_link).lexically_normal().string();
    }
  } catch (const std::exception &) {
    // sysfs_id stays empty; the major/minor pair still anchors the identity.
  }

  if (!identity.valid()) {
    return std::nullopt;  // device vanished before any identity was captured
  }
  return identity;
}

bool deviceIdentityUnchanged(const std::string &name,
                             const DeviceIdentity &expected,
                             const std::filesystem::path &root) {
  const std::optional<DeviceIdentity> current = readDeviceIdentity(name, root);
  return current.has_value() && *current == expected;
}

DiskMetadata readDeviceMetadata(const std::string &name,
                                const std::filesystem::path &root) {
  DiskMetadata metadata;
  const fs::path base = root / "sys" / "block" / name / "device";
  std::string text;
  if (readTextFile(base / "model", text)) {
    metadata.model = trimAscii(text);
  }
  if (readTextFile(base / "vendor", text)) {
    metadata.vendor = trimAscii(text);
  }
  if (readTextFile(base / "rev", text)) {
    metadata.firmware_revision = trimAscii(text);
  }
  if (readTextFile(base / "serial", text)) {
    metadata.serial_available = !trimAscii(text).empty();
  }

  const auto readU64 = [&](const fs::path &path) -> std::uint64_t {
    std::string value;
    if (!readTextFile(path, value)) {
      return 0;
    }
    std::uint64_t parsed = 0;
    try {
      std::size_t consumed = 0;
      parsed = std::stoull(trimAscii(value), &consumed);
      if (consumed == 0) {
        return 0;
      }
    } catch (const std::exception &) {
      return 0;
    }
    return parsed;
  };
  const fs::path queue = root / "sys" / "block" / name / "queue";
  metadata.logical_block_size = readU64(queue / "logical_block_size");
  metadata.physical_block_size = readU64(queue / "physical_block_size");
  return metadata;
}

DiskHealthSource diskHealthSourceForDevice(const std::string &name) {
  if (name.rfind("nvme", 0) == 0) {
    return DiskHealthSource::Nvme;
  }
  return DiskHealthSource::AtaSmart;
}

bool withinRetryBackoff(HealthErrorCategory category,
                        std::chrono::steady_clock::time_point last_attempt,
                        std::chrono::steady_clock::time_point now) {
  if (category != HealthErrorCategory::Unsupported &&
      category != HealthErrorCategory::PermissionDenied) {
    return false;
  }
  return now - last_attempt < kHealthRetryBackoff;
}

// --- Providers ------------------------------------------------------------

namespace {

/// libatasmart attribute gather (the library calls back once per attribute).
struct AttributeGather {
  std::vector<SmartAttribute> attributes;
};

void gatherSmartAttribute(SkDisk * /*disk*/,
                          const SkSmartAttributeParsedData *a, void *userdata) {
  auto *gather = static_cast<AttributeGather *>(userdata);
  if (gather == nullptr || a == nullptr ||
      gather->attributes.size() >= kAttributeCap) {
    return;
  }

  const char *unit_text = sk_smart_attribute_unit_to_string(a->pretty_unit);
  const bool unit_known = unit_text != nullptr && ::strcmp(unit_text, "Unknown") != 0;

  std::optional<std::uint64_t> pretty_value;
  if (unit_known) {
    pretty_value = a->pretty_value;
  }

  std::array<std::uint8_t, kSmartRawLength> raw{};
  std::copy(std::begin(a->raw), std::end(a->raw), raw.begin());

  const std::string name = a->name != nullptr ? a->name : "";
  std::optional<std::uint8_t> current;
  if (a->current_value_valid) {
    current = a->current_value;
  }
  std::optional<std::uint8_t> worst;
  if (a->worst_value_valid) {
    worst = a->worst_value;
  }
  std::optional<std::uint8_t> threshold;
  if (a->threshold_valid) {
    threshold = a->threshold;
  }

  gather->attributes.push_back(makeSmartAttribute(
      a->id, name, !name.empty(), current, worst, threshold, raw, pretty_value,
      unit_known ? std::string(unit_text) : std::string(), unit_known,
      a->warn != FALSE, a->prefailure != FALSE, a->online != FALSE));
}

}  // namespace

DiskHealthSnapshot AtaSmartProvider::readHealth(const DiskHealthSnapshot &base) {
  DiskHealthSnapshot snap = base;
  snap.source = DiskHealthSource::AtaSmart;

  SkDisk *disk = nullptr;
  const int open_rc = sk_disk_open(snap.device_path.c_str(), &disk);
  if (open_rc != 0) {
    const int err = open_rc < 0 ? -open_rc : open_rc;
    return failSnapshot(std::move(snap), categorizeErrno(err), {});
  }

  struct DiskGuard {
    SkDisk *disk;
    ~DiskGuard() {
      if (disk != nullptr) {
        sk_disk_free(disk);
      }
    }
  } guard{disk};

  // --- Identification (model/serial/firmware). Serial presence is recorded
  // --- but the value is never copied into the snapshot or logs.
  const SkIdentifyParsedData *identify = nullptr;
  if (sk_disk_identify_parse(disk, &identify) == 0 && identify != nullptr) {
    if (snap.model.empty()) {
      snap.model = identifyField(identify->model, sizeof(identify->model));
    }
    if (snap.firmware_revision.empty()) {
      snap.firmware_revision =
          identifyField(identify->firmware, sizeof(identify->firmware));
    }
    snap.serial_available = identify->serial[0] != '\0';
  }

  // --- SMART availability.
  SkBool available = FALSE;
  if (sk_disk_smart_is_available(disk, &available) != 0 || !available) {
    return failSnapshot(std::move(snap), HealthErrorCategory::Unsupported, {});
  }

  // --- Do not wake a sleeping disk; skip the read instead (the library
  // --- explicitly recommends this to avoid spinning a parked drive).
  SkBool awake = TRUE;
  if (sk_disk_check_sleep_mode(disk, &awake) == 0 && !awake) {
    snap.ok = false;
    snap.error = HealthErrorCategory::DeviceUnavailable;
    snap.status = DiskHealthStatus::Unavailable;
    snap.detail = "The disk is sleeping; SMART data was not read to avoid "
                  "waking it up";
    return snap;
  }

  // --- Read the SMART log (read-only; never starts tests or changes settings).
  if (sk_disk_smart_read_data(disk) != 0) {
    return failSnapshot(std::move(snap),
                        categorizeErrno(errno),
                        {});
  }

  SkBool good = FALSE;
  if (sk_disk_smart_status(disk, &good) == 0) {
    snap.smart_self_assessment = good != FALSE;
  }

  SkSmartOverall overall = SK_SMART_OVERALL_GOOD;
  if (sk_disk_smart_get_overall(disk, &overall) == 0) {
    const char *overall_text = sk_smart_overall_to_string(overall);
    if (overall_text != nullptr) {
      snap.smart_overall = overall_text;
    }
  }

  std::uint64_t value = 0;
  if (sk_disk_smart_get_temperature(disk, &value) == 0 && value != 0) {
    snap.temperature_celsius = mKelvinToCelsius(value);
  }
  if (sk_disk_smart_get_power_on(disk, &value) == 0) {
    snap.power_on_hours = value / 3'600'000;  // milliseconds -> hours
  }
  if (sk_disk_smart_get_power_cycle(disk, &value) == 0) {
    snap.power_cycles = value;
  }

  // --- Attribute table.
  AttributeGather gather;
  const int attrs_rc =
      sk_disk_smart_parse_attributes(disk, gatherSmartAttribute, &gather);
  if (attrs_rc == 0) {
    snap.attributes = std::move(gather.attributes);
    snap.attributes_truncated = snap.attributes.size() >= kAttributeCap;
  }

  // --- Known, bounded common values derived from the attribute table. These
  // --- interpretations are conventional and only applied to well-known IDs.
  for (const SmartAttribute &attribute : snap.attributes) {
    if (!attribute.raw_value.has_value()) {
      continue;
    }
    switch (attribute.id) {
      case 5:   // Reallocated Sector Count
        snap.reallocated_sectors = *attribute.raw_value;
        break;
      case 187:  // Reported Uncorrectable Errors
        snap.reported_uncorrectable_errors = *attribute.raw_value;
        break;
      case 192:  // Unsafe Shutdown Count
        snap.unsafe_shutdowns = *attribute.raw_value;
        break;
      case 194:  // Temperature Celsius (many vendors)
        if (!snap.temperature_celsius.has_value() &&
            attribute.raw_value && *attribute.raw_value <= 250) {
          snap.temperature_celsius =
              static_cast<double>(*attribute.raw_value);
        }
        break;
      case 197:  // Current Pending Sector Count
        snap.current_pending_sectors = *attribute.raw_value;
        break;
      case 198:  // Offline Uncorrectable Sector Count
        snap.offline_uncorrectable_sectors = *attribute.raw_value;
        break;
      default:
        break;
    }
  }

  // --- Assessment: failing beats warning, warning beats healthy.
  const bool self_assessment_failed =
      snap.smart_self_assessment.has_value() && !*snap.smart_self_assessment;
  const bool overall_bad =
      snap.smart_overall.find("BAD") != std::string::npos;
  bool any_warning = false;
  for (const SmartAttribute &attribute : snap.attributes) {
    any_warning = any_warning || attribute.warning;
  }

  snap.ok = true;
  snap.error = HealthErrorCategory::None;
  snap.status = DiskHealthStatus::Healthy;
  if (self_assessment_failed || overall_bad) {
    snap.status = DiskHealthStatus::Failing;
  } else if (any_warning) {
    snap.status = DiskHealthStatus::Warning;
  }
  return snap;
}

DiskHealthSnapshot NvmeHealthProvider::readHealth(const DiskHealthSnapshot &base) {
  DiskHealthSnapshot snap = base;
  snap.source = DiskHealthSource::Nvme;

  const std::string controller_path = nvmeControllerPath(snap.device_name);
  if (controller_path.empty()) {
    return failSnapshot(std::move(snap), HealthErrorCategory::Unsupported,
                        "Unrecognized NVMe device name");
  }
  snap.device_path = controller_path;

  const int fd = ::open(controller_path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) {
    return failSnapshot(std::move(snap), categorizeErrno(errno), {});
  }
  const auto close_fd = [&]() { ::close(fd); };

  // Zero-filled so short responses leave parseable (empty) fields.
  std::array<std::uint8_t, kNvmeLogLength> buffer{};
  struct nvme_admin_cmd cmd{};
  cmd.opcode = kNvmeAdminGetLogPage;
  cmd.nsid = kNvmeNsidAll;
  cmd.addr = reinterpret_cast<std::uint64_t>(buffer.data());
  cmd.data_len = static_cast<std::uint32_t>(kNvmeLogLength);
  cmd.cdw10 = (static_cast<std::uint32_t>(kNvmeLogLength / 4 - 1) << 16) |
              kNvmeLogSmart;

  const int ioctl_rc = ::ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);
  close_fd();
  if (ioctl_rc != 0) {
    return failSnapshot(std::move(snap), categorizeErrno(errno), {});
  }

  const NvmeParseResult parsed = parseNvmeHealthLog(buffer.data(), buffer.size());
  if (!parsed.valid) {
    return failSnapshot(std::move(snap), HealthErrorCategory::ParseError,
                        "The NVMe controller returned an invalid SMART log");
  }
  snap.nvme = parsed.log;

  // --- Assessment. Critical warning bits that mean the drive lost reliability
  // --- or went read-only are treated as failing; the rest as warning.
  const std::uint8_t flags =
      parsed.log.critical_warning.value_or(0);
  const bool reliability_degraded = (flags & 0x04U) != 0;
  const bool read_only = (flags & 0x08U) != 0;
  const bool any_critical_warning = flags != 0;

  const bool spare_low =
      parsed.log.available_spare.has_value() &&
      parsed.log.available_spare_threshold.has_value() &&
      *parsed.log.available_spare <= *parsed.log.available_spare_threshold;
  const bool life_exhausted =
      parsed.log.percentage_used.has_value() && *parsed.log.percentage_used >= 100;

  snap.ok = true;
  snap.error = HealthErrorCategory::None;
  snap.status = DiskHealthStatus::Healthy;
  if (reliability_degraded || read_only || life_exhausted) {
    snap.status = DiskHealthStatus::Failing;
  } else if (any_critical_warning || spare_low) {
    snap.status = DiskHealthStatus::Warning;
  }
  return snap;
}

std::unique_ptr<DiskHealthProvider> defaultDiskHealthProviderFactory(
    const std::string &name) {
  if (diskHealthSourceForDevice(name) == DiskHealthSource::Nvme) {
    return std::make_unique<NvmeHealthProvider>();
  }
  return std::make_unique<AtaSmartProvider>();
}

// --- Monitor --------------------------------------------------------------

namespace {

/// The default identity reader reads live sysfs.
std::optional<DeviceIdentity> liveIdentityReader(const std::string &name) {
  return readDeviceIdentity(name);
}

}  // namespace

DiskHealthMonitor::DiskHealthMonitor(ProviderFactory provider_factory,
                                     IdentityReader identity_reader,
                                     IdentityCheck identity_check)
    : provider_factory_(std::move(provider_factory)),
      identity_reader_(std::move(identity_reader)),
      identity_check_(std::move(identity_check)) {
  if (!provider_factory_) {
    provider_factory_ = defaultDiskHealthProviderFactory;
  }
  if (!identity_reader_) {
    identity_reader_ = liveIdentityReader;
  }
  if (!identity_check_) {
    identity_check_ = [](const std::string &name,
                         const DeviceIdentity &expected) {
      return deviceIdentityUnchanged(name, expected);
    };
  }
  worker_ = std::thread(&DiskHealthMonitor::workerLoop, this);
}

DiskHealthMonitor::~DiskHealthMonitor() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

void DiskHealthMonitor::setDevices(std::vector<std::string> devices) {
  std::sort(devices.begin(), devices.end());
  devices.erase(std::unique(devices.begin(), devices.end()), devices.end());
  std::lock_guard<std::mutex> lock(mutex_);
  devices_ = std::move(devices);
  // Prune caches for devices that no longer exist so replaced disks never show
  // stale health data.
  std::vector<std::string> stale;
  stale.reserve(results_.size());
  for (const auto &pair : results_) {
    const std::string &name = pair.first;
    if (std::find(devices_.begin(), devices_.end(), name) == devices_.end()) {
      stale.push_back(name);
    }
  }
  for (const std::string &name : stale) {
    results_.erase(name);
    attempts_.erase(name);
    attempt_category_.erase(name);
  }
  // Ensure a cache entry exists for every current device.
  for (const std::string &name : devices_) {
    if (results_.find(name) == results_.end()) {
      results_.emplace(name, DiskHealthCacheEntry{});
    }
  }
}

bool DiskHealthMonitor::requestRefresh(bool force) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (refreshing_ || refresh_pending_) {
      return false;  // no overlapping health requests
    }
    refresh_pending_ = true;
    forced_ = force;
  }
  cv_.notify_all();
  return true;
}

bool DiskHealthMonitor::refreshing() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return refreshing_ || refresh_pending_;
}

std::vector<std::string> DiskHealthMonitor::deviceNames() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return devices_;
}

std::optional<DiskHealthCacheEntry> DiskHealthMonitor::entryFor(
    const std::string &name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = results_.find(name);
  if (found == results_.end()) {
    return std::nullopt;
  }
  return found->second;
}

std::vector<DiskHealthCacheEntry> DiskHealthMonitor::entries() const {
  std::vector<std::string> names;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    names.reserve(results_.size());
    for (const auto &[name, entry] : results_) {
      (void)entry;
      names.push_back(name);
    }
  }
  std::sort(names.begin(), names.end());
  std::vector<DiskHealthCacheEntry> out;
  out.reserve(names.size());
  for (const std::string &name : names) {
    if (const std::optional<DiskHealthCacheEntry> entry = entryFor(name);
        entry.has_value()) {
      out.push_back(*entry);
    }
  }
  return out;
}

void DiskHealthMonitor::waitForIdle() const {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, [&] { return !refreshing_ && !refresh_pending_; });
}

void DiskHealthMonitor::workerLoop() {
  for (;;) {
    std::vector<std::string> devices;
    bool force = true;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [&] { return stop_ || refresh_pending_; });
      if (stop_) {
        return;
      }
      devices = devices_;
      force = forced_;
      forced_ = true;
      refresh_pending_ = false;
      refreshing_ = true;
    }
    runCycle(devices, force);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      refreshing_ = false;
    }
    cv_.notify_all();
  }
}

DiskHealthSnapshot DiskHealthMonitor::readOne(const std::string &name) {
  const std::optional<DeviceIdentity> identity = identity_reader_(name);
  if (!identity.has_value()) {
    DiskHealthSnapshot snap;
    snap.device_name = name;
    snap = failSnapshot(std::move(snap),
                        HealthErrorCategory::DeviceUnavailable, {});
    return snap;
  }

  DiskHealthSnapshot snap;
  snap.device_name = name;
  snap.device_path = "/dev/" + name;
  snap.major = identity->major;
  snap.minor = identity->minor;
  snap.source = diskHealthSourceForDevice(name);

  const DiskMetadata metadata = readDeviceMetadata(name);
  snap.vendor = metadata.vendor;
  snap.model = metadata.model;
  snap.firmware_revision = metadata.firmware_revision;
  snap.serial_available = metadata.serial_available;

  std::unique_ptr<DiskHealthProvider> provider = provider_factory_(name);
  if (!provider) {
    return failSnapshot(std::move(snap),
                        HealthErrorCategory::Unsupported, {});
  }

  DiskHealthSnapshot result = provider->readHealth(snap);
  result.device_name = name;
  result.major = identity->major;
  result.minor = identity->minor;

  // Revalidate identity: a device may have been replaced or removed while the
  // (potentially slow) health read ran. Discard the result if it changed.
  if (!identity_check_(name, *identity)) {
    return failSnapshot(std::move(result),
                        HealthErrorCategory::DeviceUnavailable, {});
  }
  result.refreshed_at = std::chrono::system_clock::now();
  return result;
}

void DiskHealthMonitor::runCycle(const std::vector<std::string> &devices,
                                 bool force) {
  const auto now = std::chrono::steady_clock::now();
  for (const std::string &name : devices) {
    if (stop_) {
      return;
    }
    if (!force) {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto attempt = attempts_.find(name);
      if (attempt != attempts_.end() &&
          withinRetryBackoff(attempt_category_[name], attempt->second, now)) {
        continue;  // recently reported unsupported/permission-denied - skip
      }
    }
    const DiskHealthSnapshot result = readOne(name);

    std::lock_guard<std::mutex> lock(mutex_);
    const auto now_wall = std::chrono::system_clock::now();
    DiskHealthCacheEntry &entry = results_[name];
    entry.device_name = name;
    entry.provider = diskHealthSourceName(result.source);
    entry.last_attempt_at = now_wall;
    attempts_[name] = std::chrono::steady_clock::now();
    attempt_category_[name] = result.error;

    if (result.ok) {
      entry.state = result;
      entry.last_success = std::make_shared<DiskHealthSnapshot>(result);
      entry.last_success_at = now_wall;
      entry.last_error.clear();
      entry.last_error_category = HealthErrorCategory::None;
    } else {
      // Preserve the last successful result while surfacing the failure.
      entry.last_error = result.detail;
      entry.last_error_category = result.error;
      if (entry.last_success) {
        entry.state = *entry.last_success;
        entry.state.detail = result.detail;  // annotate stale view
      } else {
        entry.state = result;
      }
    }
  }
}

}  // namespace atm