#include <array>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <optional>
#include <poll.h>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

#include "alert_manager.hpp"
#include "cpu_monitor.hpp"
#include "disk_monitor.hpp"
#include "format_bytes.hpp"
#include "gpu_monitor.hpp"
#include "history_manager.hpp"
#include "logger.hpp"
#include "memory_monitor.hpp"
#include "network_monitor.hpp"
#include "notification_manager.hpp"
#include "package_manager.hpp"
#include "package_transaction.hpp"
#include "process_actions.hpp"
#include "process_details.hpp"
#include "process_monitor.hpp"
#include "process_tree.hpp"
#include "resource_history.hpp"
#include "sensor_monitor.hpp"
#include "settings.hpp"
#include "settings_apply.hpp"
#include "settings_manager.hpp"
#include "startup_manager.hpp"
#include "system_info.hpp"
#include "systemd_manager.hpp"

namespace {

using namespace std::chrono_literals;

/// Which full-screen view the live loop renders each second.
enum class ViewMode {
  List,  // the flat process table (default)
  Tree,  // the parent/child process tree
};

/// Simple filter for the recent-alerts list. Cycled by pressing 'f' (Enter).
enum class AlertFilter {
  All,
  Warning,
  Critical,
  Recovery,
};

/// Human-readable name of an AlertFilter ("All", "Warning", ...).
const char *alertFilterName(AlertFilter filter) {
  switch (filter) {
    case AlertFilter::All:      return "All";
    case AlertFilter::Warning:  return "Warning";
    case AlertFilter::Critical: return "Critical";
    case AlertFilter::Recovery: return "Recovery";
  }
  return "All";
}

/// Returns the next AlertFilter when the cycling filter key is pressed.
AlertFilter nextAlertFilter(AlertFilter filter) {
  switch (filter) {
    case AlertFilter::All:      return AlertFilter::Warning;
    case AlertFilter::Warning:  return AlertFilter::Critical;
    case AlertFilter::Critical: return AlertFilter::Recovery;
    case AlertFilter::Recovery: return AlertFilter::All;
  }
  return AlertFilter::All;
}

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
constexpr std::size_t kSysInfoDetailRuleWidth = 32;
constexpr std::size_t kPackageNameWidth = 24;
constexpr std::size_t kPackageVersionWidth = 16;
constexpr std::size_t kPackageSrcWidth = 10;
constexpr std::size_t kPackageDetailRuleWidth = 32;
constexpr std::size_t kMaxPackageFrameRows = 15;  // main-frame table cap
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

/// Formats the runtime refresh interval (milliseconds) as seconds ("1.0 s").
std::string formatRefreshInterval(int refresh_interval_ms) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(1)
      << (refresh_interval_ms / 1000.0) << " s";
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

/// Renders the compact SYSTEM INFORMATION overview at the top of the live
/// view (Step 12). Only the few high-level identity values are shown here
/// every second; the full breakdown (CPU topology, memory/swap capacity, DMI
/// hardware, BIOS) is available on the interactive 'y' screen.
void renderSystemInfoSections(std::ostringstream &out,
                              const atm::SystemInfo &info) {
  const auto text = [](const std::string &value) {
    return value.empty() ? std::string("N/A") : value;
  };
  out << "\n## SYSTEM INFORMATION\n\n";
  appendLabeled(out, "Operating System:", text(info.operating_system));
  appendLabeled(out, "Kernel:", text(info.kernel_version));
  appendLabeled(out, "Architecture:", text(info.architecture));
  appendLabeled(out, "Uptime:", atm::formatUptime(info.uptime_seconds));
}

/// Full system/hardware breakdown used by the interactive 'y' (System
/// Information) screen. Groups the values under SYSTEM / CPU / MEMORY / GPU /
/// HARDWARE headings; every unavailable field is shown as "N/A".
std::string renderSystemInfoDetail(const atm::SystemInfo &info) {
  const auto text = [](const std::string &value) {
    return value.empty() ? std::string("N/A") : value;
  };
  const auto kib = [](std::uint64_t bytes) {
    return formatKibibytes(bytes / kBytesPerKilobyte);
  };
  const auto rule = std::string(kSysInfoDetailRuleWidth, '-');

  std::ostringstream out;
  out << "SYSTEM INFORMATION\n" << rule << "\n";
  appendLabeled(out, "Hostname:", text(info.hostname));
  appendLabeled(out, "Operating System:", text(info.operating_system));
  appendLabeled(out, "Distribution:", text(info.distribution));
  if (!info.distribution_version.empty()) {
    appendLabeled(out, "Distribution Version:", info.distribution_version);
  }
  appendLabeled(out, "Kernel:", text(info.kernel_version));
  if (!info.kernel_release.empty()) {
    appendLabeled(out, "Kernel Release:", info.kernel_release);
  }
  appendLabeled(out, "Architecture:", text(info.architecture));
  appendLabeled(out, "Uptime:", atm::formatUptime(info.uptime_seconds));

  out << "\nCPU\n" << rule << "\n";
  appendLabeled(out, "Model:", text(info.cpu_model));
  appendLabeled(out, "Architecture:", text(info.cpu_architecture));
  appendLabeled(
      out, "Logical CPUs:",
      info.cpu_logical_cores > 0 ? std::to_string(info.cpu_logical_cores)
                                 : std::string("N/A"));
  appendLabeled(
      out, "Physical cores:",
      info.cpu_physical_cores > 0 ? std::to_string(info.cpu_physical_cores)
                                  : std::string("N/A"));

  out << "\nMEMORY\n" << rule << "\n";
  appendLabeled(out, "RAM:", info.total_memory > 0 ? kib(info.total_memory)
                                                   : std::string("N/A"));
  appendLabeled(out, "Swap:", info.total_swap > 0 ? kib(info.total_swap)
                                                  : std::string("N/A"));

  out << "\nGPU\n" << rule << "\n";
  if (info.gpus.empty()) {
    appendLabeled(out, "GPU:", "N/A");
  } else {
    for (std::size_t index = 0; index < info.gpus.size(); ++index) {
      appendLabeled(out, "GPU " + std::to_string(index) + ":",
                    info.gpus[index]);
    }
  }

  out << "\nHARDWARE\n" << rule << "\n";
  appendLabeled(out, "Manufacturer:", text(info.manufacturer));
  appendLabeled(out, "Model:", text(info.product_name));
  if (!info.product_version.empty()) {
    appendLabeled(out, "Product Version:", info.product_version);
  }
  appendLabeled(out, "Motherboard:", text(info.motherboard_vendor) +
                                         (info.motherboard.empty()
                                              ? ""
                                              : " " + info.motherboard));
  out << "\nFIRMWARE\n" << rule << "\n";
  appendLabeled(out, "Vendor:", text(info.bios_vendor));
  appendLabeled(out, "Version:", text(info.bios_version));
  appendLabeled(out, "Date:", text(info.bios_date));
  return out.str();
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

/// Renders the RESOURCE HISTORY section: live time-series graphs for CPU,
/// memory, swap, disk, network, GPU and temperature. All data is read from
/// the HistoryManager's bounded ring buffers (already-collected values); the
/// renderer never reads system metrics itself.
void renderResourceHistorySection(std::ostringstream &out,
                                  const atm::HistoryManager &history) {
  out << "\n## RESOURCE HISTORY\n\n";

  atm::GraphConfig percent;      // 0-100 fixed scale
  percent.width = 40;
  percent.height = 6;
  percent.dynamic_scale = false;

  atm::GraphConfig rate;         // auto-scaling MB-scale
  rate.width = 40;
  rate.height = 6;
  rate.dynamic_scale = true;

  // Overall CPU usage (%).
  out << "[CPU Usage]\n" << atm::GraphRenderer::renderText(
      history.cpuHistory(), percent, "", "%") << '\n';

  // Memory usage (%).
  out << "\n[MEMORY Usage]\n" << atm::GraphRenderer::renderText(
      history.memoryHistory(), percent, "", "%") << '\n';

  // Swap usage (%).
  out << "\n[SWAP Usage]\n" << atm::GraphRenderer::renderText(
      history.swapHistory(), percent, "", "%") << '\n';

  // Disk throughput (auto-scaling, MB-scale).
  out << "\n[DISK READ]\n" << atm::GraphRenderer::renderText(
      history.diskReadHistory(), rate, "", "B/s") << '\n';
  out << "\n[DISK WRITE]\n" << atm::GraphRenderer::renderText(
      history.diskWriteHistory(), rate, "", "B/s") << '\n';

  // Network throughput (auto-scaling).
  out << "\n[NETWORK RX]\n" << atm::GraphRenderer::renderText(
      history.networkRxHistory(), rate, "", "B/s") << '\n';
  out << "\n[NETWORK TX]\n" << atm::GraphRenderer::renderText(
      history.networkTxHistory(), rate, "", "B/s") << '\n';

  // GPU utilization (per GPU).
  const auto &gpus = history.gpuHistories();
  if (!gpus.empty()) {
    out << "\n[GPU Utilization]\n";
    for (std::size_t i = 0; i < gpus.size(); ++i) {
      const std::string name = "GPU " + std::to_string(i) + " " + gpus[i].name;
      out << name << '\n' << atm::GraphRenderer::renderText(
          gpus[i].utilization, percent, "", "%") << '\n';
    }
  } else {
    out << "\n[GPU]\n"
        << "N/A (no GPU utilization data).\n";
  }

  // Temperature sensors (auto CPU-like scale up to ~110C).
  const auto &sensors = history.sensorHistories();
  if (!sensors.empty()) {
    atm::GraphConfig temp;
    temp.width = 40;
    temp.height = 6;
    temp.dynamic_scale = true;
    out << "\n[TEMPERATURES]\n";
    for (const auto &sensor : sensors) {
      out << sensor.label << '\n' << atm::GraphRenderer::renderText(
          sensor.temperature, temp, "", "\u00b0C") << '\n';
    }
  } else {
    out << "\n[TEMPERATURES]\n"
        << "N/A (no hardware sensors).\n";
  }

  out << "\nGraphs update every refresh; buffer holds "
      << history.maxSamples() << " samples.\n";
}

/// Symbol shown for an AlertSeverity in the dashboard and alert lists.
const char *severitySymbol(atm::AlertSeverity severity) {
  switch (severity) {
    case atm::AlertSeverity::Normal:   return "\u25cf";             // ●
    case atm::AlertSeverity::Warning:  return "\u26a0";             // ⚠
    case atm::AlertSeverity::Critical: return "\U0001f534";         // 🔴
  }
  return "?";
}

/// Formats a metric value with its unit for alert messages.
std::string formatAlertValue(atm::AlertType type, double value) {
  std::ostringstream out;
  if (type == atm::AlertType::Temperature) {
    out << std::fixed << std::setprecision(0) << value << "\u00b0C";
    return out.str();
  }
  if (type == atm::AlertType::DiskReadActivity ||
      type == atm::AlertType::DiskWriteActivity ||
      type == atm::AlertType::NetworkReceive ||
      type == atm::AlertType::NetworkTransmit) {
    return atm::GraphRenderer::formatRate(value);  // bytes/sec -> B/s
  }
  out << std::fixed << std::setprecision(0) << value << '%';
  return out.str();
}

/// Builds a short human message describing an active alert subject.
std::string describeAlertSubject(atm::AlertType type, const std::string &source,
                                 double value) {
  const std::string v = formatAlertValue(type, value);
  switch (type) {
    case atm::AlertType::CpuUsage:
      return "CPU usage: " + v;
    case atm::AlertType::MemoryUsage:
      return "Memory usage: " + v;
    case atm::AlertType::SwapUsage:
      return "Swap usage: " + v;
    case atm::AlertType::DiskUsage:
      return source + " is " + v + " full";
    case atm::AlertType::DiskReadActivity:
      return "High disk read activity: " + v;
    case atm::AlertType::DiskWriteActivity:
      return "High disk write activity: " + v;
    case atm::AlertType::NetworkReceive:
      return "High network receive rate: " + v;
    case atm::AlertType::NetworkTransmit:
      return "High network transmit rate: " + v;
    case atm::AlertType::GpuUsage:
      return source + " usage: " + v;
    case atm::AlertType::GpuMemoryUsage:
      return source + " VRAM usage: " + v;
    case atm::AlertType::Temperature:
      return source + " temperature: " + v;
  }
  return source + ": " + v;
}

/// Worst currently-active severity across a set of alert types.
atm::AlertSeverity dashboardSeverity(const atm::AlertManager &alerts,
                                     const std::vector<atm::AlertType> &types) {
  atm::AlertSeverity worst = atm::AlertSeverity::Normal;
  for (const auto &a : alerts.activeAlerts()) {
    bool match = false;
    for (atm::AlertType t : types) {
      if (a.type == t) {
        match = true;
        break;
      }
    }
    if (match && a.severity > worst) {
      worst = a.severity;
    }
  }
  return worst;
}

/// Formats an AlertEvent timestamp as HH:MM in local time.
std::string formatAlertTimestamp(
    const std::chrono::system_clock::time_point &timestamp) {
  const std::time_t t = std::chrono::system_clock::to_time_t(timestamp);
  const std::tm *tm = std::localtime(&t);
  char buf[6];
  std::strftime(buf, sizeof(buf), "%H:%M", tm);
  return std::string(buf);
}

/// Renders the SYSTEM ALERTS dashboard plus active and recent alerts. Reads
/// only AlertManager state, which itself consumes the existing monitor
/// snapshots; no system metrics are re-read here.
void renderAlertsSections(std::ostringstream &out,
                          const atm::AlertManager &alerts,
                          AlertFilter filter) {
  out << "\n## SYSTEM ALERTS\n\n";

  const struct {
    const char *label;
    std::vector<atm::AlertType> types;
  } rows[] = {
      {"CPU",        {atm::AlertType::CpuUsage}},
      {"RAM",        {atm::AlertType::MemoryUsage}},
      {"Swap",       {atm::AlertType::SwapUsage}},
      {"Disk",       {atm::AlertType::DiskUsage, atm::AlertType::DiskReadActivity,
                      atm::AlertType::DiskWriteActivity}},
      {"GPU",        {atm::AlertType::GpuUsage, atm::AlertType::GpuMemoryUsage}},
      {"Temperature", {atm::AlertType::Temperature}},
      {"Network",    {atm::AlertType::NetworkReceive,
                      atm::AlertType::NetworkTransmit}},
  };
  for (const auto &row : rows) {
    const atm::AlertSeverity sev = dashboardSeverity(alerts, row.types);
    out << std::left << std::setw(13) << row.label << severitySymbol(sev) << ' '
        << atm::alertSeverityName(sev) << '\n';
  }
  out << "Severity: \u25cf Normal  \u26a0 Warning  \U0001f534 Critical\n";

  // Active alerts: subjects currently in a Warning or Critical state.
  out << "\n### ACTIVE ALERTS\n";
  const auto active = alerts.activeAlerts();
  if (active.empty()) {
    out << "\u2713 No active alerts\n";
  } else {
    for (const auto &a : active) {
      out << severitySymbol(a.severity) << ' '
          << describeAlertSubject(a.type, a.source, a.value) << '\n';
    }
  }

  // Recent alerts (bounded history), filtered and shown most-recent-first.
  out << "\n### RECENT ALERTS (filter: " << alertFilterName(filter) << ")\n";
  std::vector<const atm::AlertEvent *> matching;
  for (auto it = alerts.history().rbegin();
       it != alerts.history().rend() && matching.size() < 8; ++it) {
    const atm::AlertEvent &e = *it;
    bool show = false;
    switch (filter) {
      case AlertFilter::All:
        show = true;
        break;
      case AlertFilter::Warning:
        show = !e.is_recovery && e.severity == atm::AlertSeverity::Warning;
        break;
      case AlertFilter::Critical:
        show = !e.is_recovery && e.severity == atm::AlertSeverity::Critical;
        break;
      case AlertFilter::Recovery:
        show = e.is_recovery;
        break;
    }
    if (show) {
      matching.push_back(&e);
    }
  }
  if (matching.empty()) {
    out << "(none)\n";
  } else {
    for (const atm::AlertEvent *e : matching) {
      out << formatAlertTimestamp(e->timestamp) << "  "
          << (e->is_recovery ? std::string("\u2713 ")
                             : std::string(severitySymbol(e->severity)) + " ")
          << e->message << '\n';
    }
  }
  out << "Alert filter: press 'f' (then Enter) to cycle "
         "All / Warning / Critical / Recovery.\n";
}

/// Evaluates the AlertManager from the already-computed monitor snapshots.
/// Called once per refresh, immediately after history.update(). It never reads
/// /proc or /sys itself.
/// Builds the name/value pairs for GPU utilization and VRAM usage from a
/// GpuSnapshot, using NaN to mark an unavailable metric so the history layer
/// skips it.
void buildGpuMetricVectors(const atm::GpuSnapshot &gpu,
                           std::vector<std::pair<std::string, double>> &utils,
                           std::vector<std::pair<std::string, double>> &vrams) {
  utils.clear();
  vrams.clear();
  utils.reserve(gpu.devices.size());
  vrams.reserve(gpu.devices.size());
  for (const atm::GpuStats &stats : gpu.devices) {
    std::string name = stats.name;
    if (name.empty()) {
      name = stats.card;
    }
    utils.emplace_back(name,
                       stats.utilization_percent.has_value()
                           ? *stats.utilization_percent
                           : std::numeric_limits<double>::quiet_NaN());
    vrams.emplace_back(
        name, stats.memory_usage_percent.has_value()
                  ? *stats.memory_usage_percent
                  : std::numeric_limits<double>::quiet_NaN());
  }
}

/// Builds the name/value pairs for temperatures from a SensorSnapshot. Only
/// temperature sensors that expose a valid reading are included.
void buildTemperatureVector(const atm::SensorSnapshot &sensors,
                            std::vector<std::pair<std::string, double>> &temps) {
  temps.clear();
  temps.reserve(sensors.temperatures.size());
  for (const atm::TemperatureSensor &sensor : sensors.temperatures) {
    std::string label = atm::sensorTypeName(sensor.type);
    label += " " + sensor.label;
    if (!sensor.device.empty()) {
      label += " (" + sensor.device + ")";
    }
    temps.emplace_back(label, sensor.temperature_celsius);
  }
}

/// Evaluates the AlertManager from the already-computed monitor snapshots.
/// Called once per refresh, immediately after history.update(). It never reads
/// /proc or /sys itself.
void updateAlerts(atm::AlertManager &alerts, double cpu_usage,
                  const atm::MemoryInfo &memory,
                  const atm::DiskSnapshot &disk,
                  const atm::NetworkSnapshot &network,
                  const atm::GpuSnapshot &gpu,
                  const atm::SensorSnapshot &sensors,
                  const atm::HistoryManager &history) {
  // Smooth CPU with a short rolling average of recent history so a brief spike
  // does not instantly trip a Warning/Critical alert.
  double smoothed_cpu = cpu_usage;
  const auto &cpu_samples = history.cpuHistory().samples();
  if (!cpu_samples.empty()) {
    double sum = 0.0;
    std::size_t count = 0;
    for (auto it = cpu_samples.rbegin();
         it != cpu_samples.rend() && count < 5; ++it, ++count) {
      sum += it->value;
    }
    smoothed_cpu = count > 0 ? sum / static_cast<double>(count) : cpu_usage;
  }
  alerts.updateCpu(smoothed_cpu);

  alerts.updateMemory(memory.usagePercent(), memory.swapUsagePercent());

  for (const atm::DiskUsage &fs : disk.filesystems) {
    alerts.updateDiskUsage(fs.mount_point, fs.usage_percentage);
  }

  alerts.updateDiskActivity(
      static_cast<double>(disk.total_read_bytes_per_second),
      static_cast<double>(disk.total_write_bytes_per_second));

  alerts.updateNetwork(static_cast<double>(network.total_rx_bytes_per_second),
                       static_cast<double>(network.total_tx_bytes_per_second));

  std::vector<std::pair<std::string, double>> gpu_utils;
  std::vector<std::pair<std::string, double>> gpu_vrams;
  buildGpuMetricVectors(gpu, gpu_utils, gpu_vrams);
  for (std::size_t i = 0; i < gpu_utils.size(); ++i) {
    const std::string id =
        "GPU " + std::to_string(i) + " " + gpu_utils[i].first;
    alerts.updateGpu(id, gpu_utils[i].second, gpu_vrams[i].second);
  }

  for (const atm::TemperatureSensor &sensor : sensors.temperatures) {
    std::string label = atm::sensorTypeName(sensor.type);
    label += " " + sensor.label;
    if (!sensor.device.empty()) {
      label += " (" + sensor.device + ")";
    }
    const double hardware_critical =
        sensor.critical_temperature_celsius.has_value()
            ? *sensor.critical_temperature_celsius
            : -1.0;
    alerts.updateTemperature(label, sensor.temperature_celsius,
                             hardware_critical);
  }
}

/// Owned by main() and referenced by the notification sink; null by default so
/// the sink is inert until main() installs it.
atm::NotificationManager *g_notification_manager = nullptr;

/// Notification sink (see AlertManager::setNotificationSink). Forwards each
/// new alert event to the desktop notification manager, which applies its own
/// settings and cooldown and swallows any D-Bus failures.
void onAlertEvent(const atm::AlertManager & /*alerts*/,
                  const atm::AlertEvent &event) {
  if (g_notification_manager != nullptr) {
    g_notification_manager->notify(event);
  }
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

/// Filters the update list by a substring of the package name (case
/// insensitive). An empty search passes everything through.
std::vector<atm::PackageUpdate> filterPackageUpdates(
    const std::vector<atm::PackageUpdate> &updates,
    const std::string &search) {
  if (search.empty()) {
    return updates;
  }
  const std::string needle = toLowerAscii(search);
  std::vector<atm::PackageUpdate> result;
  for (const atm::PackageUpdate &update : updates) {
    if (toLowerAscii(update.name).find(needle) != std::string::npos) {
      result.push_back(update);
    }
  }
  return result;
}

/// Renders the update table (used by both the live section and the detail
/// screen). The live view caps the number of rows; the detail screen passes a
/// very large cap so the full filtered list is shown.
std::string renderPackageTableText(
    const std::vector<atm::PackageUpdate> &updates, std::size_t max_rows) {
  std::ostringstream out;
  out << std::left << std::setw(kPackageNameWidth) << "Package"
      << std::left << std::setw(kPackageVersionWidth) << "Installed"
      << std::left << std::setw(kPackageVersionWidth) << "Available"
      << std::left << std::setw(kPackageSrcWidth) << "Source"
      << "  Repo\n";
  if (updates.empty()) {
    out << "No updates available.\n";
    return out.str();
  }
  const std::size_t rows = std::min(max_rows, updates.size());
  for (std::size_t i = 0; i < rows; ++i) {
    const atm::PackageUpdate &u = updates[i];
    out << std::left << std::setw(kPackageNameWidth)
        << fitTo(u.name, kPackageNameWidth)
        << std::setw(kPackageVersionWidth)
        << fitTo(u.installed_version, kPackageVersionWidth)
        << std::setw(kPackageVersionWidth)
        << fitTo(u.available_version, kPackageVersionWidth)
        << std::setw(kPackageSrcWidth)
        << atm::packageSourceName(u.source)
        << "  " << fitTo(u.repository, 12) << '\n';
  }
  if (updates.size() > rows) {
    out << "... " << (updates.size() - rows)
        << " more (press 'p' then Enter to view all)\n";
  }
  return out.str();
}

/// Renders the PACKAGE UPDATES section of the live frame. Degrades gracefully
/// on unsupported distributions and when the databases are unavailable.
void renderPackageSections(std::ostringstream &out,
                           const atm::PackageManager &packages) {
  const atm::PackageUpdateSummary &summary = packages.summary();
  out << "\n## PACKAGE UPDATES\n\n";
  if (!summary.supported) {
    out << "Package update manager:\n"
        << fitTo(summary.error_message.empty()
                     ? "Not supported on this distribution"
                     : summary.error_message,
                 80)
        << "\n";
    return;
  }
  if (!summary.initialized || summary.refresh_failed) {
    out << "Unable to check for updates.\n\nReason:\n"
        << fitTo(summary.error_message.empty()
                     ? "Package database is unavailable."
                     : summary.error_message,
                 80)
        << "\n";
    return;
  }
  if (!summary.error_message.empty()) {
    // e.g. no sync databases configured: local info stays usable.
    out << fitTo(summary.error_message, 80) << "\n\n";
  }
  out << "Total Updates:     " << summary.total_updates << '\n'
      << "Official Repo:     " << summary.official_updates << '\n'
      << "AUR:               " << summary.aur_updates << '\n'
      << "Foreign Packages:  " << summary.foreign_packages << "\n\n";
  if (!summary.aur_helper.empty()) {
    out << "AUR helper detected: " << summary.aur_helper
        << " (not used for detection)\n";
  }
  out << (summary.total_updates > 0
              ? "⚠ " + std::to_string(summary.total_updates) +
                    " package updates available\n\n"
              : "System is up to date.\n\n")
      << renderPackageTableText(packages.updates(), kMaxPackageFrameRows)
      << "Package updates: press 'p' (then Enter) to manage\n";
}

/// Full per-package breakdown used by the package-detail screen.
std::string renderPackageDetails(const atm::PackageUpdate &update) {
  std::ostringstream out;
  out << update.name << "\n"
      << std::string(kPackageDetailRuleWidth, '-') << "\n";
  appendLabeled(out, "Name:", update.name);
  appendLabeled(out, "Installed:", update.installed_version);
  appendLabeled(out, "Available:", update.available_version);
  appendLabeled(out, "Repository:",
                update.repository.empty() ? "N/A" : update.repository);
  appendLabeled(out, "Source:", atm::packageSourceName(update.source));
  appendLabeled(out, "Architecture:",
                update.architecture.empty() ? "N/A" : update.architecture);
  appendLabeled(out, "Installed Size:",
                update.installed_size > 0
                    ? atm::formatBytes(update.installed_size)
                    : "N/A");
  appendLabeled(out, "Install Reason:",
                atm::packageInstallReasonName(update.install_reason));
  appendLabeled(out, "Update Available:", "Yes");
  if (!update.description.empty()) {
    appendLabeled(out, "Description:", update.description);
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

/// Renders a time_point as a "YYYY-MM-DD HH:MM:SS" local-time string.
std::string formatTimestamp(std::chrono::system_clock::time_point timestamp) {
  const std::time_t time = std::chrono::system_clock::to_time_t(timestamp);
  std::tm local{};
  if (::localtime_r(&time, &local) == nullptr) {
    return "N/A";
  }
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local);
  return buffer;
}

/// Formats a duration in seconds as "Xd Xh Xm Xs" (omitting empty leading
/// units).
std::string formatDuration(std::uint64_t seconds) {
  if (seconds == 0) {
    return "0s";
  }
  const std::uint64_t days = seconds / 86400;
  const std::uint64_t hours = (seconds % 86400) / 3600;
  const std::uint64_t minutes = (seconds % 3600) / 60;
  const std::uint64_t secs = seconds % 60;
  std::ostringstream out;
  if (days > 0) {
    out << days << "d ";
  }
  if (hours > 0) {
    out << hours << "h ";
  }
  if (minutes > 0) {
    out << minutes << "m ";
  }
  out << secs << "s";
  return out.str();
}

/// Human-readable label for the <value> or the `<optional>` value, showing
/// "N/A" when absent.
std::string orNa(const std::string &value) {
  return value.empty() ? std::string("N/A") : value;
}

/// Renders the full detailed breakdown for one process (Step 13). Every
/// field degrades to "N/A" when it could not be read; nothing here re-reads
/// /proc — the data was already collected by ProcessDetails.
std::string renderProcessDetails(const atm::ProcessDetailsInfo &info) {
  std::ostringstream out;
  out << "Process Details\n"
         "────────────────────────────────\n\n";
  appendLabeled(out, "Name:", info.name);
  appendLabeled(out, "PID:", std::to_string(info.pid));
  appendLabeled(out, "State:", atm::processStateName(info.state));
  appendLabeled(out, "User:", orNa(info.user));
  appendLabeled(out, "UID:", info.uid.has_value()
                               ? std::to_string(*info.uid)
                               : std::string("N/A"));
  appendLabeled(out, "GID:", info.gid.has_value()
                               ? std::to_string(*info.gid)
                               : std::string("N/A"));
  appendLabeled(out, "Parent PID:", std::to_string(info.parent_pid));
  appendLabeled(out, "Threads:", info.thread_count.has_value()
                                     ? std::to_string(*info.thread_count)
                                     : std::string("N/A"));
  appendLabeled(out, "Priority:", info.priority.has_value()
                                      ? std::to_string(*info.priority)
                                      : std::string("N/A"));
  if (info.nice_priority.has_value()) {
    appendLabeled(out, "Nice:", std::to_string(*info.nice_priority));
  } else {
    appendLabeled(out, "Nice:", info.nice_value.has_value()
                                    ? std::to_string(*info.nice_value)
                                    : std::string("N/A"));
  }
  if (info.start_time.has_value()) {
    appendLabeled(out, "Started:", formatTimestamp(*info.start_time));
  } else {
    appendLabeled(out, "Started:", "N/A");
  }
  if (info.process_uptime_seconds.has_value()) {
    appendLabeled(out, "Running For:",
                  formatDuration(*info.process_uptime_seconds));
  } else {
    appendLabeled(out, "Running For:", "N/A");
  }

  out << "\nExecutable:\n" << orNa(info.executable_path) << "\n\n"
      << "Working Directory:\n" << orNa(info.working_directory) << "\n\n"
      << "Command Line:\n"
      << (info.command_line.empty() ? std::string("N/A")
                                    : info.command_line)
      << "\n";

  out << "\nState code: " << info.state_char << " ("
      << atm::processStateName(info.state) << ")\n";

  out << "\n## Memory\n\n";
  appendLabeled(out, "Virtual:", atm::formatBytes(info.virtual_memory_bytes));
  appendLabeled(out, "Resident:", atm::formatBytes(info.resident_memory_bytes));
  appendLabeled(out, "Shared:",
                info.shared_memory_bytes.has_value()
                    ? atm::formatBytes(*info.shared_memory_bytes)
                    : std::string("N/A"));
  appendLabeled(out, "Text:",
                info.text_memory_bytes.has_value()
                    ? atm::formatBytes(*info.text_memory_bytes)
                    : std::string("N/A"));
  appendLabeled(out, "Data:",
                info.data_memory_bytes.has_value()
                    ? atm::formatBytes(*info.data_memory_bytes)
                    : std::string("N/A"));
  appendLabeled(out, "Stack:",
                info.stack_memory_bytes.has_value()
                    ? atm::formatBytes(*info.stack_memory_bytes)
                    : std::string("N/A"));
  appendLabeled(out, "Memory %:",
                info.memory_percent.has_value()
                    ? formatPercent(*info.memory_percent) + "%"
                    : std::string("N/A"));

  out << "\n## CPU\n\n";
  appendLabeled(out, "User time:",
                formatDuration(info.user_cpu_time /
                               static_cast<std::uint64_t>(
                                   std::max(1L, ::sysconf(_SC_CLK_TCK)))));
  appendLabeled(out, "System time:",
                formatDuration(info.system_cpu_time /
                               static_cast<std::uint64_t>(
                                   std::max(1L, ::sysconf(_SC_CLK_TCK)))));
  appendLabeled(out, "CPU %:",
                info.cpu_usage_percent.has_value()
                    ? formatPercent(*info.cpu_usage_percent) + "%"
                    : std::string("N/A"));
  appendLabeled(out, "Threads:", info.thread_count.has_value()
                                     ? std::to_string(*info.thread_count)
                                     : std::string("N/A"));

  out << "\n## Context Switches\n\n";
  appendLabeled(out, "Voluntary:",
                info.voluntary_context_switches.has_value()
                    ? formatThousands(*info.voluntary_context_switches)
                    : std::string("N/A"));
  appendLabeled(out, "Non-voluntary:",
                info.nonvoluntary_context_switches.has_value()
                    ? formatThousands(*info.nonvoluntary_context_switches)
                    : std::string("N/A"));

  out << "\n## I/O Statistics\n\n";
  appendLabeled(out, "Read:",
                info.read_bytes.has_value()
                    ? atm::formatBytes(*info.read_bytes)
                    : std::string("N/A"));
  appendLabeled(out, "Written:",
                info.write_bytes.has_value()
                    ? atm::formatBytes(*info.write_bytes)
                    : std::string("N/A"));
  appendLabeled(out, "Read Calls:",
                info.read_syscalls.has_value()
                    ? formatThousands(*info.read_syscalls)
                    : std::string("N/A"));
  appendLabeled(out, "Write Calls:",
                info.write_syscalls.has_value()
                    ? formatThousands(*info.write_syscalls)
                    : std::string("N/A"));
  appendLabeled(out, "Cancelled Write:",
                info.cancelled_write_bytes.has_value()
                    ? atm::formatBytes(*info.cancelled_write_bytes)
                    : std::string("N/A"));

  return out.str();
}

/// Renders the process-tree view (banner + summary + tree + tree stats +
/// footer). Each frame is a self-contained 1 s snapshot.
std::string renderTreeFrame(double cpu_usage, const atm::MemoryInfo &memory,
                            const atm::ProcessTree &tree,
                            int refresh_interval_ms) {
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
      << "Updating every " << formatRefreshInterval(refresh_interval_ms)
      << "...\n";
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
                         const atm::SystemInfo &sysinfo,
                         const atm::HistoryManager &history,
                         bool show_history,
                         const atm::AlertManager &alerts,
                         AlertFilter alert_filter,
                         const std::string &service_search,
                         atm::ServiceSort service_sort,
                         const std::string &startup_search,
                         atm::StartupSort startup_sort,
                         const atm::PackageManager &packages,
                         int refresh_interval_ms) {
  if (view == ViewMode::Tree) {
    // The tree view stays deliberately focused on the hierarchy; the storage
    // and network sections are part of the table view.
    return renderTreeFrame(cpu_usage, memory, tree, refresh_interval_ms);
  }

  std::ostringstream out;
  renderHeader(out, cpu_usage, memory);
  renderSystemInfoSections(out, sysinfo);
  renderMemorySections(out, memory);
  if (show_history) {
    renderResourceHistorySection(out, history);
  }
  renderStorageSections(out, disk);
  renderNetworkSections(out, network);
  renderGpuSections(out, gpu);
  renderSensorSections(out, sensors, gpu);
  renderAlertsSections(out, alerts, alert_filter);
  renderSystemdSections(out, systemd, service_search, service_sort);
  renderStartupSections(out, startup, startup_search, startup_sort);
  renderPackageSections(out, packages);
  renderProcessTable(out, processes);
  renderProcessStats(out, stats);
  out << "\n---\n\n"
         "Processes: "
      << processes.size() << "\n"
      << "Sort: [1] CPU  [2] Memory  [3] PID  [4] Name"
         " (current: "
      << atm::processSortName(sort) << ")\n"
      << "View: [l] Process List  [t] Process Tree (current: List)\n"
      << "Resource history: press 'r' (then Enter) to toggle graphs (current: "
      << (show_history ? "shown" : "hidden") << ")\n"
      << "Alert filter: press 'f' (then Enter) to cycle recent alerts (current: "
      << alertFilterName(alert_filter) << ")\n"
      << "Manage: press 'm' (then Enter) to control a process by PID\n"
      << "Details: press 'd' (then Enter) to inspect a process in detail\n"
      << "Network detail: press 'i' (then Enter) to inspect an interface\n"
      << "GPU detail: press 'g' (then Enter) to inspect a GPU\n"
      << "Sensor detail: press 's' (then Enter) to inspect a sensor\n"
<< "Systemd services: press 'u' (then Enter) to manage services\n"
       << "Startup apps: press 'a' (then Enter) to manage autostart\n"
       << "System info: press 'y' (then Enter) for the hardware overview\n"
       << "Package updates: press 'p' (then Enter) to manage updates\n"
       << "Settings: press 'o' (then Enter) to change configuration\n"
       << "Updating every " << formatRefreshInterval(refresh_interval_ms)
       << "...\n";
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
                 const atm::SystemInfo &sysinfo,
                 const atm::HistoryManager &history,
                 bool show_history,
                 const atm::AlertManager &alerts,
                 AlertFilter alert_filter,
                 const std::string &service_search,
                 atm::ServiceSort service_sort,
                 const std::string &startup_search,
                 atm::StartupSort startup_sort,
                 const atm::PackageManager &packages,
                 int refresh_interval_ms) {
  // ANSI "clear entire screen" + "cursor to home" so the multi-line frame
  // refreshes in place instead of scrolling the terminal.
  std::cout << "\033[2J\033[H";
  std::cout << renderFrame(cpu_usage, memory, processes, stats, sort, view, tree,
                           disk, network, gpu, sensors, systemd, startup,
                           sysinfo, history, show_history, alerts, alert_filter,
                           service_search, service_sort, startup_search,
                           startup_sort, packages, refresh_interval_ms)
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
/// enters process-control mode, 'i'/'g'/'s' open the network/GPU/sensor
/// detail screens, 'u'/'a' open systemd/startup management and 'y' opens the
/// system-information overview; anything else is ignored. Because the live
/// loop must not lose bytes meant for the (blocking) control prompts, all
/// input funnels through an internal buffer shared with readLine().
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
    InspectProcess,
    InspectNetwork,
    InspectGpu,
    InspectSensors,
    InspectSystemd,
    InspectStartup,
    InspectSystemInfo,
    InspectPackages,
    InspectSettings,
    ToggleHistory,
    ToggleAlertFilter,
    ToggleNotifications,
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
    if (token == "d" || token == "D") return Command::InspectProcess;
    if (token == "i" || token == "I") return Command::InspectNetwork;
    if (token == "g" || token == "G") return Command::InspectGpu;
    if (token == "s" || token == "S") return Command::InspectSensors;
    if (token == "u" || token == "U") return Command::InspectSystemd;
    if (token == "a" || token == "A") return Command::InspectStartup;
    if (token == "y" || token == "Y") return Command::InspectSystemInfo;
    if (token == "p" || token == "P") return Command::InspectPackages;
    if (token == "o" || token == "O") return Command::InspectSettings;
    if (token == "r" || token == "R") return Command::ToggleHistory;
    if (token == "f" || token == "F") return Command::ToggleAlertFilter;
    if (token == "n" || token == "N") return Command::ToggleNotifications;
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

/// Asks for a PID to inspect, validating it only against the current process
/// list. Unlike selectPid() this deliberately permits PID 1 and the Task
/// Manager's own PID, because details (unlike control) are safe to view.
/// Returns std::nullopt when the user cancels or the PID is invalid/gone.
std::optional<SelectedProcess> selectPidForDetails(
    ConsoleInput &input, const std::vector<atm::Process> &listed,
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

  for (const atm::Process &process : listed) {
    if (process.pid == pid) {
      return SelectedProcess{pid, process.name};
    }
  }
  std::cout << "Process does not exist (not found in the current process "
               "list).\n";
  return std::nullopt;
}

/// Runs one of the control actions from within the details view. It reuses the
/// existing ProcessActions wrappers — no signal logic is re-implemented here —
/// and gates PID 1 / the monitor's own PID behind isProtectedPid().
void runProcessDetailAction(atm::ProcessActions &actions, ConsoleInput &input,
                            int pid, const std::string &name, int action) {
  if (atm::isProtectedPid(pid)) {
    std::cout << "PID " << pid
              << " is protected by the application and cannot be controlled "
                 "from this interface.\n";
    return;
  }

  std::string prompt;
  const char *verb = "operate on";
  switch (action) {
    case 2:
      prompt = "Terminate process " + std::to_string(pid) + "?";
      verb = "terminate";
      break;
    case 3:
      prompt = "\nWARNING:\nYou are about to forcefully kill process:\n\n"
               "PID: " + std::to_string(pid) + "\nName: " + name +
               "\n\nContinue?";
      verb = "kill";
      break;
    case 4:
      prompt = "Pause process " + std::to_string(pid) + "?";
      verb = "pause";
      break;
    case 5:
      prompt = "Resume process " + std::to_string(pid) + "?";
      verb = "resume";
      break;
    default:
      return;
  }

  if (!confirm(prompt, input)) {
    std::cout << "Cancelled.\n";
    return;
  }

  atm::ActionResult result;
  switch (action) {
    case 2:
      result = actions.terminate(pid);
      break;
    case 3:
      result = actions.kill(pid);
      break;
    case 4:
      result = actions.pause(pid);
      break;
    case 5:
      result = actions.resume(pid);
      break;
  }

  if (result.success()) {
    std::cout << "Process " << pid << " " << verb << " request sent.\n";
  } else {
    printActionFailure(verb, pid, result);
  }
}

/// "d": interactively inspect one process in detail. The process list is shown
/// frozen; a PID is chosen and its /proc/<pid> entries are parsed by
/// ProcessDetails — this UI never reads /proc directly. The screen loops so
/// the user can refresh (re-inspect the same PID), apply an existing process
/// action, or go back to the live view.
void interactProcessDetail(atm::ProcessDetails &details,
                           atm::ProcessActions &actions, ConsoleInput &input,
                           const std::vector<atm::Process> &listed,
                           atm::ProcessSort sort,
                           std::uint64_t system_total_kib) {
  showProcessSelection(listed, sort);
  const auto selected =
      selectPidForDetails(input, listed, "Select PID to inspect (blank to cancel)");
  if (!selected.has_value()) {
    return;
  }
  const int pid = selected->pid;
  const std::string name = selected->name;

  for (;;) {
    // Reuse the Process Monitor's CPU figure for this PID (the snapshot is at
    // most ~1 s old) rather than building a second CPU tracker.
    std::optional<double> cpu_percent;
    for (const atm::Process &process : listed) {
      if (process.pid == pid) {
        cpu_percent = process.cpu_percent;
        break;
      }
    }

    const std::optional<atm::ProcessDetailsInfo> info =
        details.getProcessDetails(pid, system_total_kib, cpu_percent);

    if (!info.has_value()) {
      std::cout << "\nProcess no longer exists.\n"
                << "\nPress Enter to return to the process list.\n"
                << std::flush;
      static_cast<void>(input.readLine());
      return;
    }

    std::cout << "\033[2J\033[H";
    std::cout << "========================================\n"
                 "ARCH TASK MANAGER — Process Details\n"
                 "========================================\n\n"
              << renderProcessDetails(*info) << "\n\n"
              << "[1] Refresh\n"
                 "[2] Terminate\n"
                 "[3] Kill\n"
                 "[4] Stop (Pause)\n"
                 "[5] Continue (Resume)\n"
                 "[0] Back\n\n"
                 "Select action:\n> "
              << std::flush;

    const std::optional<std::string> action_line = input.readLine();
    if (!action_line) {
      std::cout << "\nInput cancelled.\n";
      return;
    }
    const std::string action_text = trimWhitespace(*action_line);
    if (action_text == "0" || action_text.empty()) {
      return;  // Back to the live view
    }
    if (action_text == "1") {
      continue;  // refresh: re-inspect the same PID on the next loop
    }
    if (action_text == "2" || action_text == "3" || action_text == "4" ||
        action_text == "5") {
      const int action = std::atoi(action_text.c_str());
      runProcessDetailAction(actions, input, pid, name, action);
      continue;  // redraw details after the action
    }
    std::cout << "Invalid action.\n";
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

/// "y": full static system/hardware overview (OS, kernel, CPU, memory/swap
/// capacity, GPU, DMI hardware and firmware). The cached SystemInfoProvider
/// snapshot is shown frozen; action [1] re-reads all static information on
/// demand so the DMI files, /etc/os-release and /proc/cpuinfo are never
/// rescanned continuously (Step 12).
void interactSystemInfoDetail(atm::SystemInfoProvider &provider,
                              atm::SystemInfo &info,
                              const atm::GpuSnapshot &gpu,
                              ConsoleInput &input) {
  for (;;) {
    std::cout << "\033[2J\033[H";
    std::cout << "========================================\n"
                 "ARCH TASK MANAGER — System Information\n"
                 "========================================\n\n"
              << renderSystemInfoDetail(info) << "\n\n"
              << "[1] Refresh System Information\n"
                 "[0] Cancel\n\n"
                 "Select action:\n> "
              << std::flush;
    const std::optional<std::string> action_line = input.readLine();
    if (!action_line) {
      std::cout << "\nInput cancelled.\n";
      return;
    }
    const std::string action_text = trimWhitespace(*action_line);
    if (action_text == "1") {
      provider.load(gpu);
      info = provider.read();
      std::cout << "\nSystem information refreshed.\n" << std::flush;
      continue;  // redraw with the fresh snapshot
    }
    if (!action_text.empty() && action_text != "0") {
      std::cout << "\nInvalid action.\n";
    }
    std::cout << "\nPress Enter to return to the live view.\n" << std::flush;
    static_cast<void>(input.readLine());
    return;
  }
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

/// An internal progress formatter for the commit phase: renders a simple ASCII
/// progress bar from 0-100, using "Working..." when no precise value exists.
std::string progressBar(int percent, std::size_t width) {
  if (percent < 0 || percent > 100) {
    return "Working...";
  }
  const std::size_t filled =
      static_cast<std::size_t>(percent * static_cast<int>(width) / 100);
  const std::size_t empty = width - filled;
  return std::string(filled, '#') + std::string(empty, '.') + " " +
         std::to_string(percent) + "%";
}

/// Renders the computed upgrade preview plus the pending modifications so the
/// user can review before confirming.
void renderTransactionPreview(const atm::TransactionPreview &preview) {
  std::cout << "Upgrade Preview\n"
               "────────────────────────────\n\n";
  std::cout << "Upgrade:       " << preview.to_upgrade << '\n'
            << "Install:       " << preview.to_install << '\n'
            << "Remove:        " << preview.to_remove << '\n'
            << "Reinstall:     " << preview.to_reinstall << '\n';
  if (preview.download_size > 0) {
    std::cout << "Download:      "
              << atm::formatBytes(preview.download_size) << '\n';
  }
  std::cout << "Size change:   ";
  if (preview.size_change >= 0) {
    std::cout << '+' << atm::formatBytes(
        static_cast<std::uint64_t>(preview.size_change));
  } else {
    std::cout << '-' << atm::formatBytes(
        static_cast<std::uint64_t>(-preview.size_change));
  }
  std::cout << "\n\n";

  std::cout << std::left << std::setw(kPackageNameWidth) << "Package"
            << std::setw(kPackageVersionWidth) << "Old Version"
            << std::setw(kPackageVersionWidth) << "New Version"
            << "  Source\n";
  for (const atm::TransactionPackage &p : preview.packages) {
    std::cout << std::left << std::setw(kPackageNameWidth)
              << fitTo(p.name, kPackageNameWidth);
    if (p.will_remove) {
      std::cout << std::setw(kPackageVersionWidth)
                << fitTo(p.old_version, kPackageVersionWidth)
                << std::setw(kPackageVersionWidth) << "REMOVE"
                << "  " << fitTo(p.repository, 12) << '\n';
    } else {
      std::cout << std::setw(kPackageVersionWidth)
                << fitTo(p.old_version, kPackageVersionWidth)
                << std::setw(kPackageVersionWidth)
                << fitTo(p.new_version, kPackageVersionWidth)
                << "  " << fitTo(p.repository, 12) << '\n';
    }
  }

  if (preview.has_removals) {
    std::cout << "\n⚠ Transaction includes package removals.\n\n"
                 "Review the following packages:\n\n";
    for (const atm::TransactionPackage &p : preview.packages) {
      if (p.will_remove) {
        std::cout << "- " << p.name << '\n';
      }
    }
    std::cout << "\nContinue?\n";
  }
}

/// Reads a confirmation line. Returns true only for y/Y/yes.
bool confirmPrompt(const std::string &prompt, ConsoleInput &input) {
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

/// Drives the full, safe upgrade workflow: sync databases -> resolve -> show
/// preview -> explicit confirmation -> execute -> refresh package info.
/// Never auto-upgrades; the user must confirm at every destructive step.
void runSystemUpgrade(atm::PackageTransaction &transaction,
                      atm::PackageManager &packages, ConsoleInput &input) {
  if (transaction.is_running()) {
    std::cout << "A package operation is already in progress.\n";
    return;
  }

  // Step 1: explicit database synchronisation (may touch the network).
  std::cout << "\nRefreshing package databases...\n"
            << "This may download updated repository metadata.\n";
  if (!transaction.syncDatabases()) {
    std::cout << "\nUpdate check failed.\n\nReason:\n"
              << transaction.error() << '\n';
    return;
  }

  // Step 2: resolve the full-system-upgrade target list.
  std::cout << "Calculating updates...\n";
  if (!transaction.resolve()) {
    std::cout << "\nUnable to calculate updates.\n\nReason:\n"
              << transaction.error() << '\n';
    return;
  }

  const atm::TransactionPreview &preview = transaction.preview();
  if (preview.to_upgrade == 0 && preview.to_install == 0 &&
      preview.to_remove == 0) {
    std::cout << "No updates available. The system is up to date.\n";
    transaction.cancel();
    return;
  }

  // Step 3: explicit confirmation (never implied by opening the page).
  std::cout << "\nYou are about to upgrade " << preview.to_upgrade
            << " package(s) and install " << preview.to_install
            << " and remove " << preview.to_remove << ".\n"
            << "This operation will modify the system package database and "
               "installed software.\n\n";
  if (!confirmPrompt("Continue?", input)) {
    std::cout << "Cancelled. No packages were modified.\n";
    transaction.cancel();
    return;
  }

  // If the transaction includes removals, show an additional warning. This is
  // informational only - legitimate Arch upgrades can involve replacements.
  if (preview.has_removals) {
    std::cout << "\n⚠ Transaction includes package removals.\n"
                 "Review the following packages:\n\n";
    for (const atm::TransactionPackage &p : preview.packages) {
      if (p.will_remove) {
        std::cout << "- " << p.name << '\n';
      }
    }
    std::cout << '\n';
    if (!confirmPrompt("Continue anyway?", input)) {
      std::cout << "Cancelled. No packages were modified.\n";
      transaction.cancel();
      return;
    }
  }

  // Show the final preview for review.
  std::cout << "\n";
  renderTransactionPreview(preview);
  std::cout << '\n';
  if (!confirmPrompt("Proceed?", input)) {
    std::cout << "Cancelled. No packages were modified.\n";
    transaction.cancel();
    return;
  }

  // Step 4: execute. Note: on Arch this normally requires root, which the
  // application does not auto-elevate; the transaction reports that clearly.
  std::cout << "\nUpdating System\n"
               "────────────────────────────\n\n"
            << "Resolving dependencies...\n"
            << progressBar(-1, 20) << '\n' << std::flush;
  atm::TransactionProgress progress;
  transaction.commit(&progress);

  if (transaction.state() == atm::TransactionState::Completed) {
    std::cout << "\n✓ System update completed successfully.\n";
    const std::vector<std::string> names = [&]() {
      std::vector<std::string> n;
      n.reserve(preview.packages.size());
      for (const atm::TransactionPackage &p : preview.packages) {
        n.push_back(p.name);
      }
      return n;
    }();
    if (transaction.includesRebootPackage(names)) {
      std::cout << "\nA reboot may be recommended after updates to:\n"
                   "- Linux kernel\n"
                   "- system libraries\n"
                   "- systemd\n\n"
                   "This is informational only; the application does not "
                   "reboot automatically.\n";
    }
  } else if (transaction.state() == atm::TransactionState::Failed) {
    std::cout << "\n⚠ System update failed.\n\nReason:\n"
              << transaction.error() << '\n';
  } else {
    std::cout << "\nTransaction ended: "
              << atm::transactionStateName(transaction.state()) << '\n';
  }

  // Step 5: refresh package information so the UI reflects the new state.
  packages.refresh();
  std::cout << "\nUpdates Available: " << packages.summary().total_updates
            << '\n';
}

/// Full-screen package update management. Accessible via 'p' (then Enter):
/// shows the update summary and update table, and offers an explicit Refresh,
/// a package-details view and a search/filter over the already-fetched
/// updates. Also provides the safe full-system-upgrade workflow.
void interactPackageDetail(atm::PackageManager &packages,
                           atm::PackageTransaction &transaction,
                           ConsoleInput &input, std::string &package_search) {
  const atm::PackageUpdateSummary &summary = packages.summary();
  std::vector<atm::PackageUpdate> filtered =
      filterPackageUpdates(packages.updates(), package_search);

  std::cout << "\033[2J\033[H";
  std::cout << "========================================\n"
               "ARCH TASK MANAGER — Package Updates\n"
               "========================================\n\n";

  if (!summary.supported) {
    std::cout << "Package update manager:\n"
                 "Not supported on this distribution\n";
    return;
  }
  if (!summary.initialized || summary.refresh_failed) {
    std::cout << "Unable to check for updates.\n\nReason:\n"
              << (summary.error_message.empty()
                      ? "Package database is unavailable."
                      : summary.error_message)
              << '\n';
    return;
  }
  if (!summary.error_message.empty()) {
    std::cout << summary.error_message << "\n\n";
  }

  std::cout << "Total Updates:     " << summary.total_updates << '\n'
            << "Official Repo:     " << summary.official_updates << '\n'
            << "AUR:               " << summary.aur_updates << '\n'
            << "Foreign Packages:  " << summary.foreign_packages << "\n\n";
  if (!summary.aur_helper.empty()) {
    std::cout << "AUR helper detected: " << summary.aur_helper
              << " (not used for detection)\n";
  }
  std::cout << renderPackageTableText(filtered, std::numeric_limits<std::size_t>::max());
  if (!package_search.empty()) {
    std::cout << "Showing " << filtered.size() << " of "
              << packages.updates().size()
              << " updates (search: \"" << package_search << "\")\n";
  }
  std::cout << "\nAutomatic package installation is disabled. Updates always "
               "require explicit\nuser confirmation.\n";
  std::cout << "\nManagement:\n"
               "[1] Refresh (local check)\n"
               "[2] View Package Details\n"
               "[3] Search/Filter Updates\n"
               "[4] Full System Upgrade\n"
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
      action_text != "4") {
    std::cout << "Invalid action.\n";
    return;
  }

  if (action_text == "1") {
    std::cout << "\nRefreshing package databases...\n"
              << "This reads the local package database and sync metadata only; "
                 "no network access or database modification occurs.\n";
    if (packages.refresh()) {
      std::cout << "Refresh complete. " << packages.summary().total_updates
                << " update(s) available.\n";
    } else {
      std::cout << "Refresh failed:\n"
                << packages.summary().error_message << '\n';
    }
    std::this_thread::sleep_for(300ms);
    return;
  }

  if (action_text == "3") {
    std::cout << "\nSearch updates (package name; blank to clear):\n> "
              << std::flush;
    const std::optional<std::string> search_line = input.readLine();
    if (!search_line) {
      std::cout << "\nInput cancelled.\n";
      return;
    }
    package_search = trimWhitespace(*search_line);
    if (package_search.empty()) {
      std::cout << "Search cleared.\n";
    } else {
      std::cout << "Filtering updates by: \"" << package_search << "\"\n";
    }
    std::this_thread::sleep_for(300ms);
    return;
  }

  if (action_text == "4") {
    runSystemUpgrade(transaction, packages, input);
    std::this_thread::sleep_for(600ms);
    return;
  }

  // Action 2: package details.
  std::cout << "\nEnter a package name from the update list:\n> " << std::flush;
  const std::optional<std::string> name_line = input.readLine();
  if (!name_line) {
    std::cout << "\nInput cancelled.\n";
    return;
  }
  const std::string name = trimWhitespace(*name_line);
  if (name.empty()) {
    std::cout << "Cancelled.\n";
    return;
  }
  const atm::PackageUpdate *update = packages.findUpdate(name);
  if (update == nullptr) {
    std::cout << "No update found for package: " << name << "\n";
    return;
  }
  std::cout << '\n' << renderPackageDetails(*update)
            << "\n\nPress Enter to return to the live view.\n"
            << std::flush;
  static_cast<void>(input.readLine());
}

/// "yes"/"no" for display.
std::string yesNo(bool value) { return value ? "yes" : "no"; }

/// Prompts for a yes/no answer to `label`. Returns true when the user provided
/// a new value (stored in `out`); false when unchanged or cancelled.
bool promptBool(ConsoleInput &input, const std::string &label, bool current,
                bool &out) {
  std::cout << label << " [y/n, current: " << yesNo(current) << "]: "
            << std::flush;
  const std::optional<std::string> line = input.readLine();
  if (!line) {
    return false;
  }
  const std::string answer = trimWhitespace(*line);
  if (answer.empty()) {
    return false;
  }
  if (answer == "y" || answer == "Y" || answer == "yes" ||
      answer == "Yes") {
    out = true;
    return true;
  }
  if (answer == "n" || answer == "N" || answer == "no" ||
      answer == "No") {
    out = false;
    return true;
  }
  std::cout << "Invalid answer. ";
  return promptBool(input, label, current, out);
}

/// Prompts for an integer in [lo, hi]. Empty input or EOF keeps the current
/// value. Invalid values are rejected, never silently accepted.
bool promptIntRange(ConsoleInput &input, const std::string &label, int current,
                    int lo, int hi, int &out) {
  for (;;) {
    std::cout << label << " [" << lo << "-" << hi
              << ", current: " << current << "]: " << std::flush;
    const std::optional<std::string> line = input.readLine();
    if (!line) {
      return false;
    }
    const std::string answer = trimWhitespace(*line);
    if (answer.empty()) {
      return false;
    }
    char *end = nullptr;
    errno = 0;
    const long value = std::strtol(answer.c_str(), &end, 10);
    if (errno == 0 && end != nullptr && *end == '\0' && value >= lo &&
        value <= hi) {
      out = static_cast<int>(value);
      return true;
    }
    std::cout << "Invalid value (expected an integer in [" << lo << "-" << hi
              << "]).\n";
  }
}

/// Prompts for a double in [lo, hi]. Empty input or EOF keeps the current
/// value. Invalid values are rejected, never silently accepted.
bool promptDoubleRange(ConsoleInput &input, const std::string &label,
                       double current, double lo, double hi, double &out) {
  for (;;) {
    std::cout << label << " [" << lo << "-" << hi
              << ", current: " << current << "]: " << std::flush;
    const std::optional<std::string> line = input.readLine();
    if (!line) {
      return false;
    }
    const std::string answer = trimWhitespace(*line);
    if (answer.empty()) {
      return false;
    }
    char *end = nullptr;
    errno = 0;
    const double value = std::strtod(answer.c_str(), &end);
    if (errno == 0 && end != nullptr && *end == '\0' && value >= lo &&
        value <= hi) {
      out = value;
      return true;
    }
    std::cout << "Invalid value (expected a number in [" << lo << "-" << hi
              << "]).\n";
  }
}

/// Prints one alert category's current values.
void printAlertCategory(const char *name,
                        const atm::cfg::AlertCategorySettings &cat) {
  std::cout << "  " << name << ": "
            << (cat.enabled ? "enabled" : "disabled")
            << " (warning " << cat.warning << ", critical " << cat.critical
            << ")\n";
}

/// Edits one alert category in-place, rejecting warning >= critical.
void editAlertCategory(atm::cfg::AlertCategorySettings &cat,
                       const std::string &name, double warn_max,
                       double crit_max, ConsoleInput &input) {
  std::cout << "\n--- " << name << " ---\n";
  bool enabled = cat.enabled;
  if (promptBool(input, "Alerts enabled", enabled, enabled)) {
    cat.enabled = enabled;
  }
  double warning = cat.warning;
  if (promptDoubleRange(input, "Warning threshold", warning, 0.0, warn_max,
                        warning)) {
    if (warning >= cat.critical) {
      std::cout << "Warning must be lower than the critical threshold ("
                << cat.critical << "). Not changed.\n";
    } else {
      cat.warning = warning;
    }
  }
  double critical = cat.critical;
  if (promptDoubleRange(input, "Critical threshold", critical, 0.0, crit_max,
                        critical)) {
    if (cat.warning >= critical) {
      std::cout << "Critical must be higher than the warning threshold ("
                << cat.warning << "). Not changed.\n";
    } else {
      cat.critical = critical;
    }
  }
}

void editGeneral(atm::cfg::SettingsManager &settings, ConsoleInput &input) {
  atm::cfg::AppSettings next = settings.settings();
  std::cout << "\n--- General ---\n";
  int refresh = next.general.refresh_interval_ms;
  if (promptIntRange(input, "Refresh interval (ms)", refresh,
                     atm::cfg::kMinRefreshIntervalMs,
                     atm::cfg::kMaxRefreshIntervalMs, refresh)) {
    next.general.refresh_interval_ms = refresh;
  }
  std::cout << "Default page [1] Process List [2] Process Tree"
            << " (current: " << next.general.default_page << "): "
            << std::flush;
  const std::optional<std::string> page_line = input.readLine();
  if (page_line) {
    const std::string choice = trimWhitespace(*page_line);
    if (choice == "2" || choice == "tree" || choice == "Tree") {
      next.general.default_page = "tree";
    } else if (choice == "1" || choice == "list" || choice == "List") {
      next.general.default_page = "list";
    }
  }
  settings.updateSettings(next);
}

void editHistory(atm::cfg::SettingsManager &settings, ConsoleInput &input) {
  atm::cfg::AppSettings next = settings.settings();
  std::cout << "\n--- History ---\n";
  int duration = next.history.history_duration_seconds;
  if (promptIntRange(input, "History duration (seconds)", duration,
                     atm::cfg::kMinHistoryDurationSeconds,
                     atm::cfg::kMaxHistoryDurationSeconds, duration)) {
    next.history.history_duration_seconds = duration;
  }
  int max_samples = next.history.max_samples;
  if (promptIntRange(input, "Maximum samples (bounded)", max_samples,
                     atm::cfg::kMinHistorySamples, atm::cfg::kMaxHistorySamples,
                     max_samples)) {
    next.history.max_samples = max_samples;
  }
  int interval = next.history.sample_interval_ms;
  if (promptIntRange(input, "Sampling interval (ms, informational)", interval,
                     atm::cfg::kMinSampleIntervalMs,
                     atm::cfg::kMaxSampleIntervalMs, interval)) {
    next.history.sample_interval_ms = interval;
  }
  settings.updateSettings(next);
}

void editAlerts(atm::cfg::SettingsManager &settings, ConsoleInput &input) {
  atm::cfg::AppSettings next = settings.settings();
  std::cout << "\n--- Alerts ---\n";
  editAlertCategory(next.alerts.cpu, "CPU usage",
                    atm::cfg::kMaxPercentThreshold,
                    atm::cfg::kMaxPercentThreshold, input);
  editAlertCategory(next.alerts.memory, "Memory usage",
                    atm::cfg::kMaxPercentThreshold,
                    atm::cfg::kMaxPercentThreshold, input);
  editAlertCategory(next.alerts.swap, "Swap usage",
                    atm::cfg::kMaxPercentThreshold,
                    atm::cfg::kMaxPercentThreshold, input);
  editAlertCategory(next.alerts.disk, "Disk capacity",
                    atm::cfg::kMaxPercentThreshold,
                    atm::cfg::kMaxPercentThreshold, input);
  editAlertCategory(next.alerts.temperature, "Temperature (Celsius)",
                    atm::cfg::kMaxTemperatureWarning,
                    atm::cfg::kMaxTemperatureCritical, input);
  int hysteresis = next.alerts.recovery_hysteresis;
  if (promptIntRange(input,
                     "Recovery hysteresis (recovery = warning - hysteresis)",
                     hysteresis, atm::cfg::kMinRecoveryHysteresis,
                     atm::cfg::kMaxRecoveryHysteresis, hysteresis)) {
    next.alerts.recovery_hysteresis = hysteresis;
  }
  settings.updateSettings(next);
}

void editNotifications(atm::cfg::SettingsManager &settings,
                       ConsoleInput &input) {
  atm::cfg::AppSettings next = settings.settings();
  std::cout << "\n--- Notifications ---\n";
  bool enabled = next.notifications.enabled;
  if (promptBool(input, "Desktop notifications", enabled, enabled)) {
    next.notifications.enabled = enabled;
  }
  bool warning = next.notifications.warning_notifications;
  if (promptBool(input, "Warning notifications", warning, warning)) {
    next.notifications.warning_notifications = warning;
  }
  bool critical = next.notifications.critical_notifications;
  if (promptBool(input, "Critical notifications", critical, critical)) {
    next.notifications.critical_notifications = critical;
  }
  bool recovery = next.notifications.recovery_notifications;
  if (promptBool(input, "Recovery notifications", recovery, recovery)) {
    next.notifications.recovery_notifications = recovery;
  }
  int cooldown = next.notifications.cooldown_seconds;
  if (promptIntRange(input, "Notification cooldown (seconds)", cooldown,
                     atm::cfg::kMinCooldownSeconds,
                     atm::cfg::kMaxCooldownSeconds, cooldown)) {
    next.notifications.cooldown_seconds = cooldown;
  }
  int timeout = next.notifications.timeout_ms;
  if (promptIntRange(input, "Notification timeout (ms)", timeout,
                     atm::cfg::kMinNotificationTimeoutMs,
                     atm::cfg::kMaxNotificationTimeoutMs, timeout)) {
    next.notifications.timeout_ms = timeout;
  }
  settings.updateSettings(next);
}

void editPackages(atm::cfg::SettingsManager &settings, ConsoleInput &input) {
  atm::cfg::AppSettings next = settings.settings();
  std::cout << "\n--- Packages ---\n";
  bool check = next.packages.check_for_updates;
  if (promptBool(input, "Check for package updates at startup", check,
                 check)) {
    next.packages.check_for_updates = check;
  }
  std::cout << "\nAutomatic package installation is disabled. Updates always "
               "require\nexplicit user confirmation in the package page.\n";
  settings.updateSettings(next);
}

/// Blocking settings page. Editing happens in memory; changes are validated by
/// the SettingsManager, applied to the running components, and saved when the
/// user leaves the page. All changes apply without a restart.
void interactSettings(atm::cfg::SettingsManager &settings,
                      int &refresh_interval_ms,
                      atm::HistoryManager &history,
                      atm::AlertManager &alerts,
                      atm::NotificationManager &notifications,
                      ConsoleInput &input) {
  for (;;) {
    const atm::cfg::AppSettings &s = settings.settings();
    std::cout << "\033[2J\033[H";
    std::cout << "========================================\n"
                 "ARCH TASK MANAGER — Settings\n"
                 "========================================\n\n"
                 "Configuration file:\n  "
              << settings.configPath().string()
              << "\n  (format v" << s.config_version << ")\n\n"
              << "General:\n"
              << "  Refresh interval: " << s.general.refresh_interval_ms
              << " ms\n"
              << "  Default page: " << s.general.default_page << "\n"
              << "\nHistory:\n"
              << "  Retention: " << s.history.history_duration_seconds
              << " s (max " << s.history.max_samples << " samples, sampled "
              << "every " << s.history.sample_interval_ms << " ms)\n"
              << "\nAlerts:\n";
    printAlertCategory("CPU", s.alerts.cpu);
    printAlertCategory("Memory", s.alerts.memory);
    printAlertCategory("Swap", s.alerts.swap);
    printAlertCategory("Disk", s.alerts.disk);
    printAlertCategory("Temperature", s.alerts.temperature);
    std::cout << "  Recovery hysteresis: " << s.alerts.recovery_hysteresis
              << "\n"
              << "\nNotifications:\n"
              << "  Enabled: " << yesNo(s.notifications.enabled) << "\n"
              << "  Warning notifications: "
              << yesNo(s.notifications.warning_notifications) << "\n"
              << "  Critical notifications: "
              << yesNo(s.notifications.critical_notifications) << "\n"
              << "  Recovery notifications: "
              << yesNo(s.notifications.recovery_notifications) << "\n"
              << "  Cooldown: " << s.notifications.cooldown_seconds << " s\n"
              << "  Timeout: " << s.notifications.timeout_ms << " ms\n"
              << "\nPackages:\n"
              << "  Check for updates at startup: "
              << yesNo(s.packages.check_for_updates) << "\n"
              << "\nAutomatic package installation is disabled. Updates always "
                 "require\nexplicit user confirmation.\n"
              << "\nManagement:\n"
              << "[1] Edit General\n"
              << "[2] Edit History\n"
              << "[3] Edit Alerts\n"
              << "[4] Edit Notifications\n"
              << "[5] Edit Packages\n"
              << "[6] Reset to Defaults\n"
              << "[0] Back (save changes)\n\n"
              << "Changes apply immediately; settings are saved when you leave "
                 "this page.\n"
              << "Select action: " << std::flush;

    const std::optional<std::string> line = input.readLine();
    if (!line) {
      break;
    }
    const std::string choice = trimWhitespace(*line);
    if (choice.empty() || choice == "0") {
      break;
    }
    if (choice == "1") {
      editGeneral(settings, input);
    } else if (choice == "2") {
      editHistory(settings, input);
    } else if (choice == "3") {
      editAlerts(settings, input);
    } else if (choice == "4") {
      editNotifications(settings, input);
    } else if (choice == "5") {
      editPackages(settings, input);
    } else if (choice == "6") {
      if (confirm("Reset all settings to defaults?", input)) {
        settings.resetToDefaults();
      }
    } else {
      std::cout << "Invalid action.\n";
    }
    std::this_thread::sleep_for(300ms);
  }

  // Leaving the page: persist any pending changes, then apply them to the
  // running components. The monitoring loop picks up the new refresh interval
  // on its next cycle.
  if (settings.isDirty()) {
    settings.save();
  }
  atm::cfg::applySettingsToRuntime(settings.settings(), refresh_interval_ms,
                                   history, alerts, notifications);
}

}  // namespace

int main() {
  // --- Configuration is loaded first so every component below can be
  // --- constructed with the user's preferences.
  atm::cfg::SettingsManager settings;
  if (!settings.load()) {
    atm::Logger::warn("Continuing with default settings");
  }
  int refresh_interval_ms = settings.settings().general.refresh_interval_ms;

  atm::CpuMonitor cpu_monitor;
  atm::MemoryMonitor memory_monitor;
  atm::ProcessMonitor process_monitor;
  atm::DiskMonitor disk_monitor;
  atm::NetworkMonitor network_monitor;
  atm::GpuMonitor gpu_monitor;
  atm::SensorMonitor sensor_monitor;
  atm::SystemdManager systemd_manager;
  atm::StartupManager startup_manager;
  atm::SystemInfoProvider system_info;
  atm::ProcessActions actions;
  atm::ProcessDetails process_details;
  atm::HistoryManager history(
      static_cast<std::size_t>(settings.settings().history.max_samples));
  atm::AlertManager alerts;
  atm::NotificationManager notifications;
  atm::PackageManager packages;
  atm::PackageTransaction package_transaction;
  ConsoleInput input;

  // Apply the loaded settings to the runtime components and to the main loop.
  atm::cfg::applySettingsToRuntime(settings.settings(), refresh_interval_ms,
                                   history, alerts, notifications);

  // Forward alert state transitions to desktop notifications.
  g_notification_manager = &notifications;
  alerts.setNotificationSink(&onAlertEvent);

  // Prepare package update detection: Arch detection + libalpm are opened at
  // startup, then one read-only local check is performed when enabled in
  // settings. This only inspects the local package database and sync metadata
  // already present on disk; it never synchronises repositories or touches the
  // network. The check never installs or upgrades anything.
  packages.initialize();
  if (settings.settings().packages.check_for_updates) {
    packages.refresh();
  } else {
    atm::Logger::info("Package update checking is disabled in settings; use "
                      "the 'p' page for a manual check");
  }
  package_transaction.initialize();

  atm::ProcessSort sort = atm::ProcessSort::Cpu;
  ViewMode view = ViewMode::List;
  atm::ServiceSort service_sort = atm::ServiceSort::Name;
  std::string service_search;
  atm::StartupSort startup_sort = atm::StartupSort::Name;
  std::string startup_search;
  std::string package_search;
  bool show_history = true;
  AlertFilter alert_filter = AlertFilter::All;

  // Choose the starting view. EOF (e.g. /dev/null stdin) defaults to the
  // configured default page. "1"/"2" always win over the setting.
  std::cout << "Select view:\n"
               "[1] Process List\n"
               "[2] Process Tree\n\n"
               "Enter a number, or press Enter for the default ("
            << (settings.settings().general.default_page == "tree"
                    ? "Process Tree"
                    : "Process List")
            << "):\n> "
            << std::flush;
  const std::optional<std::string> view_choice = input.readLine();
  if (view_choice) {
    std::string choice = trimWhitespace(*view_choice);
    if (choice.empty()) {
      choice = settings.settings().general.default_page;
    }
    if (choice == "2" || choice == "tree" || choice == "Tree") {
      view = ViewMode::Tree;
    }
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
  std::this_thread::sleep_for(std::chrono::milliseconds(refresh_interval_ms));

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
  system_info.load(first_gpu);
  atm::SystemInfo sysinfo = system_info.read();
  auto snapshot = process_monitor.read(first_memory->total);
  atm::sortProcesses(snapshot.processes, sort);
  atm::ProcessTree tree = atm::buildProcessTree(snapshot.processes);

  // Seed the resource history with the first sample so graphs show data from
  // the very first frame.
  {
    std::vector<std::pair<std::string, double>> gpu_utils;
    std::vector<std::pair<std::string, double>> gpu_vrams;
    std::vector<std::pair<std::string, double>> temps;
    buildGpuMetricVectors(first_gpu, gpu_utils, gpu_vrams);
    buildTemperatureVector(first_sensors, temps);
    history.update(*first_cpu, first_memory->usagePercent(),
                   static_cast<double>(first_memory->used()),
                   static_cast<double>(first_memory->available),
                   first_memory->swapUsagePercent(),
                   static_cast<double>(first_disk.total_read_bytes_per_second),
                   static_cast<double>(first_disk.total_write_bytes_per_second),
                   static_cast<double>(first_network.total_rx_bytes_per_second),
                   static_cast<double>(first_network.total_tx_bytes_per_second),
                   gpu_utils, gpu_vrams, temps);
    updateAlerts(alerts, *first_cpu, *first_memory, first_disk, first_network,
                 first_gpu, first_sensors, history);
  }

  renderView(*first_cpu, *first_memory, snapshot.processes, snapshot.stats,
             sort, view, tree, first_disk, first_network, first_gpu,
             first_sensors, first_systemd, first_startup, sysinfo, history,
             show_history, alerts, alert_filter, service_search, service_sort,
             startup_search, startup_sort, packages, refresh_interval_ms);

  atm::NetworkSnapshot network = first_network;
  atm::GpuSnapshot gpu = first_gpu;
  atm::SensorSnapshot sensors = first_sensors;
  atm::SystemdSnapshot systemd = first_systemd;
  atm::StartupSnapshot startup = first_startup;

  for (;;) {
    // The refresh interval is re-read every cycle so a change made on the
    // settings page takes effect on the next monitoring tick.
    std::this_thread::sleep_for(std::chrono::milliseconds(refresh_interval_ms));

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
            {
              std::vector<std::pair<std::string, double>> gpu_utils;
              std::vector<std::pair<std::string, double>> gpu_vrams;
              std::vector<std::pair<std::string, double>> temps;
              buildGpuMetricVectors(gpu, gpu_utils, gpu_vrams);
              buildTemperatureVector(sensors, temps);
              history.update(*cpu, memory->usagePercent(),
                             static_cast<double>(memory->used()),
                             static_cast<double>(memory->available),
                             memory->swapUsagePercent(),
                             static_cast<double>(disk.total_read_bytes_per_second),
                             static_cast<double>(disk.total_write_bytes_per_second),
                             static_cast<double>(network.total_rx_bytes_per_second),
                             static_cast<double>(network.total_tx_bytes_per_second),
                             gpu_utils, gpu_vrams, temps);
              updateAlerts(alerts, *cpu, *memory, disk, network, gpu, sensors,
                           history);
            }
            renderView(*cpu, *memory, snapshot.processes, snapshot.stats,
                       sort, view, tree, disk, network, gpu, sensors,
                       systemd, startup, sysinfo, history, show_history,
                       alerts, alert_filter, service_search, service_sort,
                       startup_search, startup_sort, packages,
                       refresh_interval_ms);
          }
        }
        continue;
      case ConsoleInput::Command::InspectProcess:
        if (view == ViewMode::List) {
          const std::optional<atm::MemoryInfo> mem = memory_monitor.read();
          const std::uint64_t total_kib =
              mem.has_value() ? mem->total : 0;
          interactProcessDetail(process_details, actions, input,
                                snapshot.processes, sort, total_kib);
        }
        break;
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
      case ConsoleInput::Command::InspectSystemInfo:
        if (view == ViewMode::List) {
          interactSystemInfoDetail(system_info, sysinfo, gpu, input);
        }
        break;
      case ConsoleInput::Command::InspectPackages:
        if (view == ViewMode::List) {
          interactPackageDetail(packages, package_transaction, input,
                                package_search);
        }
        break;
      case ConsoleInput::Command::InspectSettings:
        if (view == ViewMode::List) {
          interactSettings(settings, refresh_interval_ms, history, alerts,
                           notifications, input);
        }
        break;
      case ConsoleInput::Command::ToggleHistory:
        show_history = !show_history;
        break;
      case ConsoleInput::Command::ToggleAlertFilter:
        alert_filter = nextAlertFilter(alert_filter);
        break;
      case ConsoleInput::Command::ToggleNotifications:
        // Keep the settings manager (single source of truth) and the runtime
        // notification manager in sync; the change is saved on exit.
        {
          atm::cfg::AppSettings next = settings.settings();
          next.notifications.enabled = !next.notifications.enabled;
          settings.updateSettings(next);
          atm::cfg::applySettingsToRuntime(
              settings.settings(), refresh_interval_ms, history, alerts,
              notifications);
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

    {
      std::vector<std::pair<std::string, double>> gpu_utils;
      std::vector<std::pair<std::string, double>> gpu_vrams;
      std::vector<std::pair<std::string, double>> temps;
      buildGpuMetricVectors(gpu, gpu_utils, gpu_vrams);
      buildTemperatureVector(sensors, temps);
      history.update(*cpu, memory->usagePercent(),
                     static_cast<double>(memory->used()),
                     static_cast<double>(memory->available),
                     memory->swapUsagePercent(),
                     static_cast<double>(disk.total_read_bytes_per_second),
                     static_cast<double>(disk.total_write_bytes_per_second),
                     static_cast<double>(network.total_rx_bytes_per_second),
                     static_cast<double>(network.total_tx_bytes_per_second),
                     gpu_utils, gpu_vrams, temps);
      updateAlerts(alerts, *cpu, *memory, disk, network, gpu, sensors, history);
    }

    renderView(*cpu, *memory, snapshot.processes, snapshot.stats, sort, view,
               tree, disk, network, gpu, sensors, systemd, startup, sysinfo,
               history, show_history, alerts, alert_filter, service_search,
               service_sort, startup_search, startup_sort, packages,
               refresh_interval_ms);
  }

  // Clean shutdown: persist any pending settings changes.
  if (settings.isDirty()) {
    settings.save();
  }
}
