#include <array>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <optional>
#include <poll.h>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include "cpu_monitor.hpp"
#include "disk_monitor.hpp"
#include "format_bytes.hpp"
#include "gpu_monitor.hpp"
#include "memory_monitor.hpp"
#include "network_monitor.hpp"
#include "process_actions.hpp"
#include "process_monitor.hpp"
#include "process_tree.hpp"
#include "sensor_monitor.hpp"
#include "startup_manager.hpp"
#include "systemd_manager.hpp"

namespace {

using namespace std::chrono_literals;

/// Which full-screen view the live loop renders each second.
enum class ViewMode {
  List,  // the flat process table (default)
  Tree,  // the parent/child process tree
};

constexpr std::chrono::seconds kRefreshInterval{1};
constexpr int kPercentPrecision = 1;
constexpr int kPercentWidth = 5;
constexpr int kLabelWidth = 21;
constexpr double kBytesPerKilobyte = 1024.0;
constexpr std::size_t kNameColumnWidth = 18;
constexpr std::size_t kMaxNameWidth = 16;
constexpr std::size_t kStorageMountWidth = 28;
constexpr std::size_t kDeviceNameWidth = 14;
constexpr std::size_t kNetworkInterfaceWidth = 14;
constexpr std::size_t kNetworkRateWidth = 12;
constexpr std::size_t kNetworkBytesWidth = 10;
constexpr std::size_t kNetworkStateWidth = 18;
constexpr std::size_t kGpuNameWidth = 20;
constexpr std::size_t kGpuUsageWidth = 7;
constexpr std::size_t kGpuVramWidth = 18;
constexpr std::size_t kGpuClockWidth = 11;
constexpr std::size_t kSensorLabelWidth = 18;
constexpr std::size_t kSensorValueWidth = 9;
constexpr std::size_t kSensorDetailRuleWidth = 32;
constexpr std::size_t kServiceNameWidth = 28;
constexpr std::size_t kServiceStatusWidth = 12;
constexpr std::size_t kServiceEnabledWidth = 12;
constexpr std::size_t kServiceDetailRuleWidth = 32;
constexpr std::size_t kStartupNameWidth = 26;
constexpr std::size_t kStartupEnabledWidth = 10;
constexpr std::size_t kStartupScopeWidth = 8;
constexpr std::size_t kStartupDetailRuleWidth = 36;
constexpr int kMinNice = -20;
constexpr int kMaxNice = 19;

/// Formats a utilization value as " 34.7" (fixed width so the updating view
/// does not shimmer as the value changes).
std::string formatPercent(double percent) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(kPercentPrecision)
      << std::setw(kPercentWidth) << percent;
  return out.str();
}

/// Truncates text to `width` characters so columns stay aligned.
std::string fitTo(const std::string &text, std::size_t width) {
  if (text.size() <= width) {
    return text;
  }
  return text.substr(0, width);
}

/// Uppercases ASCII characters (the kernel reports operstate in lowercase).
std::string toUpperAscii(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::toupper(c));
                 });
  return text;
}

/// Lowercases ASCII characters (for case-insensitive service search).
std::string toLowerAscii(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  return text;
}

/// Formats an integer with thousands separators, e.g. 1245223 -> "1,245,223".
std::string formatThousands(std::uint64_t value) {
  const std::string digits = std::to_string(value);
  std::string out;
  out.reserve(digits.size() + digits.size() / 3);
  const int n = static_cast<int>(digits.size());
  for (int i = 0; i < n; ++i) {
    if (i > 0 && (n - i) % 3 == 0) {
      out.push_back(',');
    }
    out.push_back(digits[i]);
  }
  return out;
}

/// Formats a value given in kibibytes as a human-readable size with the
/// largest whole prefix, e.g. 15872000 -> "15.1 GB", 524288 -> "512 MB".
std::string formatKibibytes(std::uint64_t kibibytes) {
  static constexpr std::array kSuffixes = {"kB", "MB", "GB", "TB"};
  double value = static_cast<double>(kibibytes);
  std::size_t suffix = 0;
  while (value >= kBytesPerKilobyte && suffix + 1 < kSuffixes.size()) {
    value /= kBytesPerKilobyte;
    ++suffix;
  }
  std::ostringstream out;
  out << std::fixed << std::setprecision(value >= 100.0 ? 0 : 1) << value
      << ' ' << kSuffixes[suffix];
  return out.str();
}

/// Truncates a name to kMaxNameWidth characters so columns stay aligned.
std::string fitName(const std::string &name) {
  if (name.size() <= kMaxNameWidth) {
    return name;
  }
  return name.substr(0, kMaxNameWidth);
}

/// Appends "<label>" left-aligned in a fixed-width column followed by
/// "<value>", keeping every label/value column aligned across the output.
void appendLabeled(std::ostringstream &out, const std::string &label,
                   const std::string &value) {
  out << std::left << std::setw(kLabelWidth) << label << value << '\n';
}

/// One table row, shared by the live view and the process-selection screen.
std::string formatProcessRow(const atm::Process &process) {
  std::ostringstream out;
  out << std::right << std::setw(7) << process.pid << "  " << std::left
      << std::setw(kNameColumnWidth) << fitName(process.name) << "  "
      << std::right << std::setw(8) << (formatPercent(process.cpu_percent) + "%")
      << "  " << std::setw(9) << formatKibibytes(process.memory_kib) << "  "
      << std::left << std::setw(10) << atm::processStateName(process.state);
  return out.str();
}

/// Renders the banner and the system-wide CPU + memory summary lines.
void renderHeader(std::ostringstream &out, double cpu_usage,
                  const atm::MemoryInfo &memory) {
  out << "========================================\n"
         "ARCH TASK MANAGER\n"
         "=================\n\n";
  appendLabeled(out, "CPU Usage:", formatPercent(cpu_usage) + "%");
  appendLabeled(out, "Memory Usage:",
                formatPercent(memory.usagePercent()) + "%");
}

/// Renders the MEMORY and SWAP detail sections (Step 2).
void renderMemorySections(std::ostringstream &out,
                          const atm::MemoryInfo &memory) {
  out << "\n## MEMORY\n\n";
  appendLabeled(out, "Total:", formatKibibytes(memory.total));
  appendLabeled(out, "Used:", formatKibibytes(memory.used()));
  appendLabeled(out, "Available:", formatKibibytes(memory.available));
  appendLabeled(out, "Free:", formatKibibytes(memory.free));
  appendLabeled(out, "Cached:", formatKibibytes(memory.cached));
  appendLabeled(out, "Buffers:", formatKibibytes(memory.buffers));
  appendLabeled(out, "Usage:", formatPercent(memory.usagePercent()) + "%");

  out << "\n## SWAP\n\n";
  appendLabeled(out, "Total:", formatKibibytes(memory.swap_total));
  appendLabeled(out, "Used:", formatKibibytes(memory.swapUsed()));
  appendLabeled(out, "Free:", formatKibibytes(memory.swap_free));
  appendLabeled(out, "Usage:", formatPercent(memory.swapUsagePercent()) + "%");
}

/// Renders the STORAGE (physical filesystem capacities), DISK ACTIVITY
/// (aggregate read/write rates) and DEVICES (whole physical disks) sections.
/// Only physical mounts are shown; virtual/temporary/network mounts are
/// counted on one summary line so /proc, /sys, tmpfs etc. are never mistaken
/// for disk capacity (Step 6).
void renderStorageSections(std::ostringstream &out,
                           const atm::DiskSnapshot &disk) {
  out << "\n## STORAGE\n\n"
      << "FILESYSTEMS\n\n"
      << std::left << std::setw(kStorageMountWidth) << "## Mount Point"
      << std::right << std::setw(10) << "Total" << std::setw(10) << "Used"
      << std::setw(10) << "Free" << "   Type\n";
  if (disk.filesystems.empty()) {
    out << "No filesystems available.\n";
  } else {
    for (const atm::DiskUsage &fs : disk.filesystems) {
      const std::string mount =
          fs.mount_point.size() <= kStorageMountWidth
              ? fs.mount_point
              : fs.mount_point.substr(0, kStorageMountWidth);
      out << std::left << std::setw(kStorageMountWidth) << mount << std::right
          << std::setw(10) << atm::formatBytes(fs.total_bytes) << std::setw(10)
          << atm::formatBytes(fs.used_bytes) << std::setw(10)
          << atm::formatBytes(fs.available_bytes) << "   "
          << atm::diskFsTypeName(fs.type) << '\n';
    }
  }
  out << "Excluded: " << disk.excluded_mounts
      << " virtual/temporary/network mount(s)\n";

  out << "\n---\n\n## DISK ACTIVITY\n\n";
  appendLabeled(out, "Read:",
                atm::formatBytes(disk.total_read_bytes_per_second) + "/s");
  appendLabeled(out, "Write:",
                atm::formatBytes(disk.total_write_bytes_per_second) + "/s");

  out << "\n## DEVICES\n\n"
      << std::left << std::setw(kDeviceNameWidth) << "## DEVICE" << std::right
      << std::setw(8) << "SIZE" << std::setw(12) << "READ" << std::setw(12)
      << "WRITE\n";
  if (disk.devices.empty()) {
    out << "No block devices found.\n";
  } else {
    for (const atm::BlockDevice &device : disk.devices) {
      const std::string name =
          device.name.size() <= kDeviceNameWidth
              ? device.name
              : device.name.substr(0, kDeviceNameWidth);
      out << std::left << std::setw(kDeviceNameWidth) << name << std::right
          << std::setw(8) << atm::formatBytes(device.size_bytes) << std::setw(12)
          << (atm::formatBytes(device.activity.read_bytes_per_second) + "/s")
          << std::setw(12)
          << (atm::formatBytes(device.activity.write_bytes_per_second) + "/s")
          << '\n';
    }
  }
}

/// Renders the NETWORK summary table (interface, RX/TX speed, cumulative
/// RX/TX bytes, state). Non-loopback interfaces come first by name;
/// loopback (lo) is always last and marked so it is never read as the real
/// connection.
std::string renderNetworkTableText(const atm::NetworkSnapshot &network) {
  std::ostringstream out;
  out << std::left << std::setw(kNetworkInterfaceWidth) << "Interface"
      << std::right << std::setw(kNetworkRateWidth) << "RX Speed"
      << std::setw(kNetworkRateWidth) << "TX Speed"
      << std::setw(kNetworkBytesWidth) << "RX"
      << std::setw(kNetworkBytesWidth) << "TX"
      << "  State\n";
  if (network.interfaces.empty()) {
    out << "No network interfaces found.\n";
    return out.str();
  }
  for (const atm::NetworkInterfaceStats &iface : network.interfaces) {
    out << std::left << std::setw(kNetworkInterfaceWidth)
        << fitTo(iface.name, kNetworkInterfaceWidth) << std::right
        << std::setw(kNetworkRateWidth)
        << atm::formatNetworkRate(iface.rx_bytes_per_second)
        << std::setw(kNetworkRateWidth)
        << atm::formatNetworkRate(iface.tx_bytes_per_second)
        << std::setw(kNetworkBytesWidth) << atm::formatBytes(iface.rx_bytes)
        << std::setw(kNetworkBytesWidth) << atm::formatBytes(iface.tx_bytes)
        << "  " << std::left << std::setw(kNetworkStateWidth)
        << (toUpperAscii(iface.state) +
            (iface.loopback ? " (loopback)" : ""))
        << '\n';
  }
  return out.str();
}

/// Renders the NETWORK section: the summary table plus the aggregate RX/TX
/// rates. Totals cover non-loopback interfaces only.
void renderNetworkSections(std::ostringstream &out,
                           const atm::NetworkSnapshot &network) {
  out << "\n## NETWORK\n\n" << renderNetworkTableText(network);
  out << "\nTotal RX: " << atm::formatNetworkRate(network.total_rx_bytes_per_second)
      << '\n'
      << "Total TX: " << atm::formatNetworkRate(network.total_tx_bytes_per_second)
      << '\n'
      << "Loopback traffic is excluded from the totals."
      << "\nDetailed stats: press 'i' (then Enter)\n";
}

/// Formats a utilization value with a trailing '%' ("72.0%"), or "N/A" when
/// the driver does not expose it.
std::string formatGpuUsage(const std::optional<double> &usage_percent) {
  if (!usage_percent.has_value()) {
    return "N/A";
  }
  std::ostringstream out;
  out << std::fixed << std::setprecision(1) << *usage_percent << '%';
  return out.str();
}

/// One VRAM table cell: "3.2 GB/8.0 GB", or "N/A" when the driver does not
/// expose VRAM usage (e.g. Intel iGPUs that share system memory).
std::string formatGpuVramSummary(const atm::GpuStats &gpu) {
  if (!gpu.memory_used_bytes.has_value() || !gpu.memory_total_bytes.has_value()) {
    return "N/A";
  }
  return atm::formatBytes(*gpu.memory_used_bytes) + "/" +
         atm::formatBytes(*gpu.memory_total_bytes);
}

/// Renders the GPU summary table (name, usage, VRAM, clock). Missing metrics
/// degrade to "N/A" per cell; a GPU is never hidden because one metric is
/// unavailable.
std::string renderGpuTableText(const atm::GpuSnapshot &gpu) {
  std::ostringstream out;
  out << std::left << std::setw(kGpuNameWidth) << "GPU" << std::right
      << std::setw(kGpuUsageWidth) << "Usage" << std::setw(kGpuVramWidth)
      << "VRAM" << std::setw(kGpuClockWidth) << "Clock\n";
  if (gpu.devices.empty()) {
    out << "No GPU detected.\n";
    return out.str();
  }
  for (const atm::GpuStats &gpu_stats : gpu.devices) {
    out << std::left << std::setw(kGpuNameWidth)
        << fitTo(gpu_stats.name, kGpuNameWidth) << std::right
        << std::setw(kGpuUsageWidth) << formatGpuUsage(gpu_stats.utilization_percent)
        << std::setw(kGpuVramWidth) << formatGpuVramSummary(gpu_stats)
        << std::setw(kGpuClockWidth)
        << atm::formatGpuFrequency(gpu_stats.frequency_hz) << '\n';
  }
  return out.str();
}

/// Renders the GPU section of the live view.
void renderGpuSections(std::ostringstream &out, const atm::GpuSnapshot &gpu) {
  out << "\n## GPU\n\n" << renderGpuTableText(gpu)
      << "Detailed GPU info: press 'g' (then Enter)\n";
}

/// Formats a temperature with one decimal place, e.g. "51.4 °C".
std::string formatCelsius(double celsius) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(1) << celsius << " °C";
  return out.str();
}

/// Best display name for a GPU temperature sensor: resolves the hwmon device
/// name (e.g. "amdgpu") against the GPU monitor's detected devices so the GPU
/// model is shown instead of the generic driver name. Falls back to the
/// driver name when no GPU is known to be bound to it.
std::string gpuSensorDisplayName(const std::string &hwmon_device,
                                 const atm::GpuSnapshot &gpu) {
  for (const atm::GpuStats &stats : gpu.devices) {
    if (stats.driver == hwmon_device) {
      return stats.name;
    }
  }
  return hwmon_device;
}

/// Renders the SENSORS section of the live view: temperatures grouped by
/// category (CPU / GPU / Storage / Motherboard+Other) followed by fan speeds
/// when any are exposed. A machine with no temperature/fan channels at all
/// gets a single "no sensors" line — never a crash and never fake values.
void renderSensorSections(std::ostringstream &out,
                          const atm::SensorSnapshot &sensors,
                          const atm::GpuSnapshot &gpu) {
  out << "\n## SENSORS\n\n";
  if (sensors.temperatures.empty() && sensors.fans.empty()) {
    out << "No hardware temperature sensors available.\n"
        << "Detailed sensor info: press 's' (then Enter)\n";
    return;
  }

  // The display label of one sensor: the GPU model for GPU sensors, the
  // sensor label (or its "Temperature N" fallback) otherwise.
  const auto displayLabel = [&](const atm::TemperatureSensor &sensor,
                                bool gpu_group) {
    return gpu_group ? gpuSensorDisplayName(sensor.device, gpu)
                     : sensor.label;
  };

  // One temperature group: title + one aligned row per matching sensor. When
  // two sensors in the group share the same label (e.g. label-less acpitz and
  // PCH devices both fall back to "Temperature 1"), the owning device name is
  // shown instead so rows stay unambiguous.
  const auto renderTemperatureGroup = [&](const char *title,
                                          atm::SensorType type,
                                          bool gpu_group) {
    std::vector<const atm::TemperatureSensor *> rows;
    for (const atm::TemperatureSensor &sensor : sensors.temperatures) {
      if (sensor.type == type) {
        rows.push_back(&sensor);
      }
    }
    if (rows.empty()) {
      return;
    }
    std::unordered_map<std::string, std::size_t> label_count;
    for (const atm::TemperatureSensor *sensor : rows) {
      ++label_count[displayLabel(*sensor, gpu_group)];
    }
    out << title << "\n";
    for (const atm::TemperatureSensor *sensor : rows) {
      const std::string base = displayLabel(*sensor, gpu_group);
      const std::string label =
          !gpu_group && label_count[base] > 1 ? sensor->device : base;
      out << std::left << std::setw(kSensorLabelWidth)
          << fitTo(label, kSensorLabelWidth) << std::right
          << std::setw(kSensorValueWidth)
          << formatCelsius(sensor->temperature_celsius) << '\n';
    }
    out << '\n';
  };

  renderTemperatureGroup("CPU", atm::SensorType::CPU, false);
  renderTemperatureGroup("GPU", atm::SensorType::GPU, true);
  renderTemperatureGroup("Storage", atm::SensorType::Storage, false);

  // Motherboard and unclassified sensors share the last temperature group,
  // titled by what is actually present.
  std::vector<const atm::TemperatureSensor *> board_rows;
  bool any_motherboard = false;
  for (const atm::TemperatureSensor &sensor : sensors.temperatures) {
    if (sensor.type == atm::SensorType::Motherboard) {
      board_rows.push_back(&sensor);
      any_motherboard = true;
    } else if (sensor.type == atm::SensorType::Other) {
      board_rows.push_back(&sensor);
    }
  }
  if (!board_rows.empty()) {
    std::unordered_map<std::string, std::size_t> label_count;
    for (const atm::TemperatureSensor *sensor : board_rows) {
      ++label_count[sensor->label];
    }
    out << (any_motherboard ? "Motherboard" : "Other") << "\n";
    for (const atm::TemperatureSensor *sensor : board_rows) {
      const std::string &base = sensor->label;
      const std::string label =
          label_count[base] > 1 ? sensor->device : base;
      out << std::left << std::setw(kSensorLabelWidth)
          << fitTo(label, kSensorLabelWidth) << std::right
          << std::setw(kSensorValueWidth)
          << formatCelsius(sensor->temperature_celsius) << '\n';
    }
    out << '\n';
  }

  if (!sensors.fans.empty()) {
    std::unordered_map<std::string, std::size_t> label_count;
    for (const atm::FanSensor &fan : sensors.fans) {
      ++label_count[fan.label];
    }
    out << "FANS\n";
    for (const atm::FanSensor &fan : sensors.fans) {
      const std::string label =
          label_count[fan.label] > 1 ? fan.device : fan.label;
      out << std::left << std::setw(kSensorLabelWidth)
          << fitTo(label, kSensorLabelWidth) << std::right
          << std::setw(kSensorValueWidth)
          << (std::to_string(fan.rpm) + " RPM") << '\n';
    }
    out << '\n';
  }

  out << "Detailed sensor info: press 's' (then Enter)\n";
}

/// Full per-sensor breakdown used by the sensor-detail screen: every
/// temperature with its limits and derived status, then every fan. Sensors
/// are grouped under a header per hwmon device so multi-device machines stay
/// readable.
std::string renderSensorDetails(const atm::SensorSnapshot &sensors,
                                const atm::GpuSnapshot &gpu) {
  std::ostringstream out;
  if (sensors.temperatures.empty() && sensors.fans.empty()) {
    out << "No hardware temperature sensors available.\n";
    return out.str();
  }

  std::string current_device;
  for (const atm::TemperatureSensor &sensor : sensors.temperatures) {
    if (sensor.device != current_device) {
      if (!current_device.empty()) {
        out << '\n';
      }
      current_device = sensor.device;
      out << "## " << sensor.device << "\n\n";
    }
    const std::string label =
        sensor.type == atm::SensorType::GPU
            ? gpuSensorDisplayName(sensor.device, gpu)
            : sensor.label;
    out << label << "\n"
        << std::string(kSensorDetailRuleWidth, '-') << "\n";
    appendLabeled(out, "Current:", formatCelsius(sensor.temperature_celsius));
    appendLabeled(out, "Maximum:",
                  sensor.max_temperature_celsius.has_value()
                      ? formatCelsius(*sensor.max_temperature_celsius)
                      : "N/A");
    appendLabeled(out, "Critical:",
                  sensor.critical_temperature_celsius.has_value()
                      ? formatCelsius(*sensor.critical_temperature_celsius)
                      : "N/A");
    appendLabeled(out, "Status:", atm::sensorStatusName(sensor.status));
    out << '\n';
  }

  if (!sensors.fans.empty()) {
    out << "## FANS\n\n";
    for (const atm::FanSensor &fan : sensors.fans) {
      out << fan.label << "\n"
          << std::string(kSensorDetailRuleWidth, '-') << "\n";
      appendLabeled(out, "Speed:", std::to_string(fan.rpm) + " RPM");
      out << '\n';
    }
  }
  return out.str();
}

/// Renders the SYSTEMD SERVICES section of the live view: a table of
/// service names, status, enabled state and description. Failed services
/// are highlighted in their status column.
/// Case-insensitive search match against a service's name and description.
bool serviceMatchesQuery(const atm::SystemdService &svc,
                         const std::string &lower_query) {
  if (lower_query.empty()) {
    return true;
  }
  return toLowerAscii(svc.name).find(lower_query) != std::string::npos ||
         toLowerAscii(svc.description).find(lower_query) != std::string::npos;
}

/// Returns the services sorted by the requested field; the input vector is
/// copied and re-sorted so callers keep ownership of their own snapshot.
std::vector<atm::SystemdService> sortServices(
    const std::vector<atm::SystemdService> &services, atm::ServiceSort sort) {
  std::vector<atm::SystemdService> sorted = services;
  switch (sort) {
    case atm::ServiceSort::Name:
      std::stable_sort(
          sorted.begin(), sorted.end(),
          [](const atm::SystemdService &a, const atm::SystemdService &b) {
            return toLowerAscii(a.name) < toLowerAscii(b.name);
          });
      break;
    case atm::ServiceSort::Status:
      std::stable_sort(
          sorted.begin(), sorted.end(),
          [](const atm::SystemdService &a, const atm::SystemdService &b) {
            return a.state < b.state;
          });
      break;
    case atm::ServiceSort::Enabled:
      std::stable_sort(
          sorted.begin(), sorted.end(),
          [](const atm::SystemdService &a, const atm::SystemdService &b) {
            return a.file_state < b.file_state;
          });
      break;
    case atm::ServiceSort::Description:
      std::stable_sort(
          sorted.begin(), sorted.end(),
          [](const atm::SystemdService &a, const atm::SystemdService &b) {
            return toLowerAscii(a.description) < toLowerAscii(b.description);
          });
      break;
  }
  return sorted;
}

/// Filters a service list by a case-insensitive query on name/description,
/// then sorts it by the requested field.
std::vector<atm::SystemdService> filterAndSortServices(
    const std::vector<atm::SystemdService> &services,
    const std::string &query, atm::ServiceSort sort) {
  const std::string lower_query = toLowerAscii(query);
  std::vector<atm::SystemdService> filtered;
  filtered.reserve(services.size());
  for (const atm::SystemdService &svc : services) {
    if (serviceMatchesQuery(svc, lower_query)) {
      filtered.push_back(svc);
    }
  }
  return sortServices(filtered, sort);
}

/// Renders the SYSTEMD SERVICES table for a list of services.
std::string renderSystemdTableText(
    const std::vector<atm::SystemdService> &services, bool available) {
  std::ostringstream out;
  out << std::left << std::setw(kServiceNameWidth) << "Service"
      << std::right << std::setw(kServiceStatusWidth) << "Status"
      << std::setw(kServiceEnabledWidth) << "Enabled"
      << "  Description\n";
  if (!available) {
    out << "Unable to connect to systemd.\n"
        << "Service management unavailable.\n";
    return out.str();
  }
  if (services.empty()) {
    out << "No services found.\n";
    return out.str();
  }
  for (const atm::SystemdService &svc : services) {
    const std::string status_str = atm::serviceStateName(svc.state);
    const std::string enabled_str = atm::unitFileStateName(svc.file_state);
    out << std::left << std::setw(kServiceNameWidth)
        << fitTo(svc.name, kServiceNameWidth) << std::right
        << std::setw(kServiceStatusWidth)
        << fitTo(status_str, kServiceStatusWidth)
        << std::setw(kServiceEnabledWidth)
        << fitTo(enabled_str, kServiceEnabledWidth)
        << "  " << fitTo(svc.description, 40) << '\n';
  }
  return out.str();
}

/// Renders the SYSTEMD section of the live view.
void renderSystemdSections(std::ostringstream &out,
                           const atm::SystemdSnapshot &systemd,
                           const std::string &service_search,
                           atm::ServiceSort service_sort) {
  const std::vector<atm::SystemdService> services =
      filterAndSortServices(systemd.services, service_search, service_sort);
  out << "\n## SYSTEMD SERVICES\n\n"
      << renderSystemdTableText(services, systemd.available);
  if (systemd.available && !service_search.empty()) {
    out << "Showing " << services.size() << " of " << systemd.services.size()
        << " services (search: \"" << service_search << "\")\n";
  }
  out << "Service detail: press 'u' (then Enter)\n";
}

/// Full per-service breakdown used by the systemd service detail screen.
std::string renderSystemdServiceDetails(const atm::SystemdService &svc) {
  std::ostringstream out;
  out << svc.name << "\n"
      << std::string(kServiceDetailRuleWidth, '-') << "\n";
  appendLabeled(out, "Description:", svc.description);
  appendLabeled(out, "Load State:", svc.load_state);
  appendLabeled(out, "Active State:", svc.active_state);
  appendLabeled(out, "Sub State:", svc.sub_state);
  appendLabeled(out, "Enabled:",
                atm::unitFileStateName(svc.file_state));
  if (svc.main_pid > 0) {
    appendLabeled(out, "Main PID:", std::to_string(svc.main_pid));
  }
  if (!svc.unit_file_path.empty()) {
    appendLabeled(out, "Unit Path:", svc.unit_file_path);
  }
  return out.str();
}

/// Case-insensitive search match against a startup application's id, name,
/// description and command.
bool startupMatchesQuery(const atm::StartupApplication &app,
                         const std::string &lower_query) {
  if (lower_query.empty()) {
    return true;
  }
  return toLowerAscii(app.id).find(lower_query) != std::string::npos ||
         toLowerAscii(app.name).find(lower_query) != std::string::npos ||
         toLowerAscii(app.description).find(lower_query) !=
             std::string::npos ||
         toLowerAscii(app.exec_command).find(lower_query) !=
             std::string::npos;
}

/// Returns the startup applications sorted by the requested field. The input
/// is copied and re-sorted so callers keep ownership of their own snapshot.
std::vector<atm::StartupApplication> sortStartupApps(
    const std::vector<atm::StartupApplication> &apps,
    atm::StartupSort sort) {
  std::vector<atm::StartupApplication> sorted = apps;
  switch (sort) {
    case atm::StartupSort::Name:
      std::stable_sort(
          sorted.begin(), sorted.end(),
          [](const atm::StartupApplication &a,
             const atm::StartupApplication &b) {
            return toLowerAscii(a.name) < toLowerAscii(b.name);
          });
      break;
    case atm::StartupSort::Enabled:
      std::stable_sort(
          sorted.begin(), sorted.end(),
          [](const atm::StartupApplication &a,
             const atm::StartupApplication &b) {
            return a.enabled && !b.enabled;
          });
      break;
    case atm::StartupSort::Scope:
      std::stable_sort(
          sorted.begin(), sorted.end(),
          [](const atm::StartupApplication &a,
             const atm::StartupApplication &b) {
            return a.scope < b.scope;
          });
      break;
  }
  return sorted;
}

/// Filters a startup list by a case-insensitive query, then sorts it.
std::vector<atm::StartupApplication> filterAndSortStartupApps(
    const std::vector<atm::StartupApplication> &apps,
    const std::string &query, atm::StartupSort sort) {
  const std::string lower_query = toLowerAscii(query);
  std::vector<atm::StartupApplication> filtered;
  filtered.reserve(apps.size());
  for (const atm::StartupApplication &app : apps) {
    if (startupMatchesQuery(app, lower_query)) {
      filtered.push_back(app);
    }
  }
  return sortStartupApps(filtered, sort);
}

/// Renders the STARTUP APPLICATIONS table for a list of entries.
std::string renderStartupTableText(
    const std::vector<atm::StartupApplication> &apps, bool available) {
  std::ostringstream out;
  out << std::left << std::setw(kStartupNameWidth) << "Name"
      << std::right << std::setw(kStartupEnabledWidth) << "Enabled"
      << std::setw(kStartupScopeWidth) << "Scope"
      << "  Command\n";
  if (!available) {
    out << "No autostart directories found.\n";
    return out.str();
  }
  if (apps.empty()) {
    out << "No startup applications found.\n";
    return out.str();
  }
  for (const atm::StartupApplication &app : apps) {
    const std::string enabled_str = app.enabled ? "Yes" : "No";
    out << std::left << std::setw(kStartupNameWidth)
        << fitTo(app.name, kStartupNameWidth) << std::right
        << std::setw(kStartupEnabledWidth) << enabled_str
        << std::setw(kStartupScopeWidth)
        << atm::startupScopeName(app.scope)
        << "  " << fitTo(app.exec_command, 44) << '\n';
  }
  return out.str();
}

/// Renders the STARTUP APPLICATIONS section of the live view.
void renderStartupSections(std::ostringstream &out,
                           const atm::StartupSnapshot &startup,
                           const std::string &startup_search,
                           atm::StartupSort startup_sort) {
  const std::vector<atm::StartupApplication> apps =
      filterAndSortStartupApps(startup.apps, startup_search, startup_sort);
  out << "\n## STARTUP APPLICATIONS\n\n"
      << renderStartupTableText(apps, startup.available);
  if (startup.apps.size() > 0 && !startup_search.empty()) {
    out << "Showing " << apps.size() << " of " << startup.apps.size()
        << " applications (search: \"" << startup_search << "\")\n";
  }
  out << "Startup apps: press 'a' (then Enter) to manage autostart\n";
}

/// Full per-application breakdown used by the startup detail screen.
std::string renderStartupDetails(const atm::StartupApplication &app) {
  std::ostringstream out;
  out << app.id << "\n"
      << std::string(kStartupDetailRuleWidth, '-') << "\n";
  appendLabeled(out, "Name:", app.name);
  appendLabeled(out, "Description:", app.description);
  appendLabeled(out, "Command:", app.exec_command);
  appendLabeled(out, "Icon:", app.icon.empty() ? "N/A" : app.icon);
  appendLabeled(out, "Desktop File:", app.desktop_file);
  appendLabeled(out, "Scope:", atm::startupScopeName(app.scope));
  appendLabeled(out, "Enabled:", app.enabled ? "Yes" : "No");
  appendLabeled(out, "Hidden:", app.hidden ? "Yes" : "No");
  appendLabeled(out, "Overrides System:",
                app.overrides_system ? "Yes" : "No");
  if (!app.only_show_in.empty()) {
    appendLabeled(out, "OnlyShowIn:", app.only_show_in);
  }
  if (!app.not_show_in.empty()) {
    appendLabeled(out, "NotShowIn:", app.not_show_in);
  }
  return out.str();
}

/// Full per-GPU breakdown used by the GPU-detail screen. Multiple GPUs are
/// shown one after another with the index and card node on each header.
std::string renderGpuDetails(const atm::GpuSnapshot &gpu) {
  std::ostringstream out;
  if (gpu.devices.empty()) {
    out << "No GPU detected.\n";
    return out.str();
  }
  for (std::size_t index = 0; index < gpu.devices.size(); ++index) {
    const atm::GpuStats &g = gpu.devices[index];
    if (index > 0) {
      out << "\n---\n\n";
    }
    out << "GPU " << index << " (" << g.card << ")\n\n";
    appendLabeled(out, "Name:", g.name);
    appendLabeled(out, "Vendor:", atm::gpuVendorName(g.vendor));
    appendLabeled(out, "Driver:", g.driver.empty() ? "N/A" : g.driver);
    appendLabeled(out, "PCI Slot:", g.pci_slot.empty() ? "N/A" : g.pci_slot);
    out << "\n" << "Usage: " << formatGpuUsage(g.utilization_percent)
        << "\n\nMemory:\n";
    appendLabeled(out, "  Total:",
                  g.memory_total_bytes.has_value()
                      ? atm::formatBytes(*g.memory_total_bytes)
                      : "N/A");
    appendLabeled(out, "  Used:",
                  g.memory_used_bytes.has_value()
                      ? atm::formatBytes(*g.memory_used_bytes)
                      : "N/A");
    appendLabeled(out, "  Free:",
                  g.memory_free_bytes.has_value()
                      ? atm::formatBytes(*g.memory_free_bytes)
                      : "N/A");
    appendLabeled(out, "  Usage:", formatGpuUsage(g.memory_usage_percent));
    out << "\nFrequency: " << atm::formatGpuFrequency(g.frequency_hz) << '\n'
        << "Power: " << atm::formatGpuPower(g.power_watts) << '\n';
  }
  return out.str();
}

/// Full per-interface breakdown used by the network-detail screen.
std::string buildInterfaceDetail(const atm::NetworkInterfaceStats &iface) {
  std::ostringstream out;
  out << "Interface: " << iface.name << "\n\n"
      << "Receive\n";
  appendLabeled(out, "  Speed:", atm::formatNetworkRate(iface.rx_bytes_per_second));
  appendLabeled(out, "  Total:", atm::formatBytes(iface.rx_bytes));
  appendLabeled(out, "  Packets:", formatThousands(iface.rx_packets));
  appendLabeled(out, "  Errors:", std::to_string(iface.rx_errors));
  appendLabeled(out, "  Dropped:", std::to_string(iface.rx_dropped));
  out << "\nTransmit\n";
  appendLabeled(out, "  Speed:", atm::formatNetworkRate(iface.tx_bytes_per_second));
  appendLabeled(out, "  Total:", atm::formatBytes(iface.tx_bytes));
  appendLabeled(out, "  Packets:", formatThousands(iface.tx_packets));
  appendLabeled(out, "  Errors:", std::to_string(iface.tx_errors));
  appendLabeled(out, "  Dropped:", std::to_string(iface.tx_dropped));
  out << "\nState: " << toUpperAscii(iface.state) << "\n";
  if (iface.loopback) {
    out << "Loopback: yes\n";
  }
  return out.str();
}

/// Renders the process table, sorted by `sort`.
void renderProcessTable(std::ostringstream &out,
                        const std::vector<atm::Process> &processes) {
  out << "\n## PROCESSES\n\n"
      << "    PID  NAME                     CPU        RAM  STATE\n";
  for (const atm::Process &process : processes) {
    out << "  " << formatProcessRow(process) << '\n';
  }
}

/// Renders the total/running/sleeping/stopped/zombie counters.
void renderProcessStats(std::ostringstream &out,
                        const atm::ProcessStats &stats) {
  out << "\n## Process Statistics\n\n";
  appendLabeled(out, "Total:", std::to_string(stats.total));
  appendLabeled(out, "Running:", std::to_string(stats.running));
  appendLabeled(out, "Sleeping:", std::to_string(stats.sleeping));
  appendLabeled(out, "Stopped:", std::to_string(stats.stopped));
  appendLabeled(out, "Zombie:", std::to_string(stats.zombie));
}

/// Renders the process-tree view (banner + summary + tree + tree stats +
/// footer). Each frame is a self-contained 1 s snapshot.
std::string renderTreeFrame(double cpu_usage, const atm::MemoryInfo &memory,
                            const atm::ProcessTree &tree) {
  std::ostringstream out;
  renderHeader(out, cpu_usage, memory);
  out << "\n## PROCESS TREE\n\n"
      << atm::renderProcessTree(tree) << "\n\n"
      << "Total processes: " << tree.stats.total << "\n"
      << "Root processes: " << tree.stats.root_count << "\n"
      << "Maximum tree depth: " << tree.stats.max_depth << "\n"
      << "\n---\n\n"
      << "View: [l] Process List  [t] Process Tree (current: Tree)\n"
      << "Tree order: PID ascending\n"
      << "Manage: press 'm' (then Enter) to control a process by PID\n"
      << "Updating every " << kRefreshInterval.count() << " second...\n";
  return out.str();
}

/// Renders the full text view (banner + summary + memory + swap + storage +
/// network + GPU + sensors + processes + statistics + footer). Each frame is a
/// self-contained 1 s snapshot.
std::string renderFrame(double cpu_usage, const atm::MemoryInfo &memory,
                        const std::vector<atm::Process> &processes,
                        const atm::ProcessStats &stats, atm::ProcessSort sort,
                        ViewMode view, const atm::ProcessTree &tree,
                        const atm::DiskSnapshot &disk,
                        const atm::NetworkSnapshot &network,
                        const atm::GpuSnapshot &gpu,
                        const atm::SensorSnapshot &sensors,
                        const atm::SystemdSnapshot &systemd,
                        const atm::StartupSnapshot &startup,
                        const std::string &service_search,
                        atm::ServiceSort service_sort,
                        const std::string &startup_search,
                        atm::StartupSort startup_sort) {
  if (view == ViewMode::Tree) {
    // The tree view stays deliberately focused on the hierarchy; the storage
    // and network sections are part of the table view.
    return renderTreeFrame(cpu_usage, memory, tree);
  }

  std::ostringstream out;
  renderHeader(out, cpu_usage, memory);
  renderMemorySections(out, memory);
  renderStorageSections(out, disk);
  renderNetworkSections(out, network);
  renderGpuSections(out, gpu);
  renderSensorSections(out, sensors, gpu);
  renderSystemdSections(out, systemd, service_search, service_sort);
  renderStartupSections(out, startup, startup_search, startup_sort);
  renderProcessTable(out, processes);
  renderProcessStats(out, stats);
  out << "\n---\n\n"
         "Processes: "
      << processes.size() << "\n"
      << "Sort: [1] CPU  [2] Memory  [3] PID  [4] Name"
         " (current: "
      << atm::processSortName(sort) << ")\n"
      << "View: [l] Process List  [t] Process Tree (current: List)\n"
      << "Manage: press 'm' (then Enter) to control a process by PID\n"
      << "Network detail: press 'i' (then Enter) to inspect an interface\n"
      << "GPU detail: press 'g' (then Enter) to inspect a GPU\n"
      << "Sensor detail: press 's' (then Enter) to inspect a sensor\n"
      << "Systemd services: press 'u' (then Enter) to manage services\n"
      << "Startup apps: press 'a' (then Enter) to manage autostart\n"
      << "Updating every " << kRefreshInterval.count() << " second...\n";
  return out.str();
}

/// Clears the terminal and redraws the whole view in place.
void renderView(double cpu_usage, const atm::MemoryInfo &memory,
                const std::vector<atm::Process> &processes,
                const atm::ProcessStats &stats, atm::ProcessSort sort,
                ViewMode view, const atm::ProcessTree &tree,
                const atm::DiskSnapshot &disk,
                const atm::NetworkSnapshot &network,
                const atm::GpuSnapshot &gpu,
                const atm::SensorSnapshot &sensors,
                const atm::SystemdSnapshot &systemd,
                const atm::StartupSnapshot &startup,
                const std::string &service_search,
                atm::ServiceSort service_sort,
                const std::string &startup_search,
                atm::StartupSort startup_sort) {
  // ANSI "clear entire screen" + "cursor to home" so the multi-line frame
  // refreshes in place instead of scrolling the terminal.
  std::cout << "\033[2J\033[H";
  std::cout << renderFrame(cpu_usage, memory, processes, stats, sort, view, tree,
                           disk, network, gpu, sensors, systemd, startup,
                           service_search, service_sort, startup_search,
                           startup_sort)
            << std::flush;
}

/// Strips leading/trailing whitespace (including a CR) from a line.
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

/// Strict integer parse (handles an optional leading '-'). Returns false for
/// empty/non-numeric/overflowing text; the flags of `readLine` are not used.
bool parseSignedInteger(const std::string &text, int &out) {
  if (text.empty()) {
    return false;
  }
  errno = 0;
  char *end = nullptr;
  const long value = std::strtol(text.c_str(), &end, 10);
  if (errno == ERANGE || end == text.c_str() || *end != '\0') {
    return false;
  }
  if (value < INT_MIN || value > INT_MAX) {
    return false;
  }
  out = static_cast<int>(value);
  return true;
}

/// Reads raw terminal input without blocking. One complete line at a time is
/// interpreted: digits 1-4 switch the sort order, 'l'/'t' switch views, 'm'
/// enters process-control mode, 'i'/'g' open the network/GPU detail screens;
/// anything else is ignored. Because the live loop must not lose bytes
/// meant for the (blocking) control prompts, all input funnels through an
/// internal buffer shared with readLine().
class ConsoleInput {
 public:
  enum class Command {
    None,
    SortCpu,
    SortMemory,
    SortPid,
    SortName,
    ViewList,
    ViewTree,
    Manage,
    InspectNetwork,
    InspectGpu,
    InspectSensors,
    InspectSystemd,
    InspectStartup,
  };

  /// Non-blocking: drains whatever stdin currently has, then returns the next
  /// complete command line, if any.
  Command pollCommand() {
    drainAvailable();
    const auto command = nextCommand();
    if (command == Command::None && readEof()) {
      return Command::None;
    }
    return command;
  }

  /// Blocking: returns the next complete line, consuming any bytes already in
  /// the buffer first. Returns std::nullopt at EOF (so /dev/null input or
  /// Ctrl+D cancels a prompt). Raw poll()+read() is used exclusively so no
  /// second (stdio) buffer can swallow bytes drained via drainAvailable().
  std::optional<std::string> readLine() {
    for (;;) {
      const std::size_t nl = buffer_.find('\n');
      if (nl != std::string::npos) {
        std::string line = buffer_.substr(0, nl);
        buffer_.erase(0, nl + 1);
        return line;
      }
      if (eof_) {
        if (buffer_.empty()) {
          return std::nullopt;
        }
        std::string line = std::move(buffer_);
        buffer_.clear();
        return line;  // EOF mid-line: hand over whatever arrived
      }
      char ch = '\0';
      if (!readCharBlocking(ch)) {
        eof_ = true;
        continue;  // next loop iteration reports EOF / remaining buffer
      }
      buffer_.push_back(ch);
    }
  }

 private:
  std::string buffer_;
  bool eof_ = false;

  bool readCharBlocking(char &ch) {
    struct pollfd stdin_fd = {STDIN_FILENO, POLLIN, 0};
    if (::poll(&stdin_fd, 1, -1) <= 0) {
      return false;
    }
    ssize_t count = ::read(STDIN_FILENO, &ch, 1);
    if (count > 0) {
      return true;
    }
    if (count < 0 && errno == EINTR) {
      return readCharBlocking(ch);
    }
    return false;  // 0 = EOF, -1 with other errno treated as EOF too
  }

  void drainAvailable() {
    struct pollfd stdin_fd = {STDIN_FILENO, POLLIN, 0};
    if (::poll(&stdin_fd, 1, 0) <= 0 ||
        (stdin_fd.revents & POLLIN) == 0) {
      return;
    }
    char chunk[256];
    const auto count = ::read(STDIN_FILENO, chunk, sizeof(chunk));
    if (count > 0) {
      buffer_.append(chunk, static_cast<std::size_t>(count));
    } else if (count == 0) {
      eof_ = true;
    }
  }

  bool readEof() const { return eof_; }

  /// Interprets and consumes the first buffered line as a command.
  Command nextCommand() {
    const std::size_t nl = buffer_.find('\n');
    if (nl == std::string::npos) {
      return Command::None;
    }
    const std::string token = trimWhitespace(buffer_.substr(0, nl));
    buffer_.erase(0, nl + 1);  // always consume the whole line
    if (token == "1") return Command::SortCpu;
    if (token == "2") return Command::SortMemory;
    if (token == "3") return Command::SortPid;
    if (token == "4") return Command::SortName;
    if (token == "l" || token == "L") return Command::ViewList;
    if (token == "t" || token == "T") return Command::ViewTree;
    if (token == "m" || token == "M") return Command::Manage;
    if (token == "i" || token == "I") return Command::InspectNetwork;
    if (token == "g" || token == "G") return Command::InspectGpu;
    if (token == "s" || token == "S") return Command::InspectSensors;
    if (token == "u" || token == "U") return Command::InspectSystemd;
    if (token == "a" || token == "A") return Command::InspectStartup;
    return Command::None;
  }
};

/// Prints a confirmation prompt and waits for y/Y/yes (anything else is a
/// "no"). Never refuses to return on EOF: EOF cancels.
bool confirm(const std::string &prompt, ConsoleInput &input) {
  std::cout << prompt << " [y/N] " << std::flush;
  const std::optional<std::string> line = input.readLine();
  if (!line) {
    std::cout << '\n';
    return false;
  }
  const std::string answer = trimWhitespace(*line);
  return answer == "y" || answer == "Y" || answer == "yes" ||
         answer == "Yes" || answer == "YES";
}

/// Prints the outcome of a failed action with process/action context.
void printActionFailure(const char *action, int pid,
                        const atm::ActionResult &result) {
  switch (result.status) {
    case atm::ActionStatus::InvalidPid:
      std::cout << atm::actionStatusMessage(atm::ActionStatus::InvalidPid)
                << '\n';
      break;
    case atm::ActionStatus::ProcessNotFound:
      std::cout << "Process does not exist.\n";
      break;
    case atm::ActionStatus::PermissionDenied:
      std::cout << "Permission denied.\n"
                   "You do not have permission to control this process.\n";
      break;
    case atm::ActionStatus::InvalidPriority:
      std::cout << "Invalid priority. Choose a value between " << kMinNice
                << " and " << kMaxNice << ".\n";
      break;
    case atm::ActionStatus::Failed:
      std::cout << "Failed to " << action << " process " << pid << ": "
                << std::strerror(result.errno_value) << '\n';
      break;
    case atm::ActionStatus::Success:
      break;
  }
}

/// Shows the current (sorted) process list for PID selection.
void showProcessSelection(const std::vector<atm::Process> &processes,
                          atm::ProcessSort sort) {
  std::cout << "\033[2J\033[H";
  std::cout << "========================================\n"
               "ARCH TASK MANAGER — Process Control\n"
               "========================================\n\n"
            << "Process list (sorted by " << atm::processSortName(sort)
            << "):\n\n"
            << "    PID  NAME                     CPU        RAM  STATE\n";
  for (const atm::Process &process : processes) {
    std::cout << "  " << formatProcessRow(process) << '\n';
  }
  std::cout << std::flush;
}

/// Result of a successful PID selection for the action menu.
struct SelectedProcess {
  int pid = 0;
  std::string name;
};

/// Asks for a PID with the given prompt and validates it: non-numeric / ≤ 0
/// is "Invalid PID.", PID 1 and the monitor's own PID are protected, and a
/// PID absent from `listed` is rejected. Prints each rejection message itself
/// and returns std::nullopt.
std::optional<SelectedProcess> selectPid(ConsoleInput &input,
                                         const std::vector<atm::Process> &listed,
                                         const char *prompt) {
  std::cout << "\n" << prompt << ":\n> " << std::flush;

  const std::optional<std::string> pid_line = input.readLine();
  if (!pid_line) {
    std::cout << "\nInput cancelled.\n";
    return std::nullopt;
  }
  const std::string pid_text = trimWhitespace(*pid_line);
  if (pid_text.empty()) {
    std::cout << "Cancelled.\n";
    return std::nullopt;
  }

  int pid = 0;
  if (!parseSignedInteger(pid_text, pid) || pid <= 0) {
    std::cout << "Invalid PID.\n";
    return std::nullopt;
  }
  if (pid == 1) {
    std::cout << "PID 1 is protected by the application and cannot be managed "
                 "from this interface.\n";
    return std::nullopt;
  }
  if (pid == static_cast<int>(::getpid())) {
    std::cout << "You cannot manage the Task Manager process.\n";
    return std::nullopt;
  }

  // The list is at most one second old; a process absent from it is treated
  // as gone rather than guessing at a pid that might now be a different one.
  for (const atm::Process &process : listed) {
    if (process.pid == pid) {
      return SelectedProcess{pid, process.name};
    }
  }
  std::cout << "Process does not exist (not found in the current process "
               "list).\n";
  return std::nullopt;
}

/// Runs the shared action menu for one selected process (terminate, kill,
/// pause, resume, change priority). ProcessActions performs every syscall —
/// this UI code never re-implements kill(2)/setpriority(2).
void runActionMenu(atm::ProcessActions &actions, ConsoleInput &input, int pid,
                   const std::string &name) {
  std::cout << "\nProcess:\n"
            << name << "\nPID: " << pid << "\n\n"
            << "Actions:\n"
               "[1] Terminate\n"
               "[2] Kill\n"
               "[3] Pause\n"
               "[4] Resume\n"
               "[5] Change Priority\n"
               "[6] Cancel\n\n"
            << "Select action:\n> " << std::flush;

  const std::optional<std::string> action_line = input.readLine();
  if (!action_line) {
    std::cout << "\nInput cancelled.\n";
    return;
  }
  const std::string action_text = trimWhitespace(*action_line);

  atm::ActionResult result{atm::ActionStatus::Failed, 0};
  bool executed = false;

  switch (action_text == "1" ? 1 : action_text == "2" ? 2
                       : action_text == "3"          ? 3
                       : action_text == "4"          ? 4
                       : action_text == "5"          ? 5
                       : action_text == "6"          ? 6
                                                     : 0) {
    case 1:  // Terminate (SIGTERM)
      if (confirm("Terminate process " + std::to_string(pid) + "?", input)) {
        result = actions.terminate(pid);
        if (result.success()) {
          std::cout << "Process " << pid
                    << " termination signal sent successfully.\n";
        }
        executed = true;
      } else {
        std::cout << "Cancelled.\n";
      }
      break;

    case 2:  // Kill (SIGKILL) — destructive, always requires confirmation.
      std::cout << "\nWARNING:\nYou are about to forcefully kill process:\n\n"
                << "PID: " << pid << "\nName: " << name << "\n\n";
      if (confirm("Continue?", input)) {
        result = actions.kill(pid);
        if (result.success()) {
          std::cout << "Process " << pid << " killed.\n";
        }
        executed = true;
      } else {
        std::cout << "Cancelled.\n";
      }
      break;

    case 3:  // Pause (SIGSTOP)
      if (confirm("Pause process " + std::to_string(pid) + "?", input)) {
        result = actions.pause(pid);
        if (result.success()) {
          std::cout << "Process " << pid << " paused.\n";
        }
        executed = true;
      } else {
        std::cout << "Cancelled.\n";
      }
      break;

    case 4:  // Resume (SIGCONT)
      if (confirm("Resume process " + std::to_string(pid) + "?", input)) {
        result = actions.resume(pid);
        if (result.success()) {
          std::cout << "Process " << pid << " resumed.\n";
        }
        executed = true;
      } else {
        std::cout << "Cancelled.\n";
      }
      break;

    case 5: {  // Change priority.
      const std::optional<int> current = actions.currentPriority(pid);
      if (!current.has_value()) {
        std::cout << "Process does not exist. "
                     "It may have disappeared before the operation "
                     "completed.\n";
        break;
      }
      std::cout << "\nCurrent priority: " << *current << "\n"
                << "Lower values = higher scheduling priority; "
                   "higher values = lower.\n"
                << "Raising priority beyond your limit needs root.\n\n"
                << "Enter new priority (" << kMinNice << " to " << kMaxNice
                << "):\n> " << std::flush;

      const std::optional<std::string> priority_line = input.readLine();
      if (!priority_line) {
        std::cout << "\nInput cancelled.\n";
        break;
      }
      int priority = 0;
      if (!parseSignedInteger(trimWhitespace(*priority_line), priority) ||
          priority < kMinNice || priority > kMaxNice) {
        std::cout << "Invalid priority. Choose a value between " << kMinNice
                  << " and " << kMaxNice << ".\n";
        break;
      }
      result = actions.setPriority(pid, priority);
      if (result.success()) {
        std::cout << "Process " << pid << " priority set to " << priority
                  << ".\n";
      }
      executed = true;
      break;
    }

    case 6:
      std::cout << "Cancelled.\n";
      break;

    default:
      std::cout << "Invalid action.\n";
      break;
  }

  if (executed && !result.success()) {
    printActionFailure(
        action_text == "1" ? "terminate"
            : action_text == "2" ? "kill"
            : action_text == "3" ? "pause"
            : action_text == "5" ? "set priority for"
                                  : "resume",
        pid, result);
  }

  std::cout << "\nRefreshing process list...\n";
  // Give the user a moment to read the outcome before the next frame clears
  // the screen.
  std::this_thread::sleep_for(1500ms);
}

/// Runs one complete "select a process, choose an action" interaction from the
/// flat process-list view.
void runProcessControl(atm::ProcessActions &actions, ConsoleInput &input,
                       const std::vector<atm::Process> &listed,
                       atm::ProcessSort sort) {
  showProcessSelection(listed, sort);
  const auto selected =
      selectPid(input, listed, "Select PID (blank to cancel)");
  if (selected.has_value()) {
    runActionMenu(actions, input, selected->pid, selected->name);
  }
}

/// Manages a process chosen from the tree view. The tree only identifies the
/// selected PID; validation and the action menu are the same shared flow used
/// by the flat table, and ProcessActions performs the actual syscalls.
void manageFromTree(atm::ProcessActions &actions, ConsoleInput &input,
                    const atm::ProcessTree &tree,
                    const std::vector<atm::Process> &listed) {
  std::cout << "\033[2J\033[H";
  std::cout << "========================================\n"
               "ARCH TASK MANAGER — Process Tree Control\n"
               "========================================\n\n"
            << atm::renderProcessTree(tree) << "\n\n";

  const auto selected = selectPid(input, listed, "Enter PID to manage (blank to cancel)");
  if (selected.has_value()) {
    runActionMenu(actions, input, selected->pid, selected->name);
  }
}

/// "i": shows the network table frozen and inspects one interface in detail.
/// The snapshot is ~1 s old; reading it is safe because NetworkMonitor owns
/// all the counter state.
void interactNetworkDetail(const atm::NetworkSnapshot &network,
                           ConsoleInput &input) {
  std::cout << "\033[2J\033[H";
  std::cout << "========================================\n"
               "ARCH TASK MANAGER — Network Interface Detail\n"
               "========================================\n\n"
            << renderNetworkTableText(network) << "\n\n"
            << "Enter interface name to inspect (blank to cancel):\n> "
            << std::flush;

  const std::optional<std::string> line = input.readLine();
  if (!line) {
    std::cout << "\nInput cancelled.\n";
    return;
  }
  const std::string name = trimWhitespace(*line);
  if (name.empty()) {
    std::cout << "Cancelled.\n";
    return;
  }

  const auto found =
      std::find_if(network.interfaces.begin(), network.interfaces.end(),
                   [&](const atm::NetworkInterfaceStats &iface) {
                     return iface.name == name;
                   });
  if (found == network.interfaces.end()) {
    std::cout << "Interface does not exist (not found in the current "
                 "interface list).\n";
    return;
  }

  std::cout << '\n' << buildInterfaceDetail(*found)
            << "\n\nPress Enter to return to the live view.\n" << std::flush;
  static_cast<void>(input.readLine());  // wait for the user, EOF cancels
}

/// "g": shows the GPU summary frozen followed by the full per-GPU breakdown.
/// The snapshot is at most ~1 s old; all device state lives in GpuMonitor.
void interactGpuDetail(const atm::GpuSnapshot &gpu, ConsoleInput &input) {
  std::cout << "\033[2J\033[H";
  std::cout << "========================================\n"
               "ARCH TASK MANAGER — GPU Detail\n"
               "========================================\n\n"
            << renderGpuTableText(gpu) << "\n\n"
            << renderGpuDetails(gpu)
            << "\n\nPress Enter to return to the live view.\n" << std::flush;
  static_cast<void>(input.readLine());  // wait for the user, EOF cancels
}

/// "s": shows the full sensor breakdown (current/maximum/critical/status for
/// every temperature, RPM for every fan). The snapshot is at most ~1 s old;
/// the SensorMonitor owns all cached channel state.
void interactSensorDetail(const atm::SensorSnapshot &sensors,
                          const atm::GpuSnapshot &gpu, ConsoleInput &input) {
  std::cout << "\033[2J\033[H";
  std::cout << "========================================\n"
               "ARCH TASK MANAGER — Sensor Detail\n"
               "========================================\n\n"
            << renderSensorDetails(sensors, gpu)
            << "\n\nPress Enter to return to the live view.\n" << std::flush;
  static_cast<void>(input.readLine());  // wait for the user, EOF cancels
}

/// "u": shows the systemd services list frozen and provides management options.
/// Shows a choose-a-sort prompt for systemd services; updates `service_sort`.
void chooseServiceSort(ConsoleInput &input, atm::ServiceSort &service_sort) {
  std::cout << "\nSort services by:\n"
               "[1] Name\n"
               "[2] Status\n"
               "[3] Enabled state\n"
               "[4] Description\n\n"
               "Enter a number (default: Name):\n> "
            << std::flush;
  const std::optional<std::string> line = input.readLine();
  if (!line) {
    std::cout << "\nInput cancelled.\n";
    return;
  }
  const std::string choice = trimWhitespace(*line);
  if (choice == "1") service_sort = atm::ServiceSort::Name;
  else if (choice == "2") service_sort = atm::ServiceSort::Status;
  else if (choice == "3") service_sort = atm::ServiceSort::Enabled;
  else if (choice == "4") service_sort = atm::ServiceSort::Description;
  else std::cout << "Invalid choice; keeping the current sort.\n";
}

/// "u": shows the systemd services list frozen and provides management options.
void interactSystemdDetail(atm::SystemdManager &systemd_mgr,
                           const atm::SystemdSnapshot &systemd,
                           ConsoleInput &input, std::string &service_search,
                           atm::ServiceSort &service_sort) {
  const std::vector<atm::SystemdService> services =
      filterAndSortServices(systemd.services, service_search, service_sort);

  std::cout << "\033[2J\033[H";
  std::cout << "========================================\n"
               "ARCH TASK MANAGER — Systemd Service Management\n"
               "========================================\n\n"
            << renderSystemdTableText(services, systemd.available);
  if (systemd.available && !service_search.empty()) {
    std::cout << "Showing " << services.size() << " of "
              << systemd.services.size() << " services (search: \""
              << service_search << "\")\n";
  }
  std::cout << "Sort: " << atm::serviceSortName(service_sort) << "\n\n"
            << "Management:\n"
               "[1] Start Service\n"
               "[2] Stop Service\n"
               "[3] Restart Service\n"
               "[4] Enable Service\n"
               "[5] Disable Service\n"
               "[6] View Service Details\n"
               "[7] Search/Filter Services\n"
               "[8] Sort Services\n"
               "[9] Refresh Services\n"
               "[0] Cancel\n\n"
               "Select action:\n> "
            << std::flush;

  const std::optional<std::string> action_line = input.readLine();
  if (!action_line) {
    std::cout << "\nInput cancelled.\n";
    return;
  }
  const std::string action_text = trimWhitespace(*action_line);
  if (action_text == "0" || action_text.empty()) {
    std::cout << "Cancelled.\n";
    return;
  }
  if (action_text != "1" && action_text != "2" && action_text != "3" &&
      action_text != "4" && action_text != "5" && action_text != "6" &&
      action_text != "7" && action_text != "8" && action_text != "9") {
    std::cout << "Invalid action.\n";
    return;
  }

  // Search, sort and refresh do not require a service name.
  if (action_text == "7" || action_text == "8" || action_text == "9") {
    if (action_text == "7") {
      std::cout << "\nSearch services (name/description, case-insensitive;\n"
                   "blank to clear):\n> "
                << std::flush;
      const std::optional<std::string> search_line = input.readLine();
      if (!search_line) {
        std::cout << "\nInput cancelled.\n";
        return;
      }
      service_search = trimWhitespace(*search_line);
      if (service_search.empty()) {
        std::cout << "Search cleared.\n";
      } else {
        std::cout << "Filtering services by: \"" << service_search << "\"\n";
      }
    } else if (action_text == "8") {
      chooseServiceSort(input, service_sort);
    } else {
      std::cout << "\nRefreshing services...\n";
      std::this_thread::sleep_for(500ms);
    }
    return;
  }

  if (action_text == "6") {
    std::cout << "\nEnter service name (e.g. sshd.service):\n> " << std::flush;
    const std::optional<std::string> name_line = input.readLine();
    if (!name_line) {
      std::cout << "\nInput cancelled.\n";
      return;
    }
    std::string service_name = trimWhitespace(*name_line);
    if (service_name.empty()) {
      std::cout << "Cancelled.\n";
      return;
    }
    const auto found =
        std::find_if(systemd.services.begin(), systemd.services.end(),
                     [&](const atm::SystemdService &svc) {
                       return svc.name == service_name;
                     });
    if (found == systemd.services.end()) {
      std::cout << "Service not found: " << service_name << "\n";
      return;
    }
    std::cout << '\n' << renderSystemdServiceDetails(*found)
              << "\n\nPress Enter to return to the live view.\n"
              << std::flush;
    static_cast<void>(input.readLine());
    return;
  }

  // Ask for the service name.
  std::cout << "\nEnter service name (e.g. sshd.service):\n> " << std::flush;
  const std::optional<std::string> name_line = input.readLine();
  if (!name_line) {
    std::cout << "\nInput cancelled.\n";
    return;
  }
  std::string service_name = trimWhitespace(*name_line);
  if (service_name.empty()) {
    std::cout << "Cancelled.\n";
    return;
  }
  // Auto-append .service suffix if the user only typed the base name.
  if (service_name.size() < 8 ||
      service_name.compare(service_name.size() - 8, 8, ".service") != 0) {
    service_name += ".service";
  }

  atm::ServiceOperationResult result;
  std::string prompt;
  if (action_text == "1") {
    prompt = "Start " + service_name + "?";
  } else if (action_text == "2") {
    prompt = "Stop " + service_name + "?";
  } else if (action_text == "3") {
    prompt = "Restart " + service_name + "?";
  } else if (action_text == "4") {
    prompt = "Enable " + service_name + " at boot?";
  } else if (action_text == "5") {
    prompt = "Disable " + service_name + " at boot?";
  }

  if (!confirm(prompt, input)) {
    std::cout << "Cancelled.\n";
    std::this_thread::sleep_for(500ms);
    return;
  }

  if (action_text == "1") {
    result = systemd_mgr.startService(service_name);
  } else if (action_text == "2") {
    result = systemd_mgr.stopService(service_name);
  } else if (action_text == "3") {
    result = systemd_mgr.restartService(service_name);
  } else if (action_text == "4") {
    result = systemd_mgr.enableService(service_name);
  } else if (action_text == "5") {
    result = systemd_mgr.disableService(service_name);
  }

  if (result.success()) {
    std::cout << "Operation completed successfully.\n";
  } else {
    switch (result.status) {
      case atm::ServiceOperationStatus::PermissionDenied:
        std::cout << "Permission denied.\n";
        break;
      case atm::ServiceOperationStatus::UnitNotFound:
        std::cout << "Service not found: " << service_name << '\n';
        break;
      case atm::ServiceOperationStatus::BusError:
      case atm::ServiceOperationStatus::Failed:
        std::cout << "Failed: "
                  << (result.error_message.empty()
                          ? "unknown error"
                          : result.error_message)
                  << '\n';
        break;
      case atm::ServiceOperationStatus::Success:
        break;
    }
  }

  std::cout << "\nRefreshing services...\n";
  std::this_thread::sleep_for(1000ms);
}

/// "1"/"2"/"3" prompt to change the startup application sort order; updates
/// `startup_sort`.
void chooseStartupSort(ConsoleInput &input, atm::StartupSort &startup_sort) {
  std::cout << "\nSort startup applications by:\n"
               "[1] Name\n"
               "[2] Enabled state\n"
               "[3] Scope\n\n"
               "Enter a number (default: Name):\n> "
            << std::flush;
  const std::optional<std::string> line = input.readLine();
  if (!line) {
    std::cout << "\nInput cancelled.\n";
    return;
  }
  const std::string choice = trimWhitespace(*line);
  if (choice == "1") startup_sort = atm::StartupSort::Name;
  else if (choice == "2") startup_sort = atm::StartupSort::Enabled;
  else if (choice == "3") startup_sort = atm::StartupSort::Scope;
  else std::cout << "Invalid choice; keeping the current sort.\n";
}

/// "a": shows the startup applications list frozen and provides management
/// options (details, enable, disable, search, sort, refresh).
void interactStartupDetail(atm::StartupManager &startup_mgr,
                           const atm::StartupSnapshot &startup,
                           ConsoleInput &input,
                           std::string &startup_search,
                           atm::StartupSort &startup_sort) {
  const std::vector<atm::StartupApplication> apps =
      filterAndSortStartupApps(startup.apps, startup_search, startup_sort);

  std::cout << "\033[2J\033[H";
  std::cout << "========================================\n"
               "ARCH TASK MANAGER — Startup Applications\n"
               "========================================\n\n"
            << renderStartupTableText(apps, startup.available);
  if (startup.available && !startup_search.empty()) {
    std::cout << "Showing " << apps.size() << " of " << startup.apps.size()
              << " applications (search: \"" << startup_search << "\")\n";
  }
  std::cout << "Sort: " << atm::startupSortName(startup_sort) << "\n\n"
            << "Management:\n"
               "[1] Enable Startup Application\n"
               "[2] Disable Startup Application\n"
               "[3] View Application Details\n"
               "[4] Search/Filter Applications\n"
               "[5] Sort Applications\n"
               "[6] Refresh Application List\n"
               "[0] Cancel\n\n"
               "Select action:\n> "
            << std::flush;

  const std::optional<std::string> action_line = input.readLine();
  if (!action_line) {
    std::cout << "\nInput cancelled.\n";
    return;
  }
  const std::string action_text = trimWhitespace(*action_line);
  if (action_text == "0" || action_text.empty()) {
    std::cout << "Cancelled.\n";
    return;
  }
  if (action_text != "1" && action_text != "2" && action_text != "3" &&
      action_text != "4" && action_text != "5" && action_text != "6") {
    std::cout << "Invalid action.\n";
    return;
  }

  // Search, sort and refresh do not require an application name.
  if (action_text == "4" || action_text == "5" || action_text == "6") {
    if (action_text == "4") {
      std::cout << "\nSearch startup applications (name, command,\n"
                   "description, file name; blank to clear):\n> "
                << std::flush;
      const std::optional<std::string> search_line = input.readLine();
      if (!search_line) {
        std::cout << "\nInput cancelled.\n";
        return;
      }
      startup_search = trimWhitespace(*search_line);
      if (startup_search.empty()) {
        std::cout << "Search cleared.\n";
      } else {
        std::cout << "Filtering applications by: \"" << startup_search
                  << "\"\n";
      }
    } else if (action_text == "5") {
      chooseStartupSort(input, startup_sort);
    } else {
      std::cout << "\nRefreshing startup applications...\n";
      startup_mgr.refresh();
      std::this_thread::sleep_for(300ms);
    }
    return;
  }

  if (action_text == "3") {
    std::cout << "\nEnter application name or file name\n"
                 "(e.g. discord or discord.desktop):\n> "
              << std::flush;
    const std::optional<std::string> name_line = input.readLine();
    if (!name_line) {
      std::cout << "\nInput cancelled.\n";
      return;
    }
    std::string app_key = trimWhitespace(*name_line);
    if (app_key.empty()) {
      std::cout << "Cancelled.\n";
      return;
    }
    if (app_key.size() < 8 ||
        app_key.compare(app_key.size() - 8, 8, ".desktop") != 0) {
      app_key += ".desktop";
    }
    const auto found =
        std::find_if(startup.apps.begin(), startup.apps.end(),
                     [&](const atm::StartupApplication &app) {
                       return app.id == app_key || app.name == app_key;
                     });
    if (found == startup.apps.end()) {
      std::cout << "Startup application not found: " << app_key << "\n";
      return;
    }
    std::cout << '\n' << renderStartupDetails(*found)
              << "\n\nPress Enter to return to the live view.\n"
              << std::flush;
    static_cast<void>(input.readLine());
    return;
  }

  // Ask for the application to enable/disable.
  std::cout << "\nEnter application name or file name\n"
               "(e.g. discord or discord.desktop):\n> "
            << std::flush;
  const std::optional<std::string> name_line = input.readLine();
  if (!name_line) {
    std::cout << "\nInput cancelled.\n";
    return;
  }
  std::string app_key = trimWhitespace(*name_line);
  if (app_key.empty()) {
    std::cout << "Cancelled.\n";
    return;
  }
  if (app_key.size() < 8 ||
      app_key.compare(app_key.size() - 8, 8, ".desktop") != 0) {
    app_key += ".desktop";
  }
  const auto found =
      std::find_if(startup.apps.begin(), startup.apps.end(),
                   [&](const atm::StartupApplication &app) {
                     return app.id == app_key || app.name == app_key;
                   });
  if (found == startup.apps.end()) {
    std::cout << "Startup application not found: " << app_key << "\n";
    return;
  }
  const atm::StartupApplication &app = *found;

  const std::string prompt = (action_text == "1")
                                 ? "Enable " + app.name +
                                       " to start automatically at login?"
                                 : "Disable " + app.name +
                                       " from starting automatically at login?";
  if (!confirm(prompt, input)) {
    std::cout << "Cancelled.\n";
    std::this_thread::sleep_for(300ms);
    return;
  }

  const atm::StartupOperationResult result = (action_text == "1")
                                                 ? startup_mgr.enableApp(app.id)
                                                 : startup_mgr.disableApp(app.id);
  if (result.success()) {
    std::cout << "Started automatically at login: "
              << ((action_text == "1") ? "enabled" : "disabled") << ".\n";
  } else {
    switch (result.status) {
      case atm::StartupOperationStatus::PermissionDenied:
        std::cout << "Permission denied.\n"
                     "Cannot write to the user autostart directory.\n";
        break;
      case atm::StartupOperationStatus::NotFound:
        std::cout << "Startup application not found: " << app_key << "\n";
        break;
      case atm::StartupOperationStatus::IoError:
        std::cout << "Failed: "
                  << (result.message.empty() ? "unknown error"
                                             : result.message)
                  << '\n';
        break;
      case atm::StartupOperationStatus::Success:
        break;
    }
  }

  std::cout << "\nRefreshing startup applications...\n";
  std::this_thread::sleep_for(300ms);
}

}  // namespace

int main() {
  atm::CpuMonitor cpu_monitor;
  atm::MemoryMonitor memory_monitor;
  atm::ProcessMonitor process_monitor;
  atm::DiskMonitor disk_monitor;
  atm::NetworkMonitor network_monitor;
  atm::GpuMonitor gpu_monitor;
  atm::SensorMonitor sensor_monitor;
  atm::SystemdManager systemd_manager;
  atm::StartupManager startup_manager;
  atm::ProcessActions actions;
  ConsoleInput input;

  atm::ProcessSort sort = atm::ProcessSort::Cpu;
  ViewMode view = ViewMode::List;
  atm::ServiceSort service_sort = atm::ServiceSort::Name;
  std::string service_search;
  atm::StartupSort startup_sort = atm::StartupSort::Name;
  std::string startup_search;

  // Choose the starting view. EOF (e.g. /dev/null stdin) defaults to List.
  std::cout << "Select view:\n"
               "[1] Process List\n"
               "[2] Process Tree\n\n"
               "Enter a number, or press Enter for the default (Process List):\n> "
            << std::flush;
  const std::optional<std::string> view_choice = input.readLine();
  if (view_choice && trimWhitespace(*view_choice) == "2") {
    view = ViewMode::Tree;
  }

  // Baseline samples so the first printed frame already shows real deltas:
  // CPU usage, per-process CPU, disk, network and GPU (Intel RC6) over the
  // sleep below.
  static_cast<void>(cpu_monitor.readUsage());
  static_cast<void>(process_monitor.read(0));
  static_cast<void>(disk_monitor.read());
  static_cast<void>(network_monitor.read());
  static_cast<void>(gpu_monitor.read());
  sensor_monitor.discover();
  static_cast<void>(sensor_monitor.read());
  systemd_manager.discover();
  std::this_thread::sleep_for(kRefreshInterval);

  const auto first_cpu = cpu_monitor.readUsage();
  if (!first_cpu.has_value()) {
    std::cerr << "ERROR: could not read CPU usage from /proc/stat\n";
    return EXIT_FAILURE;
  }

  const auto first_memory = memory_monitor.read();
  if (!first_memory.has_value()) {
    std::cerr << "Error: Unable to read /proc/meminfo\n";
    return EXIT_FAILURE;
  }

  const atm::DiskSnapshot first_disk = disk_monitor.read();
  const atm::NetworkSnapshot first_network = network_monitor.read();
  const atm::GpuSnapshot first_gpu = gpu_monitor.read();
  const atm::SensorSnapshot first_sensors = sensor_monitor.read();
  const atm::SystemdSnapshot first_systemd = systemd_manager.read();
  const atm::StartupSnapshot first_startup = startup_manager.read();
  auto snapshot = process_monitor.read(first_memory->total);
  atm::sortProcesses(snapshot.processes, sort);
  atm::ProcessTree tree = atm::buildProcessTree(snapshot.processes);
  renderView(*first_cpu, *first_memory, snapshot.processes, snapshot.stats,
             sort, view, tree, first_disk, first_network, first_gpu,
             first_sensors, first_systemd, first_startup, service_search,
             service_sort, startup_search, startup_sort);

  atm::NetworkSnapshot network = first_network;
  atm::GpuSnapshot gpu = first_gpu;
  atm::SensorSnapshot sensors = first_sensors;
  atm::SystemdSnapshot systemd = first_systemd;
  atm::StartupSnapshot startup = first_startup;

  for (;;) {
    std::this_thread::sleep_for(kRefreshInterval);

    switch (input.pollCommand()) {
      case ConsoleInput::Command::SortCpu:
        sort = atm::ProcessSort::Cpu;
        break;
      case ConsoleInput::Command::SortMemory:
        sort = atm::ProcessSort::Memory;
        break;
      case ConsoleInput::Command::SortPid:
        sort = atm::ProcessSort::Pid;
        break;
      case ConsoleInput::Command::SortName:
        sort = atm::ProcessSort::Name;
        break;
      case ConsoleInput::Command::ViewList:
        view = ViewMode::List;
        break;
      case ConsoleInput::Command::ViewTree:
        view = ViewMode::Tree;
        break;
      case ConsoleInput::Command::Manage:
        if (view == ViewMode::Tree) {
          manageFromTree(actions, input, tree, snapshot.processes);
        } else {
          runProcessControl(actions, input, snapshot.processes, sort);
        }
        // Refresh immediately so the effect of the action is visible without
        // waiting for the next 1 s tick.
        {
          const auto cpu = cpu_monitor.readUsage();
          const auto memory = memory_monitor.read();
          if (cpu.has_value() && memory.has_value()) {
            const atm::DiskSnapshot disk = disk_monitor.read();
            const atm::NetworkSnapshot network = network_monitor.read();
            gpu = gpu_monitor.read();
            sensors = sensor_monitor.read();
            systemd = systemd_manager.read();
            startup = startup_manager.read();
            snapshot = process_monitor.read(memory->total);
            atm::sortProcesses(snapshot.processes, sort);
            tree = atm::buildProcessTree(snapshot.processes);
            renderView(*cpu, *memory, snapshot.processes, snapshot.stats,
                       sort, view, tree, disk, network, gpu, sensors,
                       systemd, startup, service_search, service_sort,
                       startup_search, startup_sort);
          }
        }
        continue;
      case ConsoleInput::Command::InspectNetwork:
        if (view == ViewMode::List) {
          interactNetworkDetail(network, input);
        }
        break;
      case ConsoleInput::Command::InspectGpu:
        if (view == ViewMode::List) {
          interactGpuDetail(gpu, input);
        }
        break;
      case ConsoleInput::Command::InspectSensors:
        if (view == ViewMode::List) {
          interactSensorDetail(sensors, gpu, input);
        }
        break;
      case ConsoleInput::Command::InspectSystemd:
        if (view == ViewMode::List) {
          interactSystemdDetail(systemd_manager, systemd, input, service_search,
                              service_sort);
        }
        break;
      case ConsoleInput::Command::InspectStartup:
        if (view == ViewMode::List) {
          interactStartupDetail(startup_manager, startup, input, startup_search,
                                startup_sort);
        }
        break;
      case ConsoleInput::Command::None:
        break;
    }

    const auto cpu = cpu_monitor.readUsage();
    if (!cpu.has_value()) {
      std::cerr << "\nERROR: could not read CPU usage from /proc/stat\n";
      return EXIT_FAILURE;
    }

    const auto memory = memory_monitor.read();
    if (!memory.has_value()) {
      std::cerr << "\nError: Unable to read /proc/meminfo\n";
      return EXIT_FAILURE;
    }

    snapshot = process_monitor.read(memory->total);
    atm::sortProcesses(snapshot.processes, sort);
    tree = atm::buildProcessTree(snapshot.processes);
    const atm::DiskSnapshot disk = disk_monitor.read();
    network = network_monitor.read();
    gpu = gpu_monitor.read();
    sensors = sensor_monitor.read();
    systemd = systemd_manager.read();
    startup = startup_manager.read();
    renderView(*cpu, *memory, snapshot.processes, snapshot.stats, sort, view,
               tree, disk, network, gpu, sensors, systemd, startup,
               service_search, service_sort, startup_search, startup_sort);
  }
}