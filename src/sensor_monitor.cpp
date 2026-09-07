#include "sensor_monitor.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace atm {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kHwmonBase = "/sys/class/hwmon";

// Millidegrees outside [-100 °C, 250 °C] are never valid computer-hardware
// temperatures; drivers use such sentinel values (e.g. the (u32)-1 "unknown"
// reading) when a sensor is not actually wired up. Such channels are skipped
// for that refresh instead of displaying nonsense.
constexpr std::int64_t kMinSaneMillidegrees = -100'000;
constexpr std::int64_t kMaxSaneMillidegrees = 250'000;

/// Fans spin far below a million RPM; values of that magnitude are the
/// (u32)-1 "unknown" sentinel and are dropped.
constexpr std::uint64_t kMaxSaneRpm = 1'000'000;

/// Fraction of the nearest operating limit at which a sensor turns "Warm".
constexpr double kWarmFraction = 0.85;

/// Strips leading/trailing ASCII whitespace (including a CR).
std::string trimWhitespace(const std::string &text) {
  std::size_t begin = 0;
  while (begin < text.size() &&
         (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r')) {
    ++begin;
  }
  std::size_t end = text.size();
  while (end > begin &&
         (text[end - 1] == ' ' || text[end - 1] == '\t' ||
          text[end - 1] == '\r')) {
    --end;
  }
  return text.substr(begin, end - begin);
}

/// Lowercases ASCII characters so classification compares case-insensitively.
std::string toLowerAscii(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  return text;
}

bool containsKeyword(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

/// True when `name` exactly matches one of `candidates`.
bool matchesAny(std::string_view name, const std::string_view *candidates,
                std::size_t count) {
  return std::any_of(candidates, candidates + count,
                     [name](std::string_view candidate) {
                       return name == candidate;
                     });
}

/// The "hwmonN" directory names under /sys/class/hwmon, sorted so the cached
/// list has a stable comparison order. Returns an empty vector when the
/// directory is absent or empty.
std::vector<std::string> listHwmonNames() {
  std::vector<std::string> names;
  std::error_code ec;
  fs::directory_iterator it(kHwmonBase, ec);
  const fs::directory_iterator end;
  while (!ec && it != end) {
    const std::string name = it->path().filename().string();
    const bool numeric_suffix =
        name.size() > 5 &&
        std::all_of(name.begin() + 5, name.end(), [](unsigned char c) {
          return std::isdigit(c) != 0;
        });
    if (name.rfind("hwmon", 0) == 0 && numeric_suffix) {
      names.push_back(name);
    }
    it.increment(ec);
  }
  std::sort(names.begin(), names.end());
  return names;
}

/// Reads the single-line `name` file of an hwmon device, falling back to the
/// directory name when the file is missing, empty or unreadable.
std::string readDeviceName(const fs::path &dir, const std::string &fallback) {
  std::ifstream file(dir / "name");
  std::string name;
  if (file && std::getline(file, name)) {
    name = trimWhitespace(name);
    if (!name.empty()) {
      return name;
    }
  }
  return fallback;
}

/// Parses the channel number of a "<kind><N>_input" filename, e.g. "temp12_input"
/// -> 12, "fan1_input" -> 1. Returns nullopt when the name does not match.
std::optional<std::size_t> inputChannelIndex(std::string_view name,
                                             std::string_view kind) {
  constexpr std::string_view kInputSuffix = "_input";
  const std::size_t digits_begin = kind.size();
  const std::size_t digits_end = name.size() - kInputSuffix.size();
  if (name.size() < kind.size() + 1 + kInputSuffix.size() ||
      name.compare(0, digits_begin, kind) != 0 ||
      name.compare(digits_end, kInputSuffix.size(), kInputSuffix) != 0 ||
      digits_begin >= digits_end) {
    return std::nullopt;
  }
  std::uint64_t index = 0;
  const std::string_view digits =
      name.substr(digits_begin, digits_end - digits_begin);
  const char *begin = digits.data();
  const char *end = digits.data() + digits.size();
  const auto [ptr, ec] = std::from_chars(begin, end, index);
  if (ec != std::errc() || ptr != end) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(index);
}

/// Reads a single-line sysfs value and parses it as a signed 64-bit integer.
/// Returns false for empty/unreadable/malformed text.
bool readInt64File(const std::string &path, std::int64_t &out) {
  std::ifstream file(path);
  std::string text;
  if (!file || !std::getline(file, text)) {
    return false;
  }
  text = trimWhitespace(text);
  if (text.empty()) {
    return false;
  }
  const char *begin = text.data();
  const char *end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, out);
  return ec == std::errc() && ptr != begin;
}

/// Reads a single-line sysfs value and parses it as an unsigned 64-bit
/// integer. Returns false for empty/unreadable/malformed text.
bool readUInt64File(const std::string &path, std::uint64_t &out) {
  std::ifstream file(path);
  std::string text;
  if (!file || !std::getline(file, text)) {
    return false;
  }
  text = trimWhitespace(text);
  if (text.empty()) {
    return false;
  }
  const char *begin = text.data();
  const char *end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, out);
  return ec == std::errc() && ptr != begin;
}

/// Reads a temperature in millidegrees Celsius and converts it to °C. Returns
/// nullopt when the file is missing, malformed or reports an implausible
/// value (a sensor stuck at a sentinel must never produce a fake reading).
std::optional<double> readTempCelsius(const std::string &path) {
  std::int64_t millidegrees = 0;
  if (!readInt64File(path, millidegrees) ||
      millidegrees < kMinSaneMillidegrees ||
      millidegrees > kMaxSaneMillidegrees) {
    return std::nullopt;
  }
  return static_cast<double>(millidegrees) / 1000.0;
}

/// Reads a fan speed in RPM. Returns nullopt for missing/empty/malformed
/// files and for the "unknown" sentinel magnitudes.
std::optional<std::uint64_t> readRpm(const std::string &path) {
  std::uint64_t rpm = 0;
  if (!readUInt64File(path, rpm) || rpm > kMaxSaneRpm) {
    return std::nullopt;
  }
  return rpm;
}

/// True when the hwmon device name identifies a CPU temperature source
/// (coretemp, k10temp/k8temp/k7temp/k6temp, x86_pkg_temp, zenpower...).
bool isCpuDevice(std::string_view device) {
  static constexpr std::string_view kCandidates[] = {
      "coretemp",  "k10temp",  "k8temp",  "k7temp",
      "k6temp",    "x86_pkg_temp", "zenpower", "cpu_thermal"};
  return matchesAny(device, kCandidates, std::size(kCandidates));
}

/// True when the hwmon device name identifies a GPU temperature source
/// (the amdgpu, radeon, nouveau, nvidia and i915 drivers).
bool isGpuDevice(std::string_view device) {
  static constexpr std::string_view kCandidates[] = {
      "amdgpu", "nouveau", "radeon", "nvidia", "i915"};
  return matchesAny(device, kCandidates, std::size(kCandidates));
}

/// True when the hwmon device name identifies a storage temperature source
/// (NVMe controllers, the SATA drivetemp driver).
bool isStorageDevice(std::string_view device) {
  static constexpr std::string_view kCandidates[] = {"nvme", "drivetemp"};
  if (matchesAny(device, kCandidates, std::size(kCandidates))) {
    return true;
  }
  // ataN temperature sensors use the "ata" prefix.
  return device.rfind("ata", 0) == 0;
}

/// True when the hwmon device name identifies a motherboard/PCH/super-I/O
/// source (acpitz and the PCH thermal drivers, the Super-I/O chip drivers).
bool isBoardDevice(std::string_view device) {
  static constexpr std::string_view kCandidates[] = {
      "acpitz",  "thinkpad",  "it87",      "it8620",    "nct6775",
      "nct6791", "nct6792",   "nct6793",   "nct6795",   "nct6796",
      "nct6797", "nct6798",   "w83627hf",  "w83667hg",  "f71808e",
      "f71868a", "asustek",   "asusec",    "eeepc",     "maxim_1668",
      "wmi_bmof"};
  if (matchesAny(device, kCandidates, std::size(kCandidates))) {
    return true;
  }
  return device.rfind("pch_", 0) == 0;  // pch_skylake, pch_cannonlake, ...
}

/// Classifies a temperature channel from the hwmon device name *and* its
/// label so a sensor is never typed by its label (or device) alone. Device
/// names are authoritative where one matches; labels refine the rest.
SensorType classifySensor(const std::string &device, const std::string &label) {
  const std::string d = toLowerAscii(device);
  const std::string l = toLowerAscii(label);

  if (isCpuDevice(d) || containsKeyword(l, "core") ||
      containsKeyword(l, "package") || containsKeyword(l, "cpu") ||
      containsKeyword(l, "tctl") || containsKeyword(l, "tdie") ||
      containsKeyword(l, "tccd")) {
    return SensorType::CPU;
  }
  if (isGpuDevice(d) || containsKeyword(l, "gpu") ||
      containsKeyword(l, "junction") || containsKeyword(l, "hbm") ||
      containsKeyword(l, "mem")) {
    return SensorType::GPU;
  }
  if (isStorageDevice(d) || containsKeyword(l, "composite") ||
      containsKeyword(l, "drive") || containsKeyword(l, "controller")) {
    return SensorType::Storage;
  }
  if (isBoardDevice(d) || containsKeyword(l, "system") ||
      containsKeyword(l, "motherboard") || containsKeyword(l, "chipset") ||
      containsKeyword(l, "pch") || containsKeyword(l, "ambient") ||
      containsKeyword(l, "south bridge") || containsKeyword(l, "north bridge")) {
    return SensorType::Motherboard;
  }
  return SensorType::Other;
}

/// Derives a simple status from the current temperature and the limits the
/// driver exposes. No limit -> Unknown; otherwise Warm starts at
/// kWarmFraction of the operating limit, High at the maximum, Critical at the
/// critical value.
SensorStatus classifyStatus(double celsius,
                            const std::optional<double> &max,
                            const std::optional<double> &crit) {
  if (!max.has_value() && !crit.has_value()) {
    return SensorStatus::Unknown;
  }
  if (crit.has_value() && celsius >= *crit) {
    return SensorStatus::Critical;
  }
  if (max.has_value() && celsius >= *max) {
    return SensorStatus::High;
  }
  const double limit = crit.has_value() ? *crit : *max;
  if (celsius >= kWarmFraction * limit) {
    return SensorStatus::Warm;
  }
  return SensorStatus::Normal;
}

/// Full path of a <kind><N>_<suffix> file inside an hwmon directory, or an
/// empty string when that file does not exist (channels do not all expose
/// every attribute).
std::string probeSiblingFile(const fs::path &dir, std::string_view kind,
                             std::size_t index, std::string_view suffix) {
  std::error_code ec;
  const fs::path path =
      dir / (std::string(kind) + std::to_string(index) + std::string(suffix));
  const bool exists = fs::is_regular_file(path, ec);
  if (ec || !exists) {
    return {};  // attribute not exposed — treat as absent, never crash
  }
  return path.string();
}

/// Reads a channel's label file, falling back to "Temperature N" / "Fan N".
std::string readChannelLabel(const fs::path &dir, std::string_view kind,
                             std::size_t index, std::string_view fallback_kind) {
  std::ifstream file(
      dir / (std::string(kind) + std::to_string(index) + "_label"));
  std::string label;
  if (file && std::getline(file, label)) {
    label = trimWhitespace(label);
    if (!label.empty()) {
      return label;
    }
  }
  std::ostringstream fallback;
  fallback << fallback_kind << ' ' << index;
  return fallback.str();
}

/// Sorts channels for stable output: by device name, then channel index.
bool channelOrderByDeviceIndex(const std::string &a_device,
                               std::size_t a_index,
                               const std::string &b_device,
                               std::size_t b_index) {
  if (a_device != b_device) {
    return a_device < b_device;
  }
  return a_index < b_index;
}

}  // namespace

const char *sensorTypeName(SensorType type) {
  switch (type) {
    case SensorType::CPU:
      return "CPU";
    case SensorType::GPU:
      return "GPU";
    case SensorType::Storage:
      return "Storage";
    case SensorType::Motherboard:
      return "Motherboard";
    case SensorType::Other:
      return "Other";
  }
  return "Other";
}

const char *sensorStatusName(SensorStatus status) {
  switch (status) {
    case SensorStatus::Normal:
      return "NORMAL";
    case SensorStatus::Warm:
      return "WARM";
    case SensorStatus::High:
      return "HIGH";
    case SensorStatus::Critical:
      return "CRITICAL";
    case SensorStatus::Unknown:
      return "N/A";
  }
  return "N/A";
}

void SensorMonitor::discover() {
  temp_channels_.clear();
  fan_channels_.clear();
  discovered_hwmon_ = listHwmonNames();

  std::error_code ec;
  hwmon_present_ = fs::is_directory(kHwmonBase, ec);

  for (const std::string &dir_name : discovered_hwmon_) {
    const fs::path dir = fs::path(kHwmonBase) / dir_name;
    const std::string device = readDeviceName(dir, dir_name);

    // Collect the channels present in this directory first (the directory
    // iteration order is unspecified), so each channel is then handled once.
    std::vector<std::size_t> temp_indices;
    std::vector<std::size_t> fan_indices;
    std::error_code it_ec;
    fs::directory_iterator it(dir, it_ec);
    const fs::directory_iterator end;
    while (!it_ec && it != end) {
      const std::string filename = it->path().filename().string();
      if (const auto index = inputChannelIndex(filename, "temp")) {
        temp_indices.push_back(*index);
      } else if (const auto index = inputChannelIndex(filename, "fan")) {
        fan_indices.push_back(*index);
      }
      it.increment(it_ec);
    }
    auto sortAndUnique = [](std::vector<std::size_t> &indices) {
      std::sort(indices.begin(), indices.end());
      indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    };
    sortAndUnique(temp_indices);
    sortAndUnique(fan_indices);

    for (const std::size_t index : temp_indices) {
      TemperatureChannel channel;
      channel.device = device;
      channel.index = index;
      channel.label = readChannelLabel(dir, "temp", index, "Temperature");
      channel.type = classifySensor(device, channel.label);
      channel.input_path =
          (dir / ("temp" + std::to_string(index) + "_input")).string();
      channel.max_path = probeSiblingFile(dir, "temp", index, "_max");
      channel.crit_path = probeSiblingFile(dir, "temp", index, "_crit");
      temp_channels_.push_back(std::move(channel));
    }

    for (const std::size_t index : fan_indices) {
      FanChannel channel;
      channel.device = device;
      channel.index = index;
      channel.label = readChannelLabel(dir, "fan", index, "Fan");
      channel.input_path =
          (dir / ("fan" + std::to_string(index) + "_input")).string();
      fan_channels_.push_back(std::move(channel));
    }
  }

  std::sort(temp_channels_.begin(), temp_channels_.end(),
            [](const TemperatureChannel &a, const TemperatureChannel &b) {
              return channelOrderByDeviceIndex(a.device, a.index, b.device,
                                               b.index);
            });
  std::sort(fan_channels_.begin(), fan_channels_.end(),
            [](const FanChannel &a, const FanChannel &b) {
              return channelOrderByDeviceIndex(a.device, a.index, b.device,
                                               b.index);
            });
}

SensorSnapshot SensorMonitor::read() {
  // Cheap directory listing each tick; discovery itself (which reads a few
  // files per channel) only runs when the set of hwmon devices changes, so an
  // unplugged/re-plugged sensor is picked up without re-scanning every second.
  if (listHwmonNames() != discovered_hwmon_) {
    discover();
  }

  SensorSnapshot snapshot;
  snapshot.available = hwmon_present_;
  snapshot.temperatures.reserve(temp_channels_.size());
  for (const TemperatureChannel &channel : temp_channels_) {
    const std::optional<double> celsius = readTempCelsius(channel.input_path);
    if (!celsius.has_value()) {
      continue;  // missing/malformed/implausible this refresh — skip quietly
    }
    TemperatureSensor sensor;
    sensor.device = channel.device;
    sensor.label = channel.label;
    sensor.type = channel.type;
    sensor.temperature_celsius = *celsius;
    sensor.max_temperature_celsius =
        channel.max_path.empty() ? std::nullopt
                                 : readTempCelsius(channel.max_path);
    sensor.critical_temperature_celsius =
        channel.crit_path.empty() ? std::nullopt
                                  : readTempCelsius(channel.crit_path);
    sensor.status = classifyStatus(sensor.temperature_celsius,
                                   sensor.max_temperature_celsius,
                                   sensor.critical_temperature_celsius);
    snapshot.temperatures.push_back(std::move(sensor));
  }

  snapshot.fans.reserve(fan_channels_.size());
  for (const FanChannel &channel : fan_channels_) {
    const std::optional<std::uint64_t> rpm = readRpm(channel.input_path);
    if (!rpm.has_value()) {
      continue;
    }
    FanSensor fan;
    fan.device = channel.device;
    fan.label = channel.label;
    fan.rpm = *rpm;
    snapshot.fans.push_back(std::move(fan));
  }

  return snapshot;
}

}  // namespace atm