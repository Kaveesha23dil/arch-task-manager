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
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

#include "alert_manager.hpp"
#include "advanced_cpu_monitor.hpp"
#include "advanced_memory_monitor.hpp"
#include "cpu_monitor.hpp"
#include "disk_health.hpp"
#include "disk_monitor.hpp"
#include "filesystem_monitor.hpp"
#include "format_bytes.hpp"
#include "gpu_monitor.hpp"
#include "history_manager.hpp"
#include "logger.hpp"
#include "memory_monitor.hpp"
#include "network_monitor.hpp"
#include "network_interface_details.hpp"
#include "network_traffic_history.hpp"
#include "notification_manager.hpp"
#include "package_manager.hpp"
#include "package_transaction.hpp"
#include "process_actions.hpp"
#include "process_cgroup.hpp"
#include "process_details.hpp"
#include "process_environment.hpp"
#include "process_io_details.hpp"
#include "process_memory_map.hpp"
#include "process_monitor.hpp"
#include "process_statistics.hpp"
#include "process_namespace.hpp"
#include "process_network.hpp"
#include "process_resources.hpp"
#include "process_scheduling.hpp"
#include "process_security.hpp"
#include "process_tree.hpp"
#include "process_report.hpp"
#include "resource_history.hpp"
#include "sensor_monitor.hpp"
#include "settings.hpp"
#include "settings_apply.hpp"
#include "settings_manager.hpp"
#include "app_autostart_manager.hpp"
#include "startup_manager.hpp"
#include "system_info.hpp"
#include "system_load_monitor.hpp"
#include "system_pressure_monitor.hpp"
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

/// Filter controlling which mounts the FILESYSTEMS table shows. The underlying
/// FilesystemMonitor model never drops mounts; this only affects the view.
enum class FilesystemFilter {
  All,        // every discovered mount
  Physical,   // real on-disk storage only (default — matches the legacy table)
  Network,    // nfs, cifs, 9p, sshfs, ...
  Temporary,  // tmpfs / ramfs
  Virtual,    // pseudo-filesystems (proc, sysfs, cgroup, ...) + overlay/container
};

/// Human-readable name of a FilesystemFilter.
const char *filesystemFilterName(FilesystemFilter filter) {
  switch (filter) {
    case FilesystemFilter::All:       return "All";
    case FilesystemFilter::Physical:  return "Physical";
    case FilesystemFilter::Network:   return "Network";
    case FilesystemFilter::Temporary: return "Temporary";
    case FilesystemFilter::Virtual:   return "Virtual";
  }
  return "All";
}

/// Returns the next FilesystemFilter when the cycling key is pressed.
FilesystemFilter nextFilesystemFilter(FilesystemFilter filter) {
  switch (filter) {
    case FilesystemFilter::All:       return FilesystemFilter::Physical;
    case FilesystemFilter::Physical:  return FilesystemFilter::Network;
    case FilesystemFilter::Network:   return FilesystemFilter::Temporary;
    case FilesystemFilter::Temporary: return FilesystemFilter::Virtual;
    case FilesystemFilter::Virtual:   return FilesystemFilter::All;
  }
  return FilesystemFilter::All;
}

/// True when the filter lets `info` through. Purely a view concern.
bool filesystemFilterAccepts(FilesystemFilter filter,
                             const atm::FilesystemInfo &info) {
  switch (filter) {
    case FilesystemFilter::All:
      return true;
    case FilesystemFilter::Physical:
      return info.classification == atm::FilesystemClass::Physical;
    case FilesystemFilter::Network:
      return info.classification == atm::FilesystemClass::Network;
    case FilesystemFilter::Temporary:
      return info.classification == atm::FilesystemClass::Temporary;
    case FilesystemFilter::Virtual:
      return info.classification == atm::FilesystemClass::Pseudo ||
             info.classification == atm::FilesystemClass::Overlay ||
             info.classification == atm::FilesystemClass::Container ||
             info.classification == atm::FilesystemClass::Unknown;
  }
  return false;
}

constexpr int kPercentPrecision = 1;
constexpr int kPercentWidth = 5;
constexpr int kLabelWidth = 21;
constexpr double kBytesPerKilobyte = 1024.0;
constexpr std::size_t kNameColumnWidth = 18;
constexpr std::size_t kMaxNameWidth = 16;
constexpr std::size_t kStorageMountWidth = 28;
constexpr std::size_t kDeviceNameWidth = 14;
constexpr std::size_t kHealthDeviceWidth = 14;
constexpr std::size_t kHealthTypeWidth = 10;
constexpr std::size_t kHealthModelWidth = 26;
constexpr std::size_t kHealthStatusWidth = 16;
constexpr std::size_t kHealthTempWidth = 9;
constexpr std::size_t kHealthPowerOnWidth = 11;
constexpr std::size_t kHealthRefreshWidth = 20;
constexpr std::size_t kHealthAttributeNameWidth = 26;
constexpr std::size_t kFsMountWidth = 24;
constexpr std::size_t kFsTypeWidth = 10;
constexpr std::size_t kFsSourceWidth = 18;
constexpr std::size_t kFsAccessWidth = 12;
constexpr std::size_t kFsSizeWidth = 9;
constexpr std::size_t kFsPercentWidth = 6;
constexpr std::size_t kFsInodeWidth = 8;
constexpr std::size_t kNetworkInterfaceWidth = 14;
constexpr std::size_t kIfDetailNameWidth = 16;
constexpr std::size_t kIfDetailTypeWidth = 10;
constexpr std::size_t kIfDetailStateWidth = 8;
constexpr std::size_t kIfDetailCarrierWidth = 8;
constexpr std::size_t kIfDetailMacWidth = 18;
constexpr std::size_t kIfDetailMtuWidth = 6;
constexpr std::size_t kIfDetailSpeedWidth = 12;
constexpr std::size_t kIfDetailDuplexWidth = 8;
constexpr std::size_t kIfDetailRateWidth = 11;
constexpr std::size_t kIfDetailAddrWidth = 22;
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
constexpr std::size_t kCpuIdWidth = 6;
constexpr std::size_t kCpuOnlineWidth = 7;
constexpr std::size_t kCpuPctWidth = 7;
constexpr std::size_t kCpuFreqWidth = 11;
constexpr std::size_t kCpuGovernorWidth = 12;
constexpr std::size_t kSwapAreaNameWidth = 32;
constexpr std::size_t kSwapAreaTypeWidth = 12;
constexpr std::size_t kSwapAreaSizeWidth = 12;
constexpr std::size_t kSwapAreaPriorityWidth = 10;
constexpr int kMinNice = atm::kMinimumNice;
constexpr int kMaxNice = atm::kMaximumNice;

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

/// Formats an optional byte count with the shared binary-unit scheme, or "N/A"
/// when the field is unavailable. Unavailable fields are never faked as zero.
std::string formatBytesOptional(const std::optional<std::uint64_t> &bytes) {
  if (!bytes.has_value()) {
    return "N/A";
  }
  return atm::formatBytes(*bytes);
}

/// Formats an optional count (e.g. huge-page page counts) or "N/A".
std::string formatCountOptional(const std::optional<std::uint64_t> &count) {
  if (!count.has_value()) {
    return "N/A";
  }
  return std::to_string(*count);
}

/// Formats an optional percentage (0.0–100.0, already clamped) or "N/A".
std::string formatPercentOptional(const std::optional<double> &percent) {
  if (!percent.has_value()) {
    return "N/A";
  }
  return formatPercent(*percent) + "%";
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
      << std::setw(6) << process.thread_count << "  "
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

/// Renders the MEMORY, MEMORY DETAILS (composition), SWAP (+ swap areas),
/// COMMITMENT and HUGE PAGES sections (Steps 2 and 36).
void renderMemorySections(std::ostringstream &out,
                          const atm::MemoryInfo &memory,
                          const atm::AdvancedMemorySnapshot &advanced) {
  out << "\n## MEMORY\n\n";
  appendLabeled(out, "Total:", formatKibibytes(memory.total));
  appendLabeled(out, "Used:", formatKibibytes(memory.used()));
  appendLabeled(out, "Available:", formatKibibytes(memory.available));
  appendLabeled(out, "Free:", formatKibibytes(memory.free));
  appendLabeled(out, "Cached:", formatKibibytes(memory.cached));
  appendLabeled(out, "Buffers:", formatKibibytes(memory.buffers));
  appendLabeled(out, "Usage:", formatPercent(memory.usagePercent()) + "%");
  if (advanced.available_is_estimate) {
    out << "Available is an estimate (MemFree + Buffers + Cached); "
           "MemAvailable was not reported by this kernel.\n";
  }

  out << "\n## MEMORY DETAILS\n\n"
      << "Memory composition (values from /proc/meminfo; rows overlap and do "
         "not add up to total RAM - Cached includes Shared/tmpfs, and "
         "SReclaimable/SUnreclaim are parts of Slab):\n";
  appendLabeled(out, "Buffers:", formatBytesOptional(advanced.buffers));
  appendLabeled(out, "Cached:", formatBytesOptional(advanced.cached));
  appendLabeled(out, "Reclaimable:",
                formatBytesOptional(advanced.reclaimableKernelBytes()));
  appendLabeled(out, "Unreclaimable:",
                formatBytesOptional(advanced.sunreclaim));
  appendLabeled(out, "Anonymous:",
                formatBytesOptional(advanced.anonymousBytes()));
  appendLabeled(out, "File-backed:",
                formatBytesOptional(advanced.fileBackedBytes()));
  appendLabeled(out, "Shared:", formatBytesOptional(advanced.shmem));
  appendLabeled(out, "Active:", formatBytesOptional(advanced.active));
  appendLabeled(out, "Inactive:", formatBytesOptional(advanced.inactive));
  appendLabeled(out, "Kernel stack:",
                formatBytesOptional(advanced.kernel_stack));
  appendLabeled(out, "Page tables:",
                formatBytesOptional(advanced.page_tables));

  out << "\nActivity & kernel accounting:\n";
  appendLabeled(out, "Active(anon):",
                formatBytesOptional(advanced.active_anon));
  appendLabeled(out, "Inactive(anon):",
                formatBytesOptional(advanced.inactive_anon));
  appendLabeled(out, "Active(file):",
                formatBytesOptional(advanced.active_file));
  appendLabeled(out, "Inactive(file):",
                formatBytesOptional(advanced.inactive_file));
  appendLabeled(out, "Unevictable:",
                formatBytesOptional(advanced.unevictable));
  appendLabeled(out, "Mlocked:", formatBytesOptional(advanced.mlocked));
  appendLabeled(out, "Slab:", formatBytesOptional(advanced.slab));
  appendLabeled(out, "Mapped:", formatBytesOptional(advanced.mapped));
  appendLabeled(out, "Writeback:", formatBytesOptional(advanced.writeback));
  appendLabeled(out, "WritebackTmp:",
                formatBytesOptional(advanced.writeback_tmp));
  if (advanced.kernelMemoryBytes().has_value()) {
    appendLabeled(out, "Kernel memory estimate:",
                  atm::formatBytes(*advanced.kernelMemoryBytes()));
  }

  out << "\n## SWAP\n\n";
  appendLabeled(out, "Total:", formatKibibytes(memory.swap_total));
  appendLabeled(out, "Used:", formatKibibytes(memory.swapUsed()));
  appendLabeled(out, "Free:", formatKibibytes(memory.swap_free));
  appendLabeled(out, "Cached:", formatBytesOptional(advanced.swap_cached));
  appendLabeled(out, "Usage:", formatPercent(memory.swapUsagePercent()) + "%");

  out << "\nSwap areas (" << advanced.swap_areas.size() << " configured):\n";
  if (advanced.swap_areas.empty()) {
    out << "None.\n";
  } else {
    out << std::left << std::setw(kSwapAreaNameWidth) << "AREA" << std::right
        << std::setw(kSwapAreaTypeWidth) << "TYPE"
        << std::setw(kSwapAreaSizeWidth) << "SIZE"
        << std::setw(kSwapAreaSizeWidth) << "USED"
        << std::setw(kSwapAreaPriorityWidth) << "PRIORITY" << '\n';
    for (const atm::SwapAreaInfo &area : advanced.swap_areas) {
      const std::string name =
          area.filename.size() <= kSwapAreaNameWidth
              ? area.filename
              : area.filename.substr(0, kSwapAreaNameWidth);
      const std::string priority =
          area.priority.has_value() ? std::to_string(*area.priority) : "N/A";
      out << std::left << std::setw(kSwapAreaNameWidth) << name << std::right
          << std::setw(kSwapAreaTypeWidth) << fitTo(area.type, kSwapAreaTypeWidth)
          << std::setw(kSwapAreaSizeWidth)
          << formatBytesOptional(area.size_bytes)
          << std::setw(kSwapAreaSizeWidth)
          << formatBytesOptional(area.used_bytes)
          << std::setw(kSwapAreaPriorityWidth) << priority << '\n';
    }
  }

  // Commitment is a virtual-memory figure, never resident RAM.
  if (advanced.commit_limit.has_value()) {
    out << "\n## COMMITMENT (virtual memory, not resident RAM)\n\n";
    appendLabeled(out, "Commit limit:",
                  formatBytesOptional(advanced.commit_limit));
    appendLabeled(out, "Committed:",
                  formatBytesOptional(advanced.committed_as));
    appendLabeled(out, "Usage:",
                  formatPercentOptional(advanced.commitPercent()));
  }

  if (advanced.hasHugePageInfo()) {
    out << "\n## HUGE PAGES\n\n";
    appendLabeled(out, "Pages total:",
                  formatCountOptional(advanced.huge_pages_total));
    appendLabeled(out, "Pages free:",
                  formatCountOptional(advanced.huge_pages_free));
    appendLabeled(out, "Pages reserved:",
                  formatCountOptional(advanced.huge_pages_rsvd));
    appendLabeled(out, "Pages surplus:",
                  formatCountOptional(advanced.huge_pages_surp));
    appendLabeled(out, "Hugepage size:",
                  formatBytesOptional(advanced.hugepagesize));
    appendLabeled(out, "Hugetlb:", formatBytesOptional(advanced.hugetlb));
    appendLabeled(out, "AnonHugePages:",
                  formatBytesOptional(advanced.anon_huge_pages));
    appendLabeled(out, "ShmemHugePages:",
                  formatBytesOptional(advanced.shmem_huge_pages));
    appendLabeled(out, "ShmemPmdMapped:",
                  formatBytesOptional(advanced.shmem_pmd_mapped));
    appendLabeled(out, "FileHugePages:",
                  formatBytesOptional(advanced.file_huge_pages));
    appendLabeled(out, "FilePmdMapped:",
                  formatBytesOptional(advanced.file_pmd_mapped));
    appendLabeled(out, "DirectMap4k:",
                  formatBytesOptional(advanced.direct_map_4k));
    appendLabeled(out, "DirectMap2M:",
                  formatBytesOptional(advanced.direct_map_2m));
    appendLabeled(out, "DirectMap1G:",
                  formatBytesOptional(advanced.direct_map_1g));
  }
}

/// Formats one percentage cell of the per-CPU table: a fixed-width percentage
/// when a valid sample exists, "N/A" otherwise (offline/new counter baseline).
std::string formatCpuPctCell(bool valid, double percent) {
  if (valid) {
    return formatPercent(percent) + "%";
  }
  return "N/A";
}

/// Builds the (cpu_id, utilization) pairs for a CPU-details snapshot. Only
/// CPUs with a valid sample are included, so offline or freshly-appeared CPUs
/// never corrupt the per-CPU history set.
std::vector<std::pair<int, double>> cpuHistoryVector(
    const atm::AdvancedCpuSnapshot &cpu) {
  std::vector<std::pair<int, double>> out;
  out.reserve(cpu.cpus.size());
  for (const atm::CpuStatistics &stat : cpu.cpus) {
    if (stat.has_sample) {
      out.emplace_back(stat.cpu_id, stat.delta.busy_percent);
    }
  }
  return out;
}

/// The aggregate busy percentage of a CPU-details snapshot (0 when no valid
/// sample exists yet), used to drive the header, alerts and the history graph.
double aggregateCpuPercent(const atm::AdvancedCpuSnapshot &cpu) {
  return cpu.aggregate.has_sample ? cpu.aggregate.delta.busy_percent : 0.0;
}

/// Builds the advanced-memory history sample for a snapshot. Unavailable
/// metrics stay unset so the history manager simply skips them for that
/// refresh instead of plotting garbage.
atm::AdvancedMemoryMetrics buildAdvancedMemoryMetrics(
    const atm::AdvancedMemorySnapshot &memory) {
  atm::AdvancedMemoryMetrics metrics;
  metrics.available_percent = memory.availablePercent();
  if (memory.cached.has_value()) {
    metrics.cached_bytes = static_cast<double>(*memory.cached);
  }
  if (memory.reclaimableKernelBytes().has_value()) {
    metrics.reclaimable_bytes =
        static_cast<double>(*memory.reclaimableKernelBytes());
  }
  metrics.commitment_percent = memory.commitPercent();
  return metrics;
}

/// Builds the system-pressure history sample for a snapshot. Only the
/// 10-second some/full averages are graphed (60/300-second windows and totals
/// live in the pressure detail table). Values that are unavailable stay unset
/// so the history manager skips the sample instead of plotting garbage.
atm::PressureMetrics buildPressureMetrics(
    const atm::SystemPressureSnapshot &pressure) {
  atm::PressureMetrics metrics;
  metrics.cpu_some = pressure.cpu.some.avg10;
  metrics.cpu_full = pressure.cpu.full.avg10;
  metrics.memory_some = pressure.memory.some.avg10;
  metrics.memory_full = pressure.memory.full.avg10;
  metrics.io_some = pressure.io.some.avg10;
  metrics.io_full = pressure.io.full.avg10;
  return metrics;
}

/// The number of logical CPUs currently online, from the advanced CPU
/// monitor's cached topology. 0 when the kernel did not report a usable online
/// mask; load normalization is skipped in that case rather than guessed.
std::uint32_t onlineLogicalCpuCount(const atm::AdvancedCpuSnapshot &cpu) {
  if (!cpu.topology.online_available) {
    return 0;
  }
  return static_cast<std::uint32_t>(cpu.topology.online.size());
}

/// Builds the system-load history sample for a snapshot. Raw 1/5/15-minute
/// averages are recorded whenever the file was readable; per-CPU normalized
/// averages are recorded only when an online CPU count is known. Unavailable
/// values stay unset so the history manager skips them for that refresh
/// instead of plotting garbage.
atm::LoadMetrics buildLoadMetrics(const atm::SystemLoadSnapshot &load,
                                  std::uint32_t online_cpus) {
  atm::LoadMetrics metrics;
  if (!load.load.readable) {
    return metrics;
  }
  metrics.load1 = load.load.load1;
  metrics.load5 = load.load.load5;
  metrics.load15 = load.load.load15;
  metrics.normalized_load1 =
      atm::normalizeLoad(load.load.load1, online_cpus);
  metrics.normalized_load5 =
      atm::normalizeLoad(load.load.load5, online_cpus);
  metrics.normalized_load15 =
      atm::normalizeLoad(load.load.load15, online_cpus);
  return metrics;
}

/// Renders the ADVANCED CPU section of the live view: the total-CPU time
/// breakdown followed by one row per logical CPU. Frequencies are the values
/// the kernel currently reports and may be shared by CPUs on the same policy;
/// they are never claimed to be guaranteed hardware clocks.
void renderCpuSections(std::ostringstream &out,
                       const atm::AdvancedCpuSnapshot &cpu) {
  out << "\n## CPU DETAILS\n\n";

  const atm::CpuStatistics &aggregate = cpu.aggregate;
  if (cpu.proc_stat_readable && aggregate.has_sample) {
    out << "Total CPU (percent of elapsed CPU time):\n";
    appendLabeled(out, "Busy:", formatPercent(aggregate.delta.busy_percent) + "%");
    appendLabeled(out, "User:", formatPercent(aggregate.delta.user_percent) + "%");
    appendLabeled(out, "Nice:", formatPercent(aggregate.delta.nice_percent) + "%");
    appendLabeled(out, "System:",
                  formatPercent(aggregate.delta.system_percent) + "%");
    appendLabeled(out, "I/O Wait:",
                  formatPercent(aggregate.delta.iowait_percent) + "%");
    appendLabeled(out, "IRQ:", formatPercent(aggregate.delta.irq_percent) + "%");
    appendLabeled(out, "SoftIRQ:",
                  formatPercent(aggregate.delta.softirq_percent) + "%");
    appendLabeled(out, "Steal:",
                  formatPercent(aggregate.delta.steal_percent) + "%");
    appendLabeled(out, "Idle:", formatPercent(aggregate.delta.idle_percent) + "%");
  } else if (cpu.proc_stat_readable) {
    out << "Total CPU: calculating first sample...\n";
  } else {
    out << "Total CPU: unavailable (/proc/stat could not be read)\n";
  }

  out << "\nPer-CPU:\n";
  out << std::right << std::setw(kCpuIdWidth) << "CPU" << " "
      << std::left << std::setw(kCpuOnlineWidth) << "Online" << " "
      << std::right << std::setw(kCpuPctWidth) << "Usage" << " "
      << std::setw(kCpuPctWidth) << "User" << " " << std::setw(kCpuPctWidth)
      << "System" << " " << std::setw(kCpuPctWidth) << "I/O Wait" << " "
      << std::setw(kCpuPctWidth) << "Idle" << " " << std::setw(kCpuFreqWidth)
      << "Frequency" << " " << std::left << std::setw(kCpuGovernorWidth)
      << "Governor\n";

  for (const atm::CpuStatistics &stat : cpu.cpus) {
    const bool valid = stat.has_sample;
    out << std::right << std::setw(kCpuIdWidth)
        << fitTo("cpu" + std::to_string(stat.cpu_id), kCpuIdWidth) << " "
        << std::left << std::setw(kCpuOnlineWidth)
        << (stat.online ? "online" : "offline") << " " << std::right
        << std::setw(kCpuPctWidth)
        << formatCpuPctCell(valid, stat.delta.busy_percent) << " "
        << std::setw(kCpuPctWidth)
        << formatCpuPctCell(valid, stat.delta.user_percent) << " "
        << std::setw(kCpuPctWidth)
        << formatCpuPctCell(valid, stat.delta.system_percent) << " "
        << std::setw(kCpuPctWidth)
        << formatCpuPctCell(valid, stat.delta.iowait_percent) << " "
        << std::setw(kCpuPctWidth)
        << formatCpuPctCell(valid, stat.delta.idle_percent) << " "
        << std::setw(kCpuFreqWidth)
        << atm::formatCpuFrequency(stat.frequency.current_khz) << " "
        << std::left << std::setw(kCpuGovernorWidth)
        << fitTo(atm::formatCpuGovernor(stat.frequency.governor),
                 kCpuGovernorWidth) << '\n';
  }

  out << "\nFrequency is the currently reported cpufreq value (kHz); it is not "
         "a guaranteed hardware clock, and CPUs sharing a frequency policy "
         "report the same values.\n";
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

/// Renders the DISK ACTIVITY (aggregate read/write rates) and DEVICES (whole
/// physical disks) sections. Detailed filesystem capacity moved to the
/// dedicated FILESYSTEMS section (Step 40); the alert feed still uses the
/// DiskMonitor's own filesystem capacities.
void renderStorageSections(std::ostringstream &out,
                           const atm::DiskSnapshot &disk) {
  out << "\n## STORAGE\n\n## DISK ACTIVITY\n\n";
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

/// Compact address list for the table (first two, "..." if more).
std::string renderAddressList(const std::vector<atm::NetworkAddressInfo> &addresses) {
  std::ostringstream out;
  const std::size_t shown = addresses.size() > 2 ? 2 : addresses.size();
  for (std::size_t i = 0; i < shown; ++i) {
    if (i != 0) {
      out << ", ";
    }
    out << addresses[i].address;
  }
  if (addresses.size() > shown) {
    out << ", ...";
  }
  return out.str();
}

/// Renders the NETWORK INTERFACES table (link metadata + addresses + the same
/// traffic rates as the NETWORK section). Loopback floats last, matching the
/// traffic table; rows are stable across renames because selection keys on the
/// sysfs ifindex, not the name.
std::string renderNetworkInterfaceTableText(
    const atm::NetworkInterfaceSnapshot &snapshot) {
  std::ostringstream out;
  if (!snapshot.sysfs_readable) {
    out << "Interface details unavailable: " << snapshot.error_detail << "\n";
    return out.str();
  }
  out << std::left << std::setw(kIfDetailNameWidth) << "Interface"
      << std::setw(kIfDetailTypeWidth) << "Type"
      << std::setw(kIfDetailStateWidth) << "State"
      << std::setw(kIfDetailCarrierWidth) << "Carrier"
      << std::setw(kIfDetailMacWidth) << "MAC"
      << std::setw(kIfDetailMtuWidth) << "MTU"
      << std::setw(kIfDetailSpeedWidth) << "Speed"
      << std::setw(kIfDetailDuplexWidth) << "Duplex"
      << std::right << std::setw(kIfDetailRateWidth) << "RX"
      << std::setw(kIfDetailRateWidth) << "TX" << std::left
      << std::setw(kIfDetailAddrWidth) << "  Addresses" << '\n';
  for (const atm::NetworkInterfaceInfo &info : snapshot.interfaces) {
    const std::uint64_t rx =
        info.traffic ? info.traffic->rx_bytes_per_second : 0;
    const std::uint64_t tx =
        info.traffic ? info.traffic->tx_bytes_per_second : 0;
    std::string state = fitTo(toUpperAscii(info.link.operstate.value_or("unknown")),
                              kIfDetailStateWidth);
    out << std::left << std::setw(kIfDetailNameWidth)
        << fitTo(info.name, kIfDetailNameWidth)
        << std::setw(kIfDetailTypeWidth)
        << atm::networkInterfaceTypeName(info.type)
        << std::setw(kIfDetailStateWidth) << state
        << std::setw(kIfDetailCarrierWidth)
        << atm::formatNetworkCarrier(info.link.carrier)
        << std::setw(kIfDetailMacWidth)
        << fitTo(info.link.mac_address.value_or("N/A"), kIfDetailMacWidth)
        << std::setw(kIfDetailMtuWidth)
        << (info.link.mtu.has_value() ? std::to_string(*info.link.mtu) : "-")
        << std::setw(kIfDetailSpeedWidth)
        << atm::formatNetworkSpeed(info.link.speed_mbps)
        << std::setw(kIfDetailDuplexWidth)
        << atm::formatNetworkDuplex(info.link.duplex) << std::right
        << std::setw(kIfDetailRateWidth) << atm::formatNetworkRate(rx)
        << std::setw(kIfDetailRateWidth) << atm::formatNetworkRate(tx) << std::left
        << std::setw(kIfDetailAddrWidth)
        << "  " + fitTo(renderAddressList(info.addresses), kIfDetailAddrWidth - 2)
        << '\n';
  }
  return out.str();
}

/// Renders the NETWORK INTERFACES section. The monitor reads sysfs and
/// getifaddrs() once per tick (from the monitoring loop), so rendering is a
/// pure read of the cached snapshot.
void renderNetworkInterfaceSections(std::ostringstream &out,
                                    const atm::NetworkInterfaceMonitor &monitor) {
  out << "\n## NETWORK INTERFACES\n\n"
      << renderNetworkInterfaceTableText(monitor.current())
      << "\nDetailed info: press 'i' (then Enter)\n";
}

/// Formats a timestamp as "YYYY-MM-DD HH:MM:SS" for the network traffic
/// "last update" line (defined later in this file alongside the other
/// formatters; declared here so the traffic section can use it).
std::string formatTimestamp(std::chrono::system_clock::time_point timestamp);

/// Renders the NETWORK TRAFFIC HISTORY section: RX/TX byte-rate graphs plus
/// current/peak/in-window-total/samples/span/last-update for one selected
/// series (the aggregate or a tracked interface). Everything is read from the
/// NetworkTrafficHistory ring buffers — the renderer never touches the network
/// itself and never parses /proc/net/dev again. The 'c' key cycles the
/// selection; a stale selection (removed interface) falls back to the
/// aggregate via displayNameFor()/seriesFor() behaviour in the component.
void renderNetworkTrafficHistorySection(
    std::ostringstream &out, const atm::NetworkTrafficHistory &traffic,
    const std::string &selection) {
  out << "\n## NETWORK TRAFFIC HISTORY\n\n";
  const atm::NetworkTrafficSeries *series = traffic.seriesFor(selection);
  if (series == nullptr) {
    out << "Traffic history: no interfaces sampled yet (waiting for the next "
           "network read).\n"
        << "Selection: press 'c' (then Enter) to cycle.\n";
    return;
  }

  out << "History: " << series->display_name
      << "  (press 'c' to cycle)\n";

  atm::GraphConfig rate;
  rate.width = 40;
  rate.height = 6;
  rate.dynamic_scale = true;

  // Current/peak from the derived byte-rate rings (bytes/s).
  double rx_current = 0.0, rx_peak = 0.0;
  for (const atm::TimedSample &sample : series->rx_bytes_per_second.samples()) {
    rx_current = sample.value;
    rx_peak = std::max(rx_peak, sample.value);
  }
  double tx_current = 0.0, tx_peak = 0.0;
  for (const atm::TimedSample &sample : series->tx_bytes_per_second.samples()) {
    tx_current = sample.value;
    tx_peak = std::max(tx_peak, sample.value);
  }

  const bool has_rates = !series->rx_bytes_per_second.empty() ||
                         !series->tx_bytes_per_second.empty();
  if (has_rates) {
    out << "[RX rate]\n"
        << atm::GraphRenderer::renderText(series->rx_bytes_per_second, rate, "",
                                          "B/s")
        << '\n'
        << "[TX rate]\n"
        << atm::GraphRenderer::renderText(series->tx_bytes_per_second, rate, "",
                                          "B/s")
        << '\n';
  } else {
    out << "[RX rate]      (no samples yet)\n"
        << "[TX rate]      (no samples yet)\n"
        << "Waiting for a second network read to measure transfer rates.\n"
        << '\n';
  }

  const auto formatWs = [](double value) {
    return atm::formatNetworkRate(
        value > 0.0 ? static_cast<std::uint64_t>(value) : 0);
  };

  // Total transferred during the retained window (last - first cumulative
  // sample). A counter reset inside the window or an aggregate membership
  // change makes the delta unreliable: reported as unavailable, never a bogus
  // negative or a doubled value.
  const auto windowTotal = [](const atm::ResourceHistory<atm::TimedSample> &ring)
      -> std::optional<double> {
    if (ring.size() < 2) {
      return std::nullopt;
    }
    const double delta = ring.samples().back().value - ring.samples().front().value;
    return delta >= 0.0 ? std::optional<double>(delta) : std::nullopt;
  };
  const std::optional<double> rx_total = windowTotal(series->rx_bytes_total);
  const std::optional<double> tx_total = windowTotal(series->tx_bytes_total);

  out << "Current RX: " << formatWs(rx_current)
      << "   Peak RX: " << formatWs(rx_peak) << '\n'
      << "Current TX: " << formatWs(tx_current)
      << "   Peak TX: " << formatWs(tx_peak) << '\n';

  out << "Total in window: RX "
      << (rx_total.has_value() ? atm::formatBytes(static_cast<std::uint64_t>(*rx_total))
                               : "n/a (counter reset / insufficient data)")
      << "   TX "
      << (tx_total.has_value() ? atm::formatBytes(static_cast<std::uint64_t>(*tx_total))
                               : "n/a")
      << '\n';

  // Samples and time span (steady clock range of the recorded cumulative ring).
  out << "Samples: " << series->rx_bytes_total.size();
  if (series->rx_bytes_total.size() >= 2) {
    const double span_seconds = std::chrono::duration<double>(
        series->rx_bytes_total.samples().back().timestamp -
        series->rx_bytes_total.samples().front().timestamp)
                                    .count();
    out << "   Span: "
        << (span_seconds < 1.0 ? "<1 s"
                               : std::to_string(static_cast<long long>(span_seconds)) +
                                     " s");
  }
  out << "   Updated: " << formatTimestamp(series->last_update) << '\n';

  if (series->aggregate) {
    out << "Note: the aggregate includes virtual/bridge/tunnel interfaces — "
           "not physical link throughput. Totals are the sum of all "
           "non-loopback interface counters.\n";
    if (series->membership_changed) {
      out << "Note: the interface set changed this tick; totals across the "
             "change are approximate.\n";
    }
  }
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

  // Per-CPU utilization (%). Capped so the frame stays reasonable on systems
  // with many logical CPUs; the HistoryManager still tracks every CPU.
  const auto &cpus = history.cpuHistories();
  if (!cpus.empty()) {
    out << "\n[Per-CPU Utilization]\n";
    constexpr std::size_t kPerCpuGraphLimit = 8;
    for (std::size_t i = 0; i < cpus.size() && i < kPerCpuGraphLimit; ++i) {
      out << "cpu" << cpus[i].cpu_id << '\n'
          << atm::GraphRenderer::renderText(cpus[i].utilization, percent, "",
                                            "%")
          << '\n';
    }
    if (cpus.size() > kPerCpuGraphLimit) {
      out << "... " << (cpus.size() - kPerCpuGraphLimit)
          << " more CPUs not shown (per-CPU data is still tracked).\n";
    }
  } else {
    out << "\n[Per-CPU Utilization]\n"
        << "N/A (no per-CPU data yet).\n";
  }

  // Memory usage (%).
  out << "\n[MEMORY Usage]\n" << atm::GraphRenderer::renderText(
      history.memoryHistory(), percent, "", "%") << '\n';

  // Swap usage (%).
  out << "\n[SWAP Usage]\n" << atm::GraphRenderer::renderText(
      history.swapHistory(), percent, "", "%") << '\n';

  // Advanced memory (available %, absolute quantities auto-scale).
  out << "\n[AVAILABLE MEMORY]\n" << atm::GraphRenderer::renderText(
      history.memoryAvailablePercentHistory(), percent, "", "%") << '\n';
  out << "\n[USED RAM]\n" << atm::GraphRenderer::renderText(
      history.usedRamHistory(), rate, "", "kB") << '\n';
  out << "\n[CACHED]\n" << atm::GraphRenderer::renderText(
      history.cachedBytesHistory(), rate, "", "B") << '\n';
  out << "\n[RECLAIMABLE]\n" << atm::GraphRenderer::renderText(
      history.reclaimableBytesHistory(), rate, "", "B") << '\n';
  if (!history.commitmentPercentHistory().empty()) {
    out << "\n[COMMITMENT Usage]\n" << atm::GraphRenderer::renderText(
        history.commitmentPercentHistory(), percent, "", "%") << '\n';
  }

  // System load — raw load averages and per-CPU normalized averages. These are
  // counts (not percentages) so they auto-scale; a missing dataset simply has
  // no graph. The normalized graphs only appear once an online CPU count is
  // known.
  {
    atm::GraphConfig load_scale;
    load_scale.width = 40;
    load_scale.height = 6;
    load_scale.dynamic_scale = true;
    const auto renderLoadHist =
        [&](const char *title,
            const atm::ResourceHistory<atm::TimedSample> &h) {
          if (!h.empty()) {
            out << "\n" << title << "\n"
                << atm::GraphRenderer::renderText(h, load_scale, "", "")
                << '\n';
          }
        };
    if (!history.load1History().empty()) {
      out << "\n[SYSTEM LOAD]\n";
      renderLoadHist("Load Average (1 min)", history.load1History());
      renderLoadHist("Load Average (5 min)", history.load5History());
      renderLoadHist("Load Average (15 min)", history.load15History());
    }
    if (!history.normalizedLoad1History().empty() ||
        !history.normalizedLoad5History().empty() ||
        !history.normalizedLoad15History().empty()) {
      out << "\n[NORMALIZED LOAD]\n";
      renderLoadHist("Normalized Load (1 min)",
                     history.normalizedLoad1History());
      renderLoadHist("Normalized Load (5 min)",
                     history.normalizedLoad5History());
      renderLoadHist("Normalized Load (15 min)",
                     history.normalizedLoad15History());
    }
  }

  // System pressure (PSI) — 10-second averages. Only metrics that have data
  // are graphed; unavailable categories simply have no graph.
  {
    const auto renderPressureHist =
        [&](const char *title,
            const atm::ResourceHistory<atm::TimedSample> &h) {
          if (!h.empty()) {
            out << "\n" << title << "\n"
                << atm::GraphRenderer::renderText(h, percent, "", "%")
                << '\n';
          }
        };
    bool any_pressure = !history.cpuPressureSomeHistory().empty() ||
                        !history.cpuPressureFullHistory().empty() ||
                        !history.memoryPressureSomeHistory().empty() ||
                        !history.memoryPressureFullHistory().empty() ||
                        !history.ioPressureSomeHistory().empty() ||
                        !history.ioPressureFullHistory().empty();
    if (any_pressure) {
      out << "\n[SYSTEM PRESSURE]\n";
      renderPressureHist("CPU Pressure Some",
                         history.cpuPressureSomeHistory());
      renderPressureHist("CPU Pressure Full",
                         history.cpuPressureFullHistory());
      renderPressureHist("Memory Pressure Some",
                         history.memoryPressureSomeHistory());
      renderPressureHist("Memory Pressure Full",
                         history.memoryPressureFullHistory());
      renderPressureHist("I/O Pressure Some",
                         history.ioPressureSomeHistory());
      renderPressureHist("I/O Pressure Full",
                         history.ioPressureFullHistory());
    }
  }

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

constexpr std::size_t kPressureResourceWidth = 8;
constexpr std::size_t kPressureTypeWidth = 5;
constexpr std::size_t kPressureAvgWidth = 6;
constexpr std::size_t kPressureTotalWidth = 10;

/// Formats a PSI percentage average for display in the pressure table.
std::string formatPressureAvg(const std::optional<double> &avg) {
  if (!avg.has_value()) {
    return "N/A";
  }
  return formatPercent(*avg) + "%";
}

/// Formats a cumulative PSI stalled-time total stored in microseconds.
/// Values are scaled to the largest human-readable unit for compactness.
/// PSI `total` is documented by the kernel to be in microseconds and is NOT a
/// wall-clock second count.
std::string formatPressureTotalMicroseconds(
    const std::optional<std::uint64_t> &total) {
  if (!total.has_value()) {
    return "N/A";
  }
  std::ostringstream out;
  const std::uint64_t usec = *total;
  if (usec < 1'000) {
    out << usec << " \u00b5s";
  } else if (usec < 1'000'000) {
    out << std::fixed << std::setprecision(2) << (usec / 1'000.0) << " ms";
  } else if (usec < 1'000'000ULL * 3'600) {
    out << std::fixed << std::setprecision(2) << (usec / 1'000'000.0) << " s";
  } else {
    out << std::fixed << std::setprecision(1)
        << (usec / (1'000'000.0 * 3'600)) << " h";
  }
  return out.str();
}

/// Small glyph for a pressure severity indicator in the summary view.
const char *pressureSeverityGlyph(atm::PressureSeverity severity) {
  switch (severity) {
    case atm::PressureSeverity::Normal:   return "\u25cb";  // ○
    case atm::PressureSeverity::Elevated: return "\u25b2";  // ▲
    case atm::PressureSeverity::High:     return "\u26a0";   // ⚠
    case atm::PressureSeverity::Critical: return "\U0001f534";  // 🔴
  }
  return "?";
}

/// Renders one pressure-category summary line showing avg10, severity,
/// and availability.
void renderPressureCategorySummary(
    std::ostringstream &out, const char *name,
    const atm::PressureCategoryData &data) {
  out << std::left << std::setw(kPressureResourceWidth) << name << "Some: "
      << formatPressureAvg(data.some.avg10);
  if (data.full.line_present) {
    out << "  Full: " << formatPressureAvg(data.full.avg10);
  }
  const std::optional<atm::PressureSeverity> sev =
      data.some.avg10.has_value()
          ? atm::classifyPressureSeverity(*data.some.avg10)
          : (data.full.avg10.has_value()
                 ? atm::classifyPressureSeverity(*data.full.avg10)
                 : std::optional<atm::PressureSeverity>{});
  if (sev.has_value()) {
    out << "  " << pressureSeverityGlyph(*sev) << " "
        << atm::pressureSeverityName(*sev);
  }
  if (data.status != atm::PressureReadStatus::Read) {
    out << "  [" << atm::pressureReadStatusName(data.status) << "]";
  }
  out << '\n';
}

/// Renders the SYSTEM PRESSURE section: a three-line summary plus a full
/// some/full detail table for every category. This section is read-only; PSI
/// being unavailable shows a soft degradation message.
void renderPressureSections(std::ostringstream &out,
                            const atm::SystemPressureSnapshot &pressure) {
  out << "\n## SYSTEM PRESSURE\n\n";
  if (!pressure.anyAvailable()) {
    out << "PSI unavailable — no system pressure data.\n";
    return;
  }

  renderPressureCategorySummary(out, "CPU", pressure.cpu);
  renderPressureCategorySummary(out, "Memory", pressure.memory);
  renderPressureCategorySummary(out, "I/O", pressure.io);

  out << "\n";
  out << std::left << std::setw(kPressureResourceWidth) << "Resource"
      << ' ' << std::left << std::setw(kPressureTypeWidth) << "Type"
      << std::right << std::setw(kPressureAvgWidth) << "Avg10"
      << std::setw(kPressureAvgWidth) << "Avg60"
      << std::setw(kPressureAvgWidth + 1) << "Avg300"
      << std::setw(kPressureTotalWidth) << "Total\n";

  // Build a row for one metric (some or full) of one category. Rows for
  // metrics whose line was never present are shown with "N/A" cells.
  const auto renderRow =
      [&](const char *resource, const char *type,
          const atm::PressureMetric &metric) {
        out << std::left << std::setw(kPressureResourceWidth) << resource
            << ' ' << std::left << std::setw(kPressureTypeWidth) << type;
        out << std::right << std::setw(kPressureAvgWidth)
            << formatPressureAvg(metric.avg10)
            << std::setw(kPressureAvgWidth)
            << formatPressureAvg(metric.avg60)
            << std::setw(kPressureAvgWidth + 1)
            << formatPressureAvg(metric.avg300)
            << std::setw(kPressureTotalWidth)
            << formatPressureTotalMicroseconds(metric.total)
            << '\n';
      };

  renderRow("CPU", "Some", pressure.cpu.some);
  if (pressure.cpu.full.line_present || pressure.cpu.some.line_present) {
    renderRow("CPU", "Full", pressure.cpu.full);
  }
  renderRow("Memory", "Some", pressure.memory.some);
  if (pressure.memory.full.line_present || pressure.memory.some.line_present) {
    renderRow("Memory", "Full", pressure.memory.full);
  }
  renderRow("I/O", "Some", pressure.io.some);
  if (pressure.io.full.line_present || pressure.io.some.line_present) {
    renderRow("I/O", "Full", pressure.io.full);
  }
  out << '\n';
}

/// Formats a load average with two decimals, e.g. "0.42".
std::string formatLoadAverage(double load) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(2) << load;
  return out.str();
}

/// Small glyph for a load-severity indicator in the summary view.
const char *loadSeverityGlyph(atm::LoadSeverity severity) {
  switch (severity) {
    case atm::LoadSeverity::Normal:   return "\u25cb";  // ○
    case atm::LoadSeverity::Elevated: return "\u25b2";  // ▲
    case atm::LoadSeverity::High:     return "\u26a0";   // ⚠
    case atm::LoadSeverity::Critical: return "\U0001f534";  // 🔴
  }
  return "?";
}

/// Formats a wall-clock time point as "YYYY-MM-DD HH:MM:SS" in local time.
std::string formatDateTime(std::chrono::system_clock::time_point when) {
  const std::time_t tt = std::chrono::system_clock::to_time_t(when);
  std::tm local{};
  ::localtime_r(&tt, &local);
  char buffer[64];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local);
  return buffer;
}

/// Renders the SYSTEM LOAD AND UPTIME section: load averages (raw and per-CPU
/// normalized), a heuristic severity, running/total tasks, last PID, uptime,
/// idle time and the wall-clock boot estimate. Everything is read-only;
/// unavailable values show "N/A" instead of faked zeros.
void renderSystemLoadSections(std::ostringstream &out,
                              const atm::SystemLoadSnapshot &load,
                              std::uint32_t online_cpus) {
  out << "\n## SYSTEM LOAD AND UPTIME\n\n";
  if (!load.load.readable) {
    out << "Load averages unavailable (/proc/loadavg could not be read).\n";
  } else {
    appendLabeled(out, "Load Average (1 min):",
                  formatLoadAverage(load.load.load1));
    appendLabeled(out, "Load Average (5 min):",
                  formatLoadAverage(load.load.load5));
    appendLabeled(out, "Load Average (15 min):",
                  formatLoadAverage(load.load.load15));
    const std::optional<double> normalized =
        atm::normalizeLoad(load.load.load1, online_cpus);
    if (online_cpus > 0) {
      appendLabeled(out, "Normalized (per CPU):",
                    formatLoadAverage(*normalized) + " / " +
                        std::to_string(online_cpus) + " logical CPU" +
                        (online_cpus == 1 ? "" : "s"));
      const atm::LoadSeverity severity =
          atm::classifyLoadSeverity(*normalized);
      appendLabeled(out, "Load Severity:",
                    std::string(loadSeverityGlyph(severity)) + " " +
                        atm::loadSeverityName(severity));
    } else {
      appendLabeled(out, "Normalized (per CPU):", "N/A (CPU count unknown)");
      appendLabeled(out, "Load Severity:", "N/A");
    }
    appendLabeled(
        out, "Running/Total:",
        load.load.running_total_available
            ? std::to_string(static_cast<std::uint64_t>(load.load.running)) +
                  "/" +
                  std::to_string(static_cast<std::uint64_t>(load.load.total))
            : std::string("N/A"));
    appendLabeled(out, "Last PID:",
                  load.load.last_pid_available
                      ? formatThousands(load.load.last_pid)
                      : std::string("N/A"));
  }

  if (!load.uptime.readable) {
    out << "Uptime unavailable (/proc/uptime could not be read).\n";
  } else {
    appendLabeled(out, "Uptime:",
                  atm::formatUptime(static_cast<std::uint64_t>(
                      load.uptime.uptime_seconds)));
    appendLabeled(out, "Idle time:",
                  atm::formatUptime(static_cast<std::uint64_t>(
                      load.uptime.idle_seconds)));
    appendLabeled(out, "Boot time:",
                  load.boot_time.has_value()
                      ? formatDateTime(*load.boot_time)
                      : std::string("N/A"));
  }
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

/// Converts a CPU time in USER_HZ ticks to a human-readable duration.
std::string formatTicks(std::uint64_t ticks);

/// Renders the process table, sorted by `sort`.
void renderProcessTable(std::ostringstream &out,
                        const std::vector<atm::Process> &processes) {
  out << "\n## PROCESSES\n\n"
      << "    PID  NAME                     CPU        RAM   THR  STATE\n";
  for (const atm::Process &process : processes) {
    out << "  " << formatProcessRow(process) << '\n';
  }
}

/// Renders the aggregated system-wide process statistics.
void renderProcessStats(std::ostringstream &out,
                        const atm::SystemProcessStatistics &stats) {
  out << "\n## Process Statistics\n\n";
  appendLabeled(out, "Total:", std::to_string(stats.total));
  appendLabeled(out, "Running:", std::to_string(stats.running));
  appendLabeled(out, "Sleeping:", std::to_string(stats.sleeping));
  appendLabeled(out, "Disk sleep:", std::to_string(stats.disk_sleep));
  appendLabeled(out, "Stopped:", std::to_string(stats.stopped));
  appendLabeled(out, "Zombie:", std::to_string(stats.zombie));
  appendLabeled(out, "Idle:", std::to_string(stats.idle));
  appendLabeled(out, "Unknown:", std::to_string(stats.unknown));
  appendLabeled(out, "Threads:", std::to_string(stats.total_threads));
  appendLabeled(out, "Aggregate CPU:",
                formatPercent(stats.aggregate_cpu_percent) + "%");
  appendLabeled(out, "CPU user time:",
                formatTicks(stats.total_user_cpu_ticks));
  appendLabeled(out, "CPU system time:",
                formatTicks(stats.total_system_cpu_ticks));
  appendLabeled(out, "Total CPU time:",
                formatTicks(stats.total_cpu_ticks));
  appendLabeled(out, "Resident memory:",
                formatKibibytes(stats.total_rss_kib));
  appendLabeled(out, "Shared memory:",
                formatKibibytes(stats.total_shared_kib));
  appendLabeled(out, "Memory usage:",
                formatPercent(stats.aggregate_memory_percent) + "%");

  std::ostringstream io;
  io << atm::GraphRenderer::formatRate(stats.total_read_rate) << " read, "
     << atm::GraphRenderer::formatRate(stats.total_write_rate) << " write";
  appendLabeled(out, "Process I/O:", io.str());

  std::ostringstream activity;
  activity << "+" << stats.process_creations << " created / -"
           << stats.process_exits << " exited";
  appendLabeled(out, "Activity:", activity.str());
  appendLabeled(out, "Created last scan:",
                std::to_string(stats.process_creations));
  appendLabeled(out, "Exited last scan:",
                std::to_string(stats.process_exits));
  appendLabeled(out, "Creation rate:",
                formatPercent(stats.creation_rate_per_second) + "/s");
  appendLabeled(out, "Exit rate:",
                formatPercent(stats.exit_rate_per_second) + "/s");
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

/// Renders the DISK HEALTH summary table shared by the live view and the 'h'
/// detail screen. Health data lives in DiskHealthMonitor's background cache; a
/// device is only ever shown 'Healthy' when a real read-only health source
/// reported a passing result.
void renderDiskHealthSummary(
    std::ostringstream &out,
    const std::vector<atm::DiskHealthCacheEntry> &entries) {
  if (entries.empty()) {
    out << "No disk health data has been collected yet.\n";
    return;
  }
  out << std::left << std::setw(kHealthDeviceWidth) << "DEVICE" << std::right
      << std::setw(kHealthTypeWidth) << "TYPE"
      << std::setw(kHealthModelWidth) << "MODEL"
      << std::setw(kHealthStatusWidth) << "STATUS"
      << std::setw(kHealthTempWidth) << "TEMP"
      << std::setw(kHealthPowerOnWidth) << "POWER ON"
      << std::setw(kHealthRefreshWidth) << "LAST REFRESH"
      << "  NOTE\n";
  for (const atm::DiskHealthCacheEntry &entry : entries) {
    const atm::DiskHealthSnapshot &s = entry.state;
    std::ostringstream temp;
    temp << (s.temperature_celsius.has_value()
                 ? formatCelsius(*s.temperature_celsius)
                 : "-");
    std::ostringstream power_on;
    power_on << (s.power_on_hours.has_value()
                     ? std::to_string(*s.power_on_hours) + "h"
                     : "-");
    const std::string note =
        entry.last_error_category != atm::HealthErrorCategory::None &&
                !entry.last_error.empty()
            ? entry.last_error
            : (s.ok ? "" : s.detail);
    out << std::left << std::setw(kHealthDeviceWidth)
        << fitTo(s.device_name, kHealthDeviceWidth) << std::right
        << std::setw(kHealthTypeWidth) << atm::diskHealthSourceName(s.source)
        << std::setw(kHealthModelWidth)
        << fitTo(s.model.empty() ? "-" : s.model, kHealthModelWidth)
        << std::setw(kHealthStatusWidth)
        << atm::diskHealthStatusName(s.status)
        << std::setw(kHealthTempWidth) << temp.str()
        << std::setw(kHealthPowerOnWidth) << power_on.str()
        << std::setw(kHealthRefreshWidth)
        << (s.ok ? formatTimestamp(s.refreshed_at) : "-") << "  " << note
        << '\n';
  }
}

/// Renders the DISK HEALTH section of the live view above the process table.
/// The cache is read without blocking and never triggers health reads.
void renderDiskHealthSections(std::ostringstream &out,
                              const atm::DiskHealthMonitor &disk_health) {
  out << "\n## DISK HEALTH\n\n";
  renderDiskHealthSummary(out, disk_health.entries());
  out << "Detailed disk health: press 'h' (then Enter)\n";
}

/// Renders the per-device health detail screen (identification, assessment,
/// temperature/power history, bounded SMART attributes and NVMe counters). All
/// values come from the read-only background refresh; nothing is fabricated.
std::string renderDiskHealthDetailText(
    const atm::DiskHealthCacheEntry &entry) {
  const atm::DiskHealthSnapshot &s = entry.state;
  std::ostringstream out;

  out << "DEVICE\n";
  appendLabeled(out, "Name:", s.device_name.empty() ? "-" : s.device_name);
  appendLabeled(out, "Type:", atm::diskHealthSourceName(s.source));
  appendLabeled(out, "Path:", s.device_path.empty() ? "-" : s.device_path);
  appendLabeled(out, "Model:", s.model.empty() ? "-" : s.model);
  appendLabeled(out, "Firmware:",
                s.firmware_revision.empty() ? "-" : s.firmware_revision);
  appendLabeled(out, "Serial:",
                s.serial_available ? "present (not displayed)" : "not exposed");
  appendLabeled(out, "Status:", atm::diskHealthStatusName(s.status));

  out << "\nHEALTH\n";
  if (s.ok) {
    if (s.smart_self_assessment.has_value()) {
      appendLabeled(out, "SMART health:",
                    *s.smart_self_assessment ? "PASSED" : "FAILED");
    }
    if (!s.smart_overall.empty()) {
      appendLabeled(out, "SMART overall:", s.smart_overall);
    }
    if (s.temperature_celsius.has_value()) {
      appendLabeled(out, "Temperature:", formatCelsius(*s.temperature_celsius));
    }
    if (s.power_on_hours.has_value()) {
      appendLabeled(out, "Powered on:",
                    std::to_string(*s.power_on_hours) + " h");
    }
    if (s.power_cycles.has_value()) {
      appendLabeled(out, "Power cycles:", std::to_string(*s.power_cycles));
    }
    if (s.reallocated_sectors.has_value()) {
      appendLabeled(out, "Reallocated sectors:",
                    std::to_string(*s.reallocated_sectors));
    }
    if (s.current_pending_sectors.has_value()) {
      appendLabeled(out, "Pending sectors:",
                    std::to_string(*s.current_pending_sectors));
    }
    if (s.offline_uncorrectable_sectors.has_value()) {
      appendLabeled(out, "Offline uncorrectable:",
                    std::to_string(*s.offline_uncorrectable_sectors));
    }
    if (s.reported_uncorrectable_errors.has_value()) {
      appendLabeled(out, "Reported uncorrectable errors:",
                    std::to_string(*s.reported_uncorrectable_errors));
    }
    if (s.unsafe_shutdowns.has_value()) {
      appendLabeled(out, "Unsafe shutdowns:",
                    std::to_string(*s.unsafe_shutdowns));
    }
  } else {
    appendLabeled(out, "Readable health data:",
                  s.detail.empty() ? "not available" : ("not available — " +
                                                        s.detail));
  }

  if (!s.attributes.empty()) {
    out << "\nSMART ATTRIBUTES"
        << (s.attributes_truncated ? " (truncated)" : "") << "\n\n"
        << std::left << std::setw(5) << "ID" << std::setw(27) << "NAME"
        << std::right << std::setw(8) << "CURRENT" << std::setw(8) << "WORST"
        << std::setw(8) << "THRESH" << "  RAW\n";
    for (const atm::SmartAttribute &attribute : s.attributes) {
      out << std::right << std::setw(4) << static_cast<unsigned>(attribute.id)
          << "  " << std::left << std::setw(kHealthAttributeNameWidth)
          << fitTo(attribute.name_known ? attribute.name : "(unknown)",
                   kHealthAttributeNameWidth)
          << std::right
          << std::setw(8)
          << std::to_string(attribute.current.value_or(static_cast<std::uint8_t>(0)))
          << std::setw(8)
          << std::to_string(attribute.worst.value_or(static_cast<std::uint8_t>(0)))
          << std::setw(8)
          << std::to_string(attribute.threshold.value_or(static_cast<std::uint8_t>(0)))
          << "  " << attribute.raw_hex
          << (!attribute.unit.empty() && attribute.pretty_value.has_value()
                  ? "  (" + std::to_string(*attribute.pretty_value) + " " +
                        attribute.unit + ")"
                  : "")
          << (attribute.warning ? "  [WARNING]" : "") << '\n';
    }
  }

  const atm::NvmeHealthLog *nvme = s.nvme ? &*s.nvme : nullptr;
  if (nvme != nullptr) {
    out << "\nNVMe SMART / HEALTH LOG\n";
    if (nvme->percentage_used.has_value()) {
      appendLabeled(out, "Life used:", std::to_string(*nvme->percentage_used) +
                                   "%");
    }
    if (nvme->available_spare.has_value()) {
      appendLabeled(out, "Available spare:",
                    std::to_string(*nvme->available_spare) + "%");
    }
    if (nvme->available_spare_threshold.has_value()) {
      appendLabeled(out, "Spare threshold:",
                    std::to_string(*nvme->available_spare_threshold) + "%");
    }
    if (nvme->data_units_read.has_value()) {
      appendLabeled(out, "Data read:",
                    atm::formatBytes(
                        atm::nvmeDataUnitsToBytes(*nvme->data_units_read)));
    }
    if (nvme->data_units_written.has_value()) {
      appendLabeled(out, "Data written:",
                    atm::formatBytes(
                        atm::nvmeDataUnitsToBytes(*nvme->data_units_written)));
    }
    if (nvme->host_read_commands.has_value()) {
      appendLabeled(out, "Host read commands:",
                    std::to_string(*nvme->host_read_commands));
    }
    if (nvme->host_write_commands.has_value()) {
      appendLabeled(out, "Host write commands:",
                    std::to_string(*nvme->host_write_commands));
    }
    if (nvme->controller_busy_time_minutes.has_value()) {
      appendLabeled(out, "Busy time:",
                    std::to_string(*nvme->controller_busy_time_minutes) +
                        " minutes");
    }
    if (nvme->power_cycles.has_value()) {
      appendLabeled(out, "Power cycles:", std::to_string(*nvme->power_cycles));
    }
    if (nvme->power_on_hours.has_value()) {
      appendLabeled(out, "Powered on:", std::to_string(*nvme->power_on_hours) +
                                   " h");
    }
    if (nvme->unsafe_shutdowns.has_value()) {
      appendLabeled(out, "Unsafe shutdowns:",
                    std::to_string(*nvme->unsafe_shutdowns));
    }
    if (nvme->media_and_data_integrity_errors.has_value()) {
      appendLabeled(out, "Media errors:",
                    std::to_string(*nvme->media_and_data_integrity_errors));
    }
    if (nvme->error_information_log_entries.has_value()) {
      appendLabeled(out, "Error log entries:",
                    std::to_string(*nvme->error_information_log_entries));
    }
    if (nvme->warning_temperature_time_minutes.has_value()) {
      appendLabeled(out, "Warning temp time:",
                    std::to_string(*nvme->warning_temperature_time_minutes) +
                        " minutes");
    }
    if (nvme->critical_temperature_time_minutes.has_value()) {
      appendLabeled(out, "Critical temp time:",
                    std::to_string(*nvme->critical_temperature_time_minutes) +
                        " minutes");
    }
    if (nvme->critical_warning.has_value() && *nvme->critical_warning != 0) {
      const std::vector<std::string> warnings =
          atm::nvmeCriticalWarnings(*nvme->critical_warning);
      out << "\nCRITICAL WARNINGS\n";
      for (const std::string &warning : warnings) {
        out << "- " << warning << '\n';
      }
    }
  }

  if (entry.last_error_category != atm::HealthErrorCategory::None) {
    appendLabeled(out, "Last error:",
                  entry.last_error.empty() ? s.detail : entry.last_error);
    appendLabeled(out, "Error type:", atm::healthErrorCategoryName(
                                         entry.last_error_category));
  }
  if (entry.last_success_at.has_value()) {
    appendLabeled(out, "Last good read:",
                  formatTimestamp(*entry.last_success_at));
  }
  return out.str();
}

/// Renders a percentage value or "N/A" when the mount reports none (e.g. there
/// are no inodes to measure). Raw value; the caller pads the column.
std::string formatFsPercent(const std::optional<double> &percent) {
  if (!percent.has_value()) {
    return "N/A";
  }
  std::ostringstream o;
  o << std::fixed << std::setprecision(1) << *percent << '%';
  return o.str();
}

/// Renders a byte count or "-" when unavailable. Values are never zeroed; a
/// missing figure stays visibly unavailable (Step 40).
std::string formatFsBytes(const std::optional<std::uint64_t> &bytes) {
  if (!bytes.has_value()) {
    return "-";
  }
  return atm::formatBytes(*bytes);
}

/// Renders the access (read-only?) column for a mount.
std::string formatFsAccess(atm::MountAccess access) {
  switch (access) {
    case atm::MountAccess::ReadWrite: return "rw";
    case atm::MountAccess::ReadOnly:  return "ro";
    case atm::MountAccess::Unknown:   return "?";
  }
  return "?";
}

/// Renders the FILESYSTEMS table into `out`, honoring `filter`. Returns the
/// number of rows drawn so callers can report "shown of total". Rows are
/// stable: sorted by (mount ID, mount point) so they never reorder each tick.
std::size_t renderFilesystemTable(std::ostringstream &out,
                                  const atm::FilesystemSnapshot &snapshot,
                                  FilesystemFilter filter) {
  std::vector<const atm::FilesystemInfo *> rows;
  for (const atm::FilesystemInfo &info : snapshot.filesystems) {
    if (!filesystemFilterAccepts(filter, info)) {
      continue;
    }
    rows.push_back(&info);
  }
  std::sort(rows.begin(), rows.end(),
            [](const atm::FilesystemInfo *a, const atm::FilesystemInfo *b) {
              if (a->mount.mount_id != b->mount.mount_id) {
                return a->mount.mount_id < b->mount.mount_id;
              }
              return a->mount.mount_point < b->mount.mount_point;
            });

  out << std::left << std::setw(kFsMountWidth) << "MOUNT POINT" << std::right
      << std::setw(kFsTypeWidth) << "TYPE" << std::setw(kFsSourceWidth)
      << "SOURCE" << std::setw(kFsAccessWidth) << "READ-ONLY"
      << std::setw(kFsSizeWidth) << "TOTAL" << std::setw(kFsSizeWidth) << "USED"
      << std::setw(kFsSizeWidth) << "AVAIL" << std::setw(kFsPercentWidth)
      << "USED%" << std::setw(kFsInodeWidth) << "INODES%"
      << "   STATUS\n";
  for (const atm::FilesystemInfo *info : rows) {
    out << std::left << std::setw(kFsMountWidth)
        << fitTo(info->mount.mount_point, kFsMountWidth) << std::right
        << std::setw(kFsTypeWidth)
        << fitTo(info->mount.filesystem_type, kFsTypeWidth)
        << std::setw(kFsSourceWidth)
        << fitTo(info->mount.mount_source, kFsSourceWidth)
        << std::setw(kFsAccessWidth) << formatFsAccess(info->access)
        << std::setw(kFsSizeWidth) << formatFsBytes(info->capacity.total_bytes)
        << std::setw(kFsSizeWidth) << formatFsBytes(info->capacity.used_bytes)
        << std::setw(kFsSizeWidth)
        << formatFsBytes(info->capacity.available_bytes)
        << std::setw(kFsPercentWidth)
        << formatFsPercent(info->capacity.usage_percentage)
        << std::setw(kFsInodeWidth)
        << formatFsPercent(info->capacity.inode_usage_percentage)
        << "   "
        << (info->error == atm::FilesystemError::None
                ? "ok"
                : ((std::string)atm::filesystemErrorName(info->error) + " (" +
                   info->error_detail + ")"))
        << '\n';
  }
  return rows.size();
}

/// Renders the FILESYSTEMS section of the live view above the process table.
/// Reads only the already-captured snapshot; never touches /proc here.
void renderFilesystemSections(std::ostringstream &out,
                              const atm::FilesystemMonitor &monitor,
                              FilesystemFilter filter) {
  out << "\n## FILESYSTEMS\n\n";
  const atm::FilesystemSnapshot &snapshot = monitor.current();
  if (!snapshot.mountinfo_readable) {
    out << "No filesystem data (could not read /proc/self/mountinfo).\n\n";
    out << "Filter: " << filesystemFilterName(filter)
        << " (0 shown of 0 mounts)\n"
        << "Detailed filesystems: press 'w' (then Enter)\n";
    return;
  }
  const std::size_t shown =
      renderFilesystemTable(out, snapshot, filter);
  if (shown == 0) {
    out << "No mounts match this filter.\n";
  }
  out << "Filter: " << filesystemFilterName(filter) << " (" << shown
      << " shown of " << snapshot.filesystems.size() << " mounts)\n"
      << "Detailed filesystems: press 'w' (then Enter)\n";
}

/// Formats inode totals as "TOTAL (used free)" or "-" when the mount exposes
/// no inode accounting (Step 40 never fabricates a zero).
std::string formatFsInodes(const atm::FilesystemCapacity &capacity) {
  if (!capacity.total_inodes.has_value()) {
    return "-";
  }
  std::ostringstream o;
  o << *capacity.total_inodes;
  if (capacity.used_inodes.has_value()) {
    o << " (used " << *capacity.used_inodes;
    if (capacity.free_inodes.has_value()) {
      o << ", free " << *capacity.free_inodes;
    }
    o << ')';
  }
  return o.str();
}

/// Renders the per-mount filesystem detail screen: identification, mount
/// options, capacity and inode figures, read-only state, classification,
/// error state and history graphs (Step 40).
std::string renderFilesystemDetailText(const atm::FilesystemInfo &info,
                                       const atm::FilesystemMonitor &monitor) {
  std::ostringstream out;
  const std::string rule(30, '-');
  out << "MOUNT\n" << rule << "\n";
  appendLabeled(out, "Mount point:",
                info.mount.mount_point.empty() ? "-" : info.mount.mount_point);
  appendLabeled(out, "Mount ID:", std::to_string(info.mount.mount_id));
  appendLabeled(out, "Parent ID:", std::to_string(info.mount.parent_id));
  appendLabeled(out, "Device:",
                std::to_string(info.mount.major) + ":" +
                    std::to_string(info.mount.minor));
  appendLabeled(out, "Root:",
                info.mount.root.empty() ? "-" : info.mount.root);
  appendLabeled(out, "Source:",
                info.mount.mount_source.empty() ? "-" : info.mount.mount_source);
  appendLabeled(out, "Filesystem type:",
                info.mount.filesystem_type.empty() ? "-"
                                                   : info.mount.filesystem_type);
  appendLabeled(out, "Classification:",
                atm::filesystemClassName(info.classification));
  appendLabeled(out, "Access:", atm::mountAccessName(info.access));
  appendLabeled(out, "Mount options:",
                info.mount.mount_options.empty() ? "-" : info.mount.mount_options);
  appendLabeled(out, "Super options:",
                info.mount.super_options.empty() ? "-" : info.mount.super_options);
  if (!info.mount.optional_fields.empty()) {
    std::string joined;
    for (const std::string &field : info.mount.optional_fields) {
      if (!joined.empty()) {
        joined += ", ";
      }
      joined += field;
    }
    appendLabeled(out, "Optional fields:", joined);
  }

  out << "\nCAPACITY\n" << rule << "\n";
  appendLabeled(out, "Total:", formatFsBytes(info.capacity.total_bytes));
  appendLabeled(out, "Used:", formatFsBytes(info.capacity.used_bytes));
  appendLabeled(out, "Free:", formatFsBytes(info.capacity.free_bytes));
  appendLabeled(out, "Available:", formatFsBytes(info.capacity.available_bytes));
  appendLabeled(out, "Used percentage:", formatFsPercent(info.capacity.usage_percentage));
  appendLabeled(out, "Available percentage:", formatFsPercent(info.capacity.available_percentage));
  appendLabeled(out, "Inodes total:", formatFsInodes(info.capacity));
  appendLabeled(out, "Inode usage:", formatFsPercent(info.capacity.inode_usage_percentage));

  out << "\nREFRESH\n" << rule << "\n";
  appendLabeled(out, "Refreshed at:", formatTimestamp(info.refreshed_at));
  if (info.error != atm::FilesystemError::None) {
    appendLabeled(out, "Error:", atm::filesystemErrorName(info.error));
    appendLabeled(out, "Detail:", info.error_detail);
  }

  out << "\nSAMPLE HISTORY\n" << rule << "\n";
  const atm::FilesystemHistory *history = monitor.historyFor(info.identity());
  if (history == nullptr) {
    out << "No history is tracked for this mount.\n";
  } else {
    atm::GraphConfig graph;
    graph.width = 40;
    graph.height = 5;
    graph.dynamic_scale = false;
    if (!history->usage_percent.empty()) {
      out << "\n[USED %]\n"
          << atm::GraphRenderer::renderText(history->usage_percent, graph,
                                            info.mount.mount_point, "%")
          << '\n';
    }
    atm::GraphConfig bytes_graph = graph;
    bytes_graph.dynamic_scale = true;
    if (!history->available_bytes.empty()) {
      out << "\n[AVAILABLE]\n"
          << atm::GraphRenderer::renderText(history->available_bytes, bytes_graph,
                                            info.mount.mount_point, "B")
          << '\n';
    }
    if (!history->inode_usage_percent.empty()) {
      out << "\n[INODES %]\n"
          << atm::GraphRenderer::renderText(history->inode_usage_percent, graph,
                                            info.mount.mount_point, "%")
          << '\n';
    }
  }
  return out.str();
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

/// Formats a CPU time in USER_HZ ticks as a human-readable duration.
/// USER_HZ is 100 clock ticks per second on Linux (see /proc/<pid>/stat).
std::string formatTicks(std::uint64_t ticks) {
  return formatDuration(ticks / 100);
}

/// Human-readable label for the <value> or the `<optional>` value, showing
/// "N/A" when absent.
std::string orNa(const std::string &value) {
  return value.empty() ? std::string("N/A") : value;
}

/// Renders one resource-limit row as "Label: soft / hard", where either side
/// shows "Unlimited" or a formatted value and the whole row is "N/A" when the
/// limit could not be parsed. `as_count` selects a plain count (files,
/// processes, signals, queues) versus a byte-based limit.
void renderLimit(std::ostringstream &out, const std::string &label,
                 const std::optional<atm::ResourceLimit> &limit,
                 bool as_count) {
  if (!limit.has_value()) {
    appendLabeled(out, label + ":", "N/A");
    return;
  }
  const auto side = [as_count](std::optional<std::uint64_t> value,
                               bool unlimited) {
    if (unlimited) {
      return std::string("Unlimited");
    }
    if (value.has_value()) {
      return as_count ? std::to_string(*value) : atm::formatBytes(*value);
    }
    return std::string("N/A");
  };
  appendLabeled(out, label + ":",
                side(limit->soft, limit->soft_unlimited) + " / " +
                    side(limit->hard, limit->hard_unlimited));
}

/// Renders the read-only memory-map section of the process inspector.
/// `/proc/<pid>/maps` is metadata only: virtual address ranges, permission
/// bits, offsets and mapping names — never memory contents.
void renderMemoryMapsSection(std::ostringstream &out,
                             const atm::ProcessMemoryMapsResult &maps,
                             const std::string *path_filter) {
  switch (maps.status) {
    case atm::MemoryMapStatus::Success:
      break;
    case atm::MemoryMapStatus::PermissionDenied:
      out << "Memory maps unavailable: permission denied.\n";
      return;
    case atm::MemoryMapStatus::ProcessNotFound:
    case atm::MemoryMapStatus::IdentityUnknown:
      out << "Process no longer exists.\n";
      return;
    case atm::MemoryMapStatus::ProcessReused:
      out << "The process identity changed (the PID was reused); memory maps "
             "were discarded.\n";
      return;
    case atm::MemoryMapStatus::InvalidPid:
      out << "Invalid PID.\n";
      return;
    case atm::MemoryMapStatus::ReadError:
      out << "Memory maps unavailable: "
          << (maps.errno_value != 0 ? std::strerror(maps.errno_value)
                                    : std::string("read failed"))
          << ".\n";
      return;
  }

  out << "Virtual memory mapping statistics (this is the virtual address space "
         "layout, not physical RAM usage):\n";
  appendLabeled(out, "Mappings:", std::to_string(maps.maps.size()));
  appendLabeled(out, "Total mapped:", atm::formatBytes(maps.total_bytes));
  appendLabeled(out, "Executable mappings:",
                std::to_string(maps.executable_count));
  appendLabeled(out, "Writable mappings:",
                std::to_string(maps.writable_count));
  appendLabeled(out, "File-backed mappings:",
                std::to_string(maps.file_backed_count));
  appendLabeled(out, "Anonymous mappings:",
                std::to_string(maps.anonymous_count));

  if (maps.maps.empty()) {
    out << "\nNo memory mappings available.\n";
    return;
  }
  if (maps.truncated) {
    out << "\nResults truncated: only the first " << maps.maps.size()
        << " of at least " << maps.maps.size() + 1
        << " mappings are shown.\n";
  }
  if (path_filter != nullptr && !path_filter->empty()) {
    out << "\nFiltering by path: \"" << *path_filter << "\" (press 8 to "
           "change, 9 to clear)\n";
  }

  out << "\n";
  out << std::left << std::setw(14) << "Start" << std::setw(15) << "End"
      << std::setw(9) << "Size" << std::setw(6) << "Perm" << std::right
      << std::setw(10) << "Offset" << "    Path\n";
  for (const atm::ProcessMemoryMap &mapping : maps.maps) {
    if (path_filter != nullptr && !path_filter->empty() &&
        mapping.pathname.find(*path_filter) == std::string::npos) {
      continue;
    }
    std::ostringstream start_hex;
    std::ostringstream end_hex;
    std::ostringstream offset_hex;
    start_hex << std::hex << std::uppercase << mapping.start;
    end_hex << std::hex << std::uppercase << mapping.end;
    offset_hex << std::hex << std::uppercase << mapping.offset;
    out << std::left << std::setw(14) << start_hex.str()
        << std::setw(15) << end_hex.str() << std::setw(9)
        << atm::formatBytes(mapping.size()) << std::setw(6)
        << mapping.permissions << std::right << std::setw(10)
        << offset_hex.str() << "    "
        << (mapping.pathname.empty() ? std::string("-")
                                     : mapping.pathname)
        << '\n';
  }
}

/// Renders the read-only network-connections section of the process inspector.
/// Built from /proc/<pid>/fd + /proc/net/* — socket metadata only, never
/// packet contents, and no socket is modified.
void renderNetworkConnectionsSection(
    std::ostringstream &out,
    const atm::ProcessNetworkConnectionsResult &connections) {
  switch (connections.status) {
    case atm::NetworkConnectionsStatus::Success:
      break;
    case atm::NetworkConnectionsStatus::PermissionDenied:
      out << "Network connections unavailable: permission denied.\n";
      return;
    case atm::NetworkConnectionsStatus::ProcessNotFound:
    case atm::NetworkConnectionsStatus::IdentityUnknown:
      out << "Process no longer exists.\n";
      return;
    case atm::NetworkConnectionsStatus::ProcessReused:
      out << "The process identity changed (the PID was reused); network "
             "connections were discarded.\n";
      return;
    case atm::NetworkConnectionsStatus::InvalidPid:
      out << "Invalid PID.\n";
      return;
    case atm::NetworkConnectionsStatus::ReadError:
      out << "Network connections unavailable: "
          << (connections.errno_value != 0
                  ? std::strerror(connections.errno_value)
                  : std::string("read failed"))
          << ".\n";
      return;
  }

  appendLabeled(out, "Total connections:",
                std::to_string(connections.connections.size()));
  appendLabeled(out, "TCP connections:", std::to_string(connections.tcp_count));
  appendLabeled(out, "UDP sockets:", std::to_string(connections.udp_count));
  appendLabeled(out, "Unix sockets:", std::to_string(connections.unix_count));
  appendLabeled(out, "Listening sockets:",
                std::to_string(connections.listening_count));
  appendLabeled(out, "Established connections:",
                std::to_string(connections.established_count));
  out << "Addresses are numeric (no DNS resolution is performed).\n";

  if (connections.connections.empty()) {
    out << "\nNo network connections found.\n";
    return;
  }
  if (connections.truncated) {
    out << "\nResults truncated: only the first "
        << connections.connections.size() << " of at least "
        << connections.connections.size() + 1
        << " connections are shown.\n";
  }

  out << "\n"
         "FD  Protocol  Local Address                Local Port  "
         "Remote Address               Remote Port  State\n";
  for (const atm::ProcessNetworkConnection &conn : connections.connections) {
    out << std::right << std::setw(3) << conn.fd << "  " << std::left
        << std::setw(9) << atm::connectionProtocolName(conn.protocol)
        << std::setw(30) << conn.local_address << std::right
        << std::setw(11) << conn.local_port << "  " << std::left
        << std::setw(31) << conn.remote_address << std::right
        << std::setw(12) << conn.remote_port;

    std::string state;
    if (conn.protocol == atm::ConnectionProtocol::Udp4 ||
        conn.protocol == atm::ConnectionProtocol::Udp6) {
      state = "UNCONNECTED";
    } else if (conn.tcp_state.has_value()) {
      state = atm::tcpStateName(*conn.tcp_state);
    }
    out << "  " << state << '\n';
  }
}

/// Renders the read-only namespaces section of the process inspector. Built
/// from the selected process's /proc/<pid>/ns symlinks — nothing is entered,
/// created, or modified, and no external tool is used.
void renderNamespacesSection(
    std::ostringstream &out,
    const atm::ProcessNamespaceResult &namespaces) {
  switch (namespaces.status) {
    case atm::NamespaceStatus::Success:
      break;
    case atm::NamespaceStatus::PermissionDenied:
      out << "Namespaces unavailable: permission denied.\n";
      return;
    case atm::NamespaceStatus::ProcessNotFound:
    case atm::NamespaceStatus::IdentityUnknown:
      out << "Process no longer exists.\n";
      return;
    case atm::NamespaceStatus::ProcessReused:
      out << "The process identity changed (the PID was reused); namespace "
             "information was discarded.\n";
      return;
    case atm::NamespaceStatus::InvalidPid:
      out << "Invalid PID.\n";
      return;
    case atm::NamespaceStatus::ReadError:
      out << "Namespaces unavailable: "
          << (namespaces.errno_value != 0
                  ? std::strerror(namespaces.errno_value)
                  : std::string("read failed"))
          << ".\n";
      return;
  }

  appendLabeled(out, "Namespaces detected:",
                std::to_string(namespaces.detected_count));
  appendLabeled(out, "Unique namespace IDs:",
                std::to_string(namespaces.unique_id_count));
  out << "Matching namespace IDs mean the entries reference the same "
         "namespace; the view is read-only and does not identify "
         "containers.\n";

  if (namespaces.namespaces.empty()) {
    out << "\nNo namespaces found.\n";
    return;
  }
  if (namespaces.truncated) {
    out << "\nResults truncated: only the first "
        << namespaces.namespaces.size() << " of " << namespaces.detected_count
        << " namespace entries are shown.\n";
  }
  if (namespaces.unavailable_count > 0) {
    out << "\nPartially available: " << namespaces.unavailable_count
        << " namespace entr"
        << (namespaces.unavailable_count == 1 ? "y" : "ies")
        << " could not be read.\n";
  }

  out << "\n"
         "Type                                ID            Target\n";
  for (const atm::ProcessNamespace &ns : namespaces.namespaces) {
    std::string type_label;
    if (ns.type == atm::NamespaceType::Unknown) {
      type_label = "Unknown (" + ns.name + ")";
    } else {
      type_label = atm::namespaceTypeName(ns.type);
      const char *short_name = atm::namespaceTypeShortName(ns.type);
      if (short_name != nullptr && short_name[0] != '\0') {
        type_label += " (" + std::string(short_name) + ")";
      }
    }
    const std::string id_text =
        ns.id.has_value() ? std::to_string(*ns.id) : std::string("N/A");
    const std::string target =
        ns.target.empty() ? "(unavailable)" : ns.target;
    out << std::left << std::setw(34) << type_label << std::right
        << std::setw(14) << id_text << "  " << std::left << target << '\n';
  }
}

/// Formats a CgroupValue that counts bytes: "N/A", "unlimited", or a 1024-base
/// size like "1.6 GB".
std::string formatCgroupBytes(const atm::CgroupValue &value) {
  if (!value.available) {
    return "N/A";
  }
  if (value.unlimited) {
    return "unlimited";
  }
  return atm::formatBytes(value.value);
}

/// Formats a CgroupValue that counts items: "N/A", "unlimited", or a
/// thousand-separated integer.
std::string formatCgroupCount(const atm::CgroupValue &value) {
  if (!value.available) {
    return "N/A";
  }
  if (value.unlimited) {
    return "unlimited";
  }
  return formatThousands(value.value);
}

/// Joins a controller list ("memory pids") or returns "N/A".
std::string joinCgroupControllers(const std::vector<std::string> &controllers) {
  if (controllers.empty()) {
    return "N/A";
  }
  std::string joined;
  for (std::size_t i = 0; i < controllers.size(); ++i) {
    if (i > 0) {
      joined.push_back(' ');
    }
    joined += controllers[i];
  }
  return joined;
}

/// Renders the read-only cgroup view of one process (Step 28). Everything is
/// native: /proc/<pid>/cgroup, /proc/self/mountinfo and the selected cgroup's
/// read-only control/metadata files. Nothing is ever written and no external
/// tool is used. Resource values describe the whole cgroup — which may include
/// other processes and threads — never only this process.
void renderCgroupsSection(std::ostringstream &out,
                          const atm::ProcessCgroupResult &cgroups) {
  switch (cgroups.status) {
    case atm::CgroupStatus::Success:
      break;
    case atm::CgroupStatus::PermissionDenied:
      out << "Cgroup information unavailable: permission denied.\n";
      return;
    case atm::CgroupStatus::ProcessNotFound:
    case atm::CgroupStatus::IdentityUnknown:
      out << "Process no longer exists.\n";
      return;
    case atm::CgroupStatus::ProcessReused:
      out << "The process identity changed (the PID was reused); cgroup "
             "information was discarded.\n";
      return;
    case atm::CgroupStatus::InvalidPid:
      out << "Invalid PID.\n";
      return;
    case atm::CgroupStatus::Unavailable:
      out << "Cgroup information unavailable.\n";
      return;
    case atm::CgroupStatus::MalformedData:
      out << "Cgroup information unavailable: membership data could not be "
             "parsed.\n";
      return;
    case atm::CgroupStatus::ReadError:
      out << "Cgroup information unavailable: "
          << (cgroups.errno_value != 0
                  ? std::strerror(cgroups.errno_value)
                  : std::string("read failed"))
          << ".\n";
      return;
  }

  appendLabeled(out, "Version:", atm::cgroupVersionName(cgroups.version));

  const atm::ProcessCgroupHierarchy *unified = nullptr;
  for (const atm::ProcessCgroupHierarchy &hierarchy : cgroups.hierarchies) {
    if (hierarchy.hierarchy_id == 0) {
      unified = &hierarchy;
      break;
    }
  }

  bool showed_v2 = false;
  if (unified != nullptr) {
    showed_v2 = true;
    appendLabeled(out, "Path:", unified->relative_path.empty()
                                   ? std::string("N/A")
                                   : unified->relative_path);
    if (!unified->resolvable) {
      out << "The cgroup path could not be resolved outside the cgroup mount "
             "(the process may live in another cgroup namespace).\n";
    }
    const atm::ProcessCgroupResources &resources = cgroups.resources;
    appendLabeled(out, "Controllers:",
                  joinCgroupControllers(resources.controllers));
    appendLabeled(out, "Type:", resources.type.empty() ? std::string("N/A")
                                                       : resources.type);

    out << "\nCPU\n";
    out << "  Weight:  " << formatCgroupCount(resources.cpu_weight) << "\n";
    if (resources.cpu_max.available && !resources.cpu_max.unlimited) {
      out << "  Max:     " << formatThousands(resources.cpu_max.quota_usec)
          << " / " << formatThousands(resources.cpu_max.period_usec) << " µs";
      if (resources.cpu_max.period_usec > 0) {
        out << " ("
            << std::setprecision(1) << std::fixed
            << (100.0 * static_cast<double>(resources.cpu_max.quota_usec) /
                static_cast<double>(resources.cpu_max.period_usec))
            << "%)";
      }
      out << "\n";
    } else if (resources.cpu_max.available) {
      out << "  Max:     unlimited\n";
    } else {
      out << "  Max:     N/A\n";
    }

    out << "\nMemory\n";
    out << "  Current: " << formatCgroupBytes(resources.memory_current) << "\n";
    out << "  Max:     " << formatCgroupBytes(resources.memory_max) << "\n";
    out << "  High:    " << formatCgroupBytes(resources.memory_high) << "\n";

    out << "\nProcesses\n";
    out << "  Current: " << formatCgroupCount(resources.pids_current) << "\n";
    out << "  Max:     " << formatCgroupCount(resources.pids_max) << "\n";

    if (unified->resolvable && resources.readable_file_count == 0) {
      out << "\nCgroup resource files were not readable (permission or kernel "
             "configuration).\n";
    } else if (!unified->resolvable) {
      out << "\nCgroup resource values are unavailable: the cgroup path "
             "cannot be resolved.\n";
    }
    out << "\nResource values describe the whole cgroup, which may include "
           "other processes and threads; they are not per-process values.\n";
  }

  // Any additional (typically cgroup v1) hierarchies are listed separately.
  std::vector<const atm::ProcessCgroupHierarchy *> extra;
  for (const atm::ProcessCgroupHierarchy &hierarchy : cgroups.hierarchies) {
    if (hierarchy.hierarchy_id != 0) {
      extra.push_back(&hierarchy);
    }
  }
  if (!extra.empty()) {
    out << "\nThe process also belongs to these cgroup hierarchies:\n\n";
    for (const atm::ProcessCgroupHierarchy *hierarchy : extra) {
      const std::string controllers = joinCgroupControllers(
          hierarchy->controllers);
      out << "  Hierarchy " << hierarchy->hierarchy_id << "  Controllers "
          << controllers << "  Path " << hierarchy->relative_path;
      if (hierarchy->mount_point.empty()) {
        out << "  [mount not found]";
      } else {
        out << "  [mounted at " << hierarchy->mount_point << "]";
      }
      out << "\n";
    }
    if (cgroups.truncated) {
      out << "Results truncated: only the first " << cgroups.hierarchies.size()
          << " records are shown.\n";
    }
    out << "\nFor cgroup v1 each controller hierarchy is separate; resource "
           "control files are only read for the unified cgroup v2 hierarchy.\n";
  }

  if (!showed_v2 && cgroups.hierarchies.empty()) {
    out << "No cgroup membership records were found.\n";
  }
}

/// Renders the read-only environment view of one process (Step 29). The data
/// was already collected and masked by ProcessDetails; this only formats it.
/// Raw secrets never reach this function. `name_filter` (optional) restricts
/// the rows to variables whose name contains the substring — values are never
/// searched. Filtering reuses the inspector's existing Memory Maps filter
/// (keys 8/9), so no second filtering mechanism is introduced.
void renderEnvironmentSection(std::ostringstream &out,
                              const atm::ProcessEnvironmentResult &environment,
                              const std::string *name_filter = nullptr) {
  switch (environment.status) {
    case atm::EnvironmentStatus::Success:
      break;
    case atm::EnvironmentStatus::EmptyEnvironment:
      out << "No environment variables available.\n";
      return;
    case atm::EnvironmentStatus::PermissionDenied:
      out << "Environment unavailable: permission denied.\n";
      return;
    case atm::EnvironmentStatus::ProcessNotFound:
    case atm::EnvironmentStatus::IdentityUnknown:
      out << "Process no longer exists.\n";
      return;
    case atm::EnvironmentStatus::ProcessReused:
      out << "The process identity changed (the PID was reused); environment "
             "data was discarded.\n";
      return;
    case atm::EnvironmentStatus::InvalidPid:
      out << "Invalid PID.\n";
      return;
    case atm::EnvironmentStatus::MalformedData:
      out << "Environment unavailable: the environment data was malformed.\n";
      return;
    case atm::EnvironmentStatus::ReadError:
      out << "Environment unavailable: "
          << (environment.errno_value != 0
                  ? std::strerror(environment.errno_value)
                  : std::string("read failed"))
          << ".\n";
      return;
  }

  appendLabeled(out, "Variables:",
                std::to_string(environment.entries.size()));
  appendLabeled(out, "Total size:",
                formatKibibytes((environment.byte_count + 1023) / 1024));
  appendLabeled(out, "Sensitive variables:",
                std::to_string(environment.sensitive_count));

  bool truncated = environment.size_truncated ||
                   environment.variable_truncated;
  if (truncated) {
    out << "Environment truncated";
    if (environment.size_truncated && environment.variable_truncated) {
      out << " (size and variable count limits).\n";
    } else if (environment.size_truncated) {
      out << " (size limit).\n";
    } else {
      out << " (variable count limit).\n";
    }
  }
  if (environment.duplicate_count > 0) {
    out << environment.duplicate_count
        << " duplicate variable name"
        << (environment.duplicate_count == 1 ? " was" : "s were")
        << " collapsed (the first value was kept).\n";
  }
  if (environment.malformed_count > 0) {
    out << environment.malformed_count
        << " malformed record"
        << (environment.malformed_count == 1 ? " was" : "s were")
        << " skipped.\n";
  }

  if (name_filter != nullptr && !name_filter->empty()) {
    out << "\nFiltering by variable name: \"" << *name_filter
        << "\" (press 8 to change, 9 to clear)\n";
  }

  if (environment.entries.empty()) {
    return;  // Success with zero entries after filtering: nothing to list
  }

  std::size_t name_width = 8;
  for (const atm::ProcessEnvironmentEntry &entry : environment.entries) {
    name_width = std::max(name_width, entry.name.size());
  }
  const std::size_t kMaxNameWidth = 60;
  name_width = std::min(name_width, kMaxNameWidth);

  out << "\n" << std::left << std::setw(name_width) << "Variable"
      << "  Value\n";
  for (const atm::ProcessEnvironmentEntry &entry : environment.entries) {
    if (name_filter != nullptr && !name_filter->empty() &&
        entry.name.find(*name_filter) == std::string::npos) {
      continue;
    }
    std::string name = entry.name;
    if (name.size() > kMaxNameWidth) {
      name.resize(kMaxNameWidth - 1);
      name += "…";
    }
    out << std::left << std::setw(name_width) << name << "  "
        << entry.value << "\n";
  }
  out << "\nPotentially sensitive values are masked (********); sensitivity "
         "detection is a heuristic and may not identify every secret.\n";
}

/// Formats a security context value; a short reason when unavailable.
std::string securityContextString(const std::string &context, bool available,
                                  int errno_value) {
  if (!available) {
    if (errno_value == EACCES || errno_value == EPERM) {
      return std::string("Permission denied");
    }
    if (errno_value == ENOENT || errno_value == ESRCH) {
      return std::string("Process disappeared");
    }
    return std::string("Unavailable");
  }
  if (context.empty()) {
    return std::string("Unavailable");
  }
  return context;
}

/// Formats the login UID status when the value could not be read.
std::string loginUidUnavailableMessage(int errno_value) {
  if (errno_value == EACCES || errno_value == EPERM) {
    return std::string("Permission denied");
  }
  return std::string("Unavailable");
}

/// Renders the Security & Credentials section of the Detailed Process
/// Inspector (Step 30). This is a strictly read-only display of the selected
/// process's credentials and security flags from /proc. Nothing here modifies
/// the process.
void renderSecuritySection(std::ostringstream &out,
                           const atm::ProcessSecurityResult &security) {
  switch (security.status) {
    case atm::SecurityStatus::Success:
      break;
    case atm::SecurityStatus::PermissionDenied:
      out << "Security information unavailable: permission denied.\n";
      return;
    case atm::SecurityStatus::ProcessNotFound:
    case atm::SecurityStatus::IdentityUnknown:
      out << "Process no longer exists.\n";
      return;
    case atm::SecurityStatus::ProcessReused:
      out << "The process identity changed (the PID was reused); security "
             "data was discarded.\n";
      return;
    case atm::SecurityStatus::InvalidPid:
      out << "Invalid PID.\n";
      return;
    case atm::SecurityStatus::ReadError:
      out << "Security information unavailable: "
          << (security.errno_value != 0
                  ? std::strerror(security.errno_value)
                  : std::string("read failed"))
          << ".\n";
      return;
  }

  const atm::ProcessSecurityInfo &info = security.info;

  // Credentials: UID / GID / supplementary groups.
  out << "### Credentials\n\n";
  out << "UID\n";
  appendLabeled(out, "  Real:", info.uid.real.has_value()
                                  ? std::to_string(*info.uid.real)
                                  : std::string("N/A"));
  appendLabeled(out, "  Effective:", info.uid.effective.has_value()
                                       ? std::to_string(*info.uid.effective)
                                       : std::string("N/A"));
  appendLabeled(out, "  Saved:", info.uid.saved.has_value()
                                   ? std::to_string(*info.uid.saved)
                                   : std::string("N/A"));
  appendLabeled(out, "  Filesystem:", info.uid.filesystem.has_value()
                                        ? std::to_string(*info.uid.filesystem)
                                        : std::string("N/A"));

  out << "GID\n";
  appendLabeled(out, "  Real:", info.gid.real.has_value()
                                  ? std::to_string(*info.gid.real)
                                  : std::string("N/A"));
  appendLabeled(out, "  Effective:", info.gid.effective.has_value()
                                       ? std::to_string(*info.gid.effective)
                                       : std::string("N/A"));
  appendLabeled(out, "  Saved:", info.gid.saved.has_value()
                                   ? std::to_string(*info.gid.saved)
                                   : std::string("N/A"));
  appendLabeled(out, "  Filesystem:", info.gid.filesystem.has_value()
                                        ? std::to_string(*info.gid.filesystem)
                                        : std::string("N/A"));

  if (info.groups_available) {
    appendLabeled(out, "Supplementary Groups:",
                  std::to_string(info.supplementary_groups.size()));
    if (!info.supplementary_groups.empty()) {
      std::ostringstream groups;
      bool first = true;
      for (const atm::SupplementaryGroup &group : info.supplementary_groups) {
        if (!first) {
          groups << "  ";
        }
        first = false;
        if (!group.name.empty()) {
          groups << group.name << " (" << group.gid << ")";
        } else {
          groups << group.gid;
        }
      }
      appendLabeled(out, "       Groups:", groups.str());
    }
  } else {
    appendLabeled(out, "Supplementary Groups:", "Unavailable");
  }

  // Capabilities.
  out << "\n### Capabilities\n";
  if (info.capabilities.empty()) {
    out << "Unavailable\n";
  } else {
    for (const atm::CapabilitySet &set : info.capabilities) {
      out << "\n" << atm::capabilitySetTypeName(set.type) << ":\n";
      if (!set.available) {
        out << "Unavailable\n";
        continue;
      }
      std::ostringstream hex_mask;
      hex_mask << "0x" << std::hex << std::setfill('0') << std::setw(16)
               << set.raw_mask;
      out << hex_mask.str() << "\n";
      if (set.decoded_names.empty() && set.unknown_bits.empty()) {
        out << "None\n";
      } else {
        for (const std::string &name : set.decoded_names) {
          out << name << "\n";
        }
        for (const std::uint32_t bit : set.unknown_bits) {
          out << "Unknown capability bit " << bit << "\n";
        }
      }
    }
  }

  // Security flags.
  out << "\n### Security\n";
  appendLabeled(out, "No New Privileges:",
                atm::noNewPrivsName(info.no_new_privs));
  appendLabeled(out, "Seccomp:", atm::seccompModeName(info.seccomp));
  appendLabeled(out, "Seccomp Filters:",
                info.seccomp_filters.has_value()
                    ? std::to_string(*info.seccomp_filters)
                    : std::string("Unavailable"));
  if (info.tracer_pid.has_value() && *info.tracer_pid != 0U) {
    appendLabeled(out, "Tracer PID:", std::to_string(*info.tracer_pid));
  } else {
    appendLabeled(out, "Tracer PID:", "None");
  }
  if (info.umask.has_value()) {
    std::ostringstream umask;
    umask << "0" << std::oct << std::setfill('0') << std::setw(4)
          << *info.umask;
    appendLabeled(out, "Umask:", umask.str());
  }
  if (info.core_dumping.has_value()) {
    appendLabeled(out, "Core Dumping:", *info.core_dumping ? "Yes" : "No");
  }

  // Security context.
  out << "\n### Security Context\n";
  appendLabeled(out, "Current:", securityContextString(
                                     info.security_context,
                                     info.security_context_available,
                                     info.security_context_errno));
  appendLabeled(out, "Exec:", securityContextString(info.exec_context,
                                                    info.exec_context_available,
                                                    info.exec_context_errno));

  // Audit.
  out << "\n### Audit\n";
  if (info.login_uid_available && info.login_uid.has_value()) {
    if (*info.login_uid == atm::kLoginUidUnset) {
      appendLabeled(out, "Login UID:", "Unset");
    } else {
      appendLabeled(out, "Login UID:", std::to_string(*info.login_uid));
    }
  } else {
    appendLabeled(out, "Login UID:",
                  loginUidUnavailableMessage(info.login_uid_errno));
  }

  out << "\nSecurity information is read-only and based on /proc. The "
         "inspector never modifies credentials or capabilities; values are "
         "subject to kernel and permission availability.\n";
}

/// Formats a rate value with a descriptive suffix. Returns "N/A" when the rate
/// is unavailable or negative.
std::string formatIoDetailsRate(double bytes_per_second, bool available) {
  if (!available || bytes_per_second < 0.0) {
    return "N/A";
  }
  return atm::formatNetworkRate(
      static_cast<std::uint64_t>(bytes_per_second + 0.5));
}

/// Renders the I/O Details section of the Detailed Process Inspector (Step 31).
/// This is a strictly read-only display of the selected process's I/O
/// accounting from /proc/<pid>/io. Nothing here modifies the process.
void renderIoDetailsSection(std::ostringstream &out,
                            const atm::ProcessIoDetailsResult &io) {
  switch (io.status) {
    case atm::IoDetailsStatus::Success:
      break;
    case atm::IoDetailsStatus::PermissionDenied:
      out << "I/O information unavailable: permission denied.\n";
      return;
    case atm::IoDetailsStatus::ProcessNotFound:
    case atm::IoDetailsStatus::IdentityUnknown:
      out << "Process no longer exists.\n";
      return;
    case atm::IoDetailsStatus::ProcessReused:
      out << "The process identity changed (the PID was reused); I/O data "
             "was discarded.\n";
      return;
    case atm::IoDetailsStatus::InvalidPid:
      out << "Invalid PID.\n";
      return;
    case atm::IoDetailsStatus::ReadError:
      out << "I/O information unavailable: "
          << (io.errno_value != 0 ? std::strerror(io.errno_value)
                                  : std::string("read failed"))
          << ".\n";
      return;
  }

  const atm::ProcessIoDetailsInfo &info = io.info;

  // Character I/O: bytes counted at the system-call level (rchar / wchar).
  // These include page-cache hits and are NOT direct storage I/O.
  out << "### Character I/O\n\n";
  appendLabeled(out, "Characters Read:",
                info.chars_read.has_value()
                    ? atm::formatBytes(*info.chars_read)
                    : std::string("N/A"));
  appendLabeled(out, "Characters Written:",
                info.chars_written.has_value()
                    ? atm::formatBytes(*info.chars_written)
                    : std::string("N/A"));
  appendLabeled(out, "Read System Calls:",
                info.read_syscalls.has_value()
                    ? formatThousands(*info.read_syscalls)
                    : std::string("N/A"));
  appendLabeled(out, "Write System Calls:",
                info.write_syscalls.has_value()
                    ? formatThousands(*info.write_syscalls)
                    : std::string("N/A"));

  // Storage I/O: bytes actually fetched from / sent to the storage layer.
  // These reflect real disk I/O and are always <= the character I/O counts.
  out << "\n### Storage I/O\n\n";
  appendLabeled(out, "Bytes Read:",
                info.bytes_read.has_value()
                    ? atm::formatBytes(*info.bytes_read)
                    : std::string("N/A"));
  appendLabeled(out, "Bytes Written:",
                info.bytes_written.has_value()
                    ? atm::formatBytes(*info.bytes_written)
                    : std::string("N/A"));
  appendLabeled(out, "Cancelled Write:",
                info.cancelled_write_bytes.has_value()
                    ? atm::formatBytes(*info.cancelled_write_bytes)
                    : std::string("N/A"));

  // Current Activity: derived rates from counter deltas.
  out << "\n### Current Activity\n\n";
  const bool has_chars =
      info.chars_read.has_value() || info.chars_written.has_value();
  const bool has_storage =
      info.bytes_read.has_value() || info.bytes_written.has_value();
  appendLabeled(out, "Char Read Rate:",
                formatIoDetailsRate(info.chars_read_rate, has_chars));
  appendLabeled(out, "Char Write Rate:",
                formatIoDetailsRate(info.chars_written_rate, has_chars));
  appendLabeled(out, "Storage Read Rate:",
                formatIoDetailsRate(info.bytes_read_rate, has_storage));
  appendLabeled(out, "Storage Write Rate:",
                formatIoDetailsRate(info.bytes_written_rate, has_storage));

  out << "\nI/O data is read-only and based on /proc. Character I/O (rchar/"
         "wchar) includes page-cache hits; storage I/O (read_bytes/write_bytes) "
         "reflects actual disk activity. Rates are derived from counter deltas "
         "between consecutive samples of the same process.\n";
}

/// Renders the full detailed breakdown for one process (Step 13). Every
/// field degrades to "N/A" when it could not be read; nothing here re-reads
/// /proc — the data was already collected by ProcessDetails.
std::string renderProcessDetails(const atm::ProcessDetailsInfo &info,
                                 const std::string *section_filter = nullptr) {
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

  out << "\n## Scheduling\n\n";
  appendLabeled(out, "Nice:",
                info.nice_priority.has_value()
                    ? std::to_string(*info.nice_priority)
                    : (info.nice_value.has_value()
                           ? std::to_string(*info.nice_value)
                           : std::string("N/A")));
  appendLabeled(out, "CPU Affinity:",
                info.allowed_cpus.has_value()
                    ? atm::formatCpuList(*info.allowed_cpus)
                    : std::string("N/A"));
  appendLabeled(out, "Allowed CPU Count:",
                info.allowed_cpus.has_value()
                    ? std::to_string(info.allowed_cpus->size())
                    : std::string("N/A"));
  out << "Scheduling edits (keys 6/7) change the running process only and are "
         "never applied without confirmation; they are not persisted.\n";

  out << "\n## I/O Details\n\n";
  if (!info.io_details.has_value()) {
    out << "Loading I/O information...\n";
  } else {
    renderIoDetailsSection(out, *info.io_details);
  }

  out << "\n## Resource Limits\n\n";
  if (info.limits.empty()) {
    appendLabeled(out, "Limits:", "N/A (unavailable)");
  } else {
    renderLimit(out, "Open Files", info.limits.open_files, true);
    renderLimit(out, "Processes", info.limits.max_processes, true);
    renderLimit(out, "Stack Size", info.limits.max_stack_size, false);
    renderLimit(out, "Locked Memory", info.limits.locked_memory, false);
    renderLimit(out, "Address Space", info.limits.address_space, false);
    renderLimit(out, "Core File Size", info.limits.core_file_size, false);
    renderLimit(out, "Pending Signals", info.limits.pending_signals, true);
    renderLimit(out, "POSIX Msg Queues", info.limits.posix_message_queues,
                true);
    renderLimit(out, "Realtime Priority", info.limits.realtime_priority, true);
    renderLimit(out, "Realtime Timeout", info.limits.realtime_timeout, false);
    out << "Resource limits are read-only and not modified by this "
           "application.\n";
  }

  out << "\n## Memory Maps\n\n";
  if (!info.memory_maps.has_value()) {
    out << "Loading memory maps...\n";
  } else {
    renderMemoryMapsSection(out, *info.memory_maps, section_filter);
  }

  out << "\n## Network Connections\n\n";
  if (!info.network_connections.has_value()) {
    out << "Loading network connections...\n";
  } else {
    renderNetworkConnectionsSection(out, *info.network_connections);
  }

  out << "\n## Namespaces\n\n";
  if (!info.namespaces.has_value()) {
    out << "Loading namespaces...\n";
  } else {
    renderNamespacesSection(out, *info.namespaces);
  }

  out << "\n## Cgroups\n\n";
  if (!info.cgroups.has_value()) {
    out << "Loading cgroup information...\n";
  } else {
    renderCgroupsSection(out, *info.cgroups);
  }

  out << "\n## Environment\n\n";
  if (!info.environment.has_value()) {
    out << "Loading environment...\n";
  } else {
    renderEnvironmentSection(out, *info.environment, section_filter);
  }

  out << "\n## Security & Credentials\n\n";
  if (!info.security.has_value()) {
    out << "Loading security information...\n";
  } else {
    renderSecuritySection(out, *info.security);
  }

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
                        const atm::SystemProcessStatistics &stats, atm::ProcessSort sort,
                        ViewMode view, const atm::ProcessTree &tree,
                        const atm::DiskSnapshot &disk,
                        const atm::NetworkSnapshot &network,
                        const atm::GpuSnapshot &gpu,
                        const atm::SensorSnapshot &sensors,
                        const atm::SystemPressureSnapshot &pressure,
                        const atm::SystemLoadSnapshot &load,
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
                         int refresh_interval_ms,
                         const atm::AdvancedCpuSnapshot &cpu_details,
                         const atm::AdvancedMemorySnapshot &memory_details,
                         const atm::DiskHealthMonitor &disk_health,
const atm::FilesystemMonitor &filesystems,
                         FilesystemFilter fs_filter,
                         const atm::NetworkInterfaceMonitor &network_interfaces,
                         const atm::NetworkTrafficHistory &network_traffic_history,
                         const std::string &traffic_selection) {
   if (view == ViewMode::Tree) {
    // The tree view stays deliberately focused on the hierarchy; the storage
    // and network sections are part of the table view.
    return renderTreeFrame(cpu_usage, memory, tree, refresh_interval_ms);
  }

  std::ostringstream out;
  renderHeader(out, cpu_usage, memory);
  renderSystemInfoSections(out, sysinfo);
  renderSystemLoadSections(out, load, onlineLogicalCpuCount(cpu_details));
  renderMemorySections(out, memory, memory_details);
  renderCpuSections(out, cpu_details);
  if (show_history) {
    renderResourceHistorySection(out, history);
  }
  renderPressureSections(out, pressure);
  renderStorageSections(out, disk);
  renderFilesystemSections(out, filesystems, fs_filter);
  renderDiskHealthSections(out, disk_health);
  renderNetworkSections(out, network);
  renderNetworkInterfaceSections(out, network_interfaces);
  renderNetworkTrafficHistorySection(out, network_traffic_history,
                                     traffic_selection);
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
      << "Sort: [1] CPU  [2] Memory  [3] PID  [4] Name  [5] Threads"
         "  [6] Read  [7] Write"
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
        << "Network traffic: press 'c' (then Enter) to cycle the history graph\n"
        << "Disk health: press 'h' (then Enter) to inspect disk health\n"
       << "Filesystems: press 'w' (then Enter) to inspect filesystems\n"
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
                const atm::SystemProcessStatistics &stats, atm::ProcessSort sort,
                ViewMode view, const atm::ProcessTree &tree,
                const atm::DiskSnapshot &disk,
                const atm::NetworkSnapshot &network,
const atm::GpuSnapshot &gpu,
                 const atm::SensorSnapshot &sensors,
                 const atm::SystemPressureSnapshot &pressure,
                 const atm::SystemLoadSnapshot &load,
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
                 int refresh_interval_ms,
                 const atm::AdvancedCpuSnapshot &cpu_details,
                 const atm::AdvancedMemorySnapshot &memory_details,
                 const atm::DiskHealthMonitor &disk_health,
                 const atm::FilesystemMonitor &filesystems,
                 FilesystemFilter fs_filter,
                 const atm::NetworkInterfaceMonitor &network_interfaces,
                 const atm::NetworkTrafficHistory &network_traffic_history,
                 const std::string &traffic_selection) {
  // ANSI "clear entire screen" + "cursor to home" so the multi-line frame
  // refreshes in place instead of scrolling the terminal.
  std::cout << "\033[2J\033[H";
  std::cout << renderFrame(cpu_usage, memory, processes, stats, sort, view, tree,
                           disk, network, gpu, sensors, pressure, load, systemd,
                           startup, sysinfo, history, show_history, alerts,
                           alert_filter, service_search, service_sort,
                           startup_search, startup_sort, packages,
                           refresh_interval_ms, cpu_details, memory_details,
                           disk_health, filesystems, fs_filter, network_interfaces,
                           network_traffic_history, traffic_selection)
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
    SortThreads,
    SortReadRate,
    SortWriteRate,
    ViewList,
    ViewTree,
    Manage,
    InspectProcess,
    InspectNetwork,
    InspectDiskHealth,
    InspectFilesystems,
    InspectGpu,
    InspectSensors,
    InspectSystemd,
    InspectStartup,
    InspectSystemInfo,
    InspectPackages,
    InspectSettings,
    CycleNetworkTraffic,
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
    if (token == "5") return Command::SortThreads;
    if (token == "6") return Command::SortReadRate;
    if (token == "7") return Command::SortWriteRate;
    if (token == "l" || token == "L") return Command::ViewList;
    if (token == "t" || token == "T") return Command::ViewTree;
    if (token == "m" || token == "M") return Command::Manage;
    if (token == "d" || token == "D") return Command::InspectProcess;
    if (token == "i" || token == "I") return Command::InspectNetwork;
    if (token == "h" || token == "H") return Command::InspectDiskHealth;
    if (token == "w" || token == "W") return Command::InspectFilesystems;
    if (token == "g" || token == "G") return Command::InspectGpu;
    if (token == "s" || token == "S") return Command::InspectSensors;
    if (token == "u" || token == "U") return Command::InspectSystemd;
    if (token == "a" || token == "A") return Command::InspectStartup;
    if (token == "y" || token == "Y") return Command::InspectSystemInfo;
    if (token == "p" || token == "P") return Command::InspectPackages;
    if (token == "o" || token == "O") return Command::InspectSettings;
    if (token == "c" || token == "C") return Command::CycleNetworkTraffic;
    if (token == "r" || token == "R") return Command::ToggleHistory;
    if (token == "f" || token == "F") return Command::ToggleAlertFilter;
    if (token == "n" || token == "N") return Command::ToggleNotifications;
    return Command::None;
  }
};

/// "h": disk health summary plus per-device detail. Press [1] to force a fresh
/// background health read (permission/capability errors are highlighted, never
/// retried as healthy), or type a device name for its full report. Health
/// reads always run on the monitor's background worker, never on the UI thread.
void interactDiskHealth(atm::DiskHealthMonitor &disk_health,
                        ConsoleInput &input) {
  for (;;) {
    std::cout << "\033[2J\033[H";
    std::cout << "========================================\n"
                 "ARCH TASK MANAGER — Disk Health Detail\n"
                 "========================================\n\n";
    std::ostringstream summary;
    renderDiskHealthSummary(summary, disk_health.entries());
    std::cout << summary.str() << "\n"
              << "[1] Refresh health data now\n"
              << "[2] Inspect a device\n"
              << "[0] Cancel\n"
              << "> " << std::flush;

    const std::optional<std::string> line = input.readLine();
    if (!line) {
      std::cout << "\nInput cancelled.\n";
      return;
    }
    const std::string choice = trimWhitespace(*line);
    if (choice.empty() || choice == "0") {
      return;
    }
    if (choice == "1") {
      const bool started = disk_health.requestRefresh(true);
      std::cout << "\n"
                << (started ? "Reading health data..."
                            : "Already reading health data...")
                << "\n"
                << std::flush;
      disk_health.waitForIdle();
      continue;
    }
    if (choice == "2") {
      const std::optional<std::string> name_line = input.readLine();
      if (!name_line) {
        std::cout << "\nInput cancelled.\n";
        return;
      }
      const std::string name = trimWhitespace(*name_line);
      if (name.empty()) {
        continue;
      }
      const std::optional<atm::DiskHealthCacheEntry> entry =
          disk_health.entryFor(name);
      if (!entry.has_value()) {
        std::cout << "\nUnknown device '" << name
                  << "'. Press Enter to return.\n"
                  << std::flush;
        static_cast<void>(input.readLine());
        continue;
      }
      std::cout << "\033[2J\033[H";
      std::cout << "========================================\n"
                   "ARCH TASK MANAGER — Disk Health: "
                << name << "\n"
                << "========================================\n\n"
                << renderDiskHealthDetailText(*entry)
                << "\n\nPress Enter to return.\n"
                << std::flush;
      static_cast<void>(input.readLine());
      continue;
    }
    std::cout << "Invalid action. Press Enter to return.\n" << std::flush;
    static_cast<void>(input.readLine());
  }
}

/// "w": freezes the FILESYSTEMS table, cycles the view filter, and inspects
/// one mount point in detail (mount/options, capacity/inodes, history graphs).
/// The snapshot is ~1 s old; reading it is safe because FilesystemMonitor
/// owns all capture state.
void interactFilesystems(const atm::FilesystemMonitor &monitor,
                         FilesystemFilter &filter, ConsoleInput &input) {
  for (;;) {
    std::cout << "\033[2J\033[H";
    std::cout << "========================================\n"
                 "ARCH TASK MANAGER — Filesystems Detail\n"
                 "========================================\n\n";
    const atm::FilesystemSnapshot &snapshot = monitor.current();
    if (!snapshot.mountinfo_readable) {
      std::cout << "No filesystem data (could not read "
                   "/proc/self/mountinfo).\n\n";
      std::cout << "[0] Cancel\n> " << std::flush;
      const std::optional<std::string> cancel = input.readLine();
      if (!cancel || trimWhitespace(*cancel) == "0" ||
          trimWhitespace(*cancel).empty()) {
        return;
      }
      continue;
    }
    std::ostringstream table;
    const std::size_t shown = renderFilesystemTable(table, snapshot, filter);
    if (shown == 0) {
      table << "No mounts match this filter.\n";
    }
    table << "Filter: " << filesystemFilterName(filter) << " (" << shown
          << " shown of " << snapshot.filesystems.size() << " mounts)\n";
    std::cout << table.str() << "\n"
              << "[1] Cycle filter (current: "
              << filesystemFilterName(filter) << ")\n"
              << "[2] Inspect a mount point\n"
              << "[0] Cancel\n"
              << "> " << std::flush;

    const std::optional<std::string> line = input.readLine();
    if (!line) {
      std::cout << "\nInput cancelled.\n";
      return;
    }
    const std::string choice = trimWhitespace(*line);
    if (choice.empty() || choice == "0") {
      return;
    }
    if (choice == "1") {
      filter = nextFilesystemFilter(filter);
      continue;
    }
    if (choice == "2") {
      const std::optional<std::string> mount_line = input.readLine();
      if (!mount_line) {
        std::cout << "\nInput cancelled.\n";
        return;
      }
      const std::string mount = trimWhitespace(*mount_line);
      if (mount.empty()) {
        continue;
      }
      const atm::FilesystemInfo *info = nullptr;
      for (const atm::FilesystemInfo &candidate : snapshot.filesystems) {
        if (candidate.mount.mount_point == mount) {
          info = &candidate;
          break;
        }
      }
      if (info == nullptr) {
        std::cout << "\nUnknown mount point '" << mount
                  << "'. Press Enter to return.\n"
                  << std::flush;
        static_cast<void>(input.readLine());
        continue;
      }
      std::cout << "\033[2J\033[H";
      std::cout << "========================================\n"
                   "ARCH TASK MANAGER — Filesystem: "
                << mount << "\n"
                << "========================================\n\n"
                << renderFilesystemDetailText(*info, monitor)
                << "\n\nPress Enter to return.\n"
                << std::flush;
      static_cast<void>(input.readLine());
      continue;
    }
    std::cout << "Invalid action. Press Enter to return.\n" << std::flush;
    static_cast<void>(input.readLine());
  }
}

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

/// Prints the outcome of a failed scheduling operation with process/action
/// context and the guidance required by the safety policy.
void printSchedulingFailure(const char *action, int pid,
                            const atm::SchedulingResult &result) {
  switch (result.status) {
    case atm::SchedulingStatus::InvalidPid:
      std::cout << atm::schedulingStatusMessage(atm::SchedulingStatus::InvalidPid)
                << '\n';
      break;
    case atm::SchedulingStatus::ProcessNotFound:
      std::cout << "Process does not exist.\n";
      break;
    case atm::SchedulingStatus::PermissionDenied:
      std::cout << "Permission denied.\n"
                   "You do not have permission to change this process's "
                   "scheduling priority.\n";
      break;
    case atm::SchedulingStatus::ProcessReused:
      std::cout << "Cancelled: the process identity changed (the PID was "
                   "reused by a different process). No change was applied.\n";
      break;
    case atm::SchedulingStatus::IdentityUnknown:
      std::cout << "The process no longer exists.\n";
      break;
    case atm::SchedulingStatus::InvalidNice:
      std::cout << "Invalid nice value. Choose a value between "
                << atm::kMinimumNice << " and " << atm::kMaximumNice << ".\n";
      break;
    case atm::SchedulingStatus::InvalidCpu:
      std::cout << "Invalid CPU id.\n";
      break;
    case atm::SchedulingStatus::EmptyAffinity:
      std::cout << "At least one CPU must remain selected.\n";
      break;
    case atm::SchedulingStatus::InvalidCpuset:
      std::cout << "The kernel rejected the CPU set (EINVAL).\n";
      break;
    case atm::SchedulingStatus::Failed:
      std::cout << "Failed to " << action << " process " << pid << ": "
                << std::strerror(result.errno_value) << '\n';
      break;
    case atm::SchedulingStatus::Success:
      break;
  }
}

/// "Change Nice Priority": prompt for a new nice value, show a current-to-new
/// confirmation, and apply via ProcessSchedulingManager with the captured
/// process identity. The identity is re-verified by the manager immediately
/// before setpriority(2), so a reused PID is never modified.
void runChangeNice(atm::ProcessSchedulingManager &scheduling,
                   ConsoleInput &input, int pid, const std::string &name) {
  if (atm::isProtectedPid(pid)) {
    std::cout << "PID " << pid
              << " is protected by the application and its scheduling cannot "
                 "be changed from this interface.\n";
    return;
  }

  const std::optional<atm::ProcessIdentity> identity =
      atm::ProcessIdentity::current(pid);
  if (!identity) {
    std::cout << "Process no longer exists.\n";
    return;
  }
  const std::optional<int> current = scheduling.getNice(pid);
  if (!current) {
    std::cout << "Could not read the process's nice value "
                 "(the process may no longer exist).\n";
    return;
  }

  std::cout << "\nCurrent nice: " << *current << "\n"
            << "Lower values = higher scheduling priority; "
               "higher values = lower priority.\n"
            << "Raising priority beyond your limit requires privileges.\n\n"
            << "Enter new nice value (" << atm::kMinimumNice << " to "
            << atm::kMaximumNice << ", blank to cancel):\n> " << std::flush;

  const std::optional<std::string> line = input.readLine();
  if (!line) {
    std::cout << "\nInput cancelled.\n";
    return;
  }
  const std::string text = trimWhitespace(*line);
  if (text.empty()) {
    std::cout << "Cancelled.\n";
    return;
  }
  int new_nice = 0;
  if (!parseSignedInteger(text, new_nice) || new_nice < atm::kMinimumNice ||
      new_nice > atm::kMaximumNice) {
    std::cout << "Invalid nice value. Choose a value between "
              << atm::kMinimumNice << " and " << atm::kMaximumNice << ".\n";
    return;
  }
  if (new_nice == *current) {
    std::cout << "Nice value is already " << *current << ". No change.\n";
    return;
  }

  std::cout << "\nChange process priority?\n\n"
            << "Process: " << name << "\n"
            << "PID: " << pid << "\n"
            << "Current nice: " << *current << "\n"
            << "New nice: " << new_nice << "\n\n"
            << "This may affect process scheduling.\n\n";
  if (!confirm("Apply this change?", input)) {
    std::cout << "Cancelled. The process's nice value was not changed.\n";
    return;
  }

  const atm::SchedulingResult result = scheduling.setNice(*identity, new_nice);
  if (result.success()) {
    std::cout << "Process " << pid
              << " nice value change applied. The inspector will refresh to "
                 "show the kernel-confirmed value.\n";
  } else {
    printSchedulingFailure("change the nice value of", pid, result);
  }
  std::this_thread::sleep_for(1200ms);
}

/// "Change CPU Affinity": let the user type a new CPU list, validate it against
/// the online CPU range (never allowing an empty mask), show a current-to-new
/// confirmation and apply through ProcessSchedulingManager with the captured
/// identity. Cancelling discards the pending selection.
void runChangeCpuAffinity(atm::ProcessSchedulingManager &scheduling,
                          ConsoleInput &input, int pid,
                          const std::string &name) {
  if (atm::isProtectedPid(pid)) {
    std::cout << "PID " << pid
              << " is protected by the application and its scheduling cannot "
                 "be changed from this interface.\n";
    return;
  }

  const std::optional<atm::ProcessIdentity> identity =
      atm::ProcessIdentity::current(pid);
  if (!identity) {
    std::cout << "Process no longer exists.\n";
    return;
  }
  const std::optional<std::vector<int>> current = scheduling.getCpuAffinity(pid);
  if (!current) {
    std::cout << "Could not read the process's CPU affinity "
                 "(the process may no longer exist).\n";
    return;
  }
  const int cpu_count = atm::ProcessSchedulingManager::systemCpuCount();

  std::cout << "\nCPU Affinity\n"
               "────────────────────────\n"
            << "Available CPUs: 0-" << (cpu_count - 1) << "\n"
            << "Allowed CPUs: " << atm::formatCpuList(*current) << "\n\n"
            << "Enter new CPU list (comma/space separated, or ranges like "
               "0-3; blank to cancel):\n> " << std::flush;

  const std::optional<std::string> line = input.readLine();
  if (!line) {
    std::cout << "\nInput cancelled.\n";
    return;
  }
  const std::string text = trimWhitespace(*line);
  if (text.empty()) {
    std::cout << "Cancelled.\n";
    return;
  }
  const std::optional<std::vector<int>> parsed =
      atm::parseCpuSelection(text, cpu_count);
  if (!parsed) {
    std::cout << "Invalid CPU list. Choose CPUs between 0 and "
              << (cpu_count - 1)
              << " (at least one CPU must remain selected).\n";
    return;
  }
  if (*parsed == *current) {
    std::cout << "CPU affinity is already "
              << atm::formatCpuList(*current) << ". No change.\n";
    return;
  }

  std::cout << "\nChange CPU affinity?\n\n"
            << "Process: " << name << "\n"
            << "PID: " << pid << "\n"
            << "Current CPUs: " << atm::formatCpuList(*current) << "\n"
            << "New CPUs: " << atm::formatCpuList(*parsed) << "\n\n";
  if (!confirm("Apply this change?", input)) {
    std::cout << "Cancelled. The process's CPU affinity was not changed.\n";
    return;
  }

  const atm::SchedulingResult result =
      scheduling.setCpuAffinity(*identity, *parsed);
  if (result.success()) {
    std::cout << "Process " << pid
              << " CPU affinity change applied. The inspector will refresh to "
                 "show the kernel-confirmed value.\n";
  } else {
    printSchedulingFailure("change the CPU affinity of", pid, result);
  }
  std::this_thread::sleep_for(1200ms);
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
            << "    PID  NAME                     CPU        RAM   THR  STATE\n";
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
/// pause, resume, change priority). ProcessActions performs every signal
/// syscall and ProcessSchedulingManager performs the scheduling one — this UI
/// code never re-implements kill(2)/setpriority(2).
void runActionMenu(atm::ProcessActions &actions,
                   atm::ProcessSchedulingManager &scheduling,
                   ConsoleInput &input, int pid, const std::string &name) {
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
  bool scheduling_handled = false;

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

    case 5: {  // Change priority (nice) — scheduling change, identity-checked
               // and confirmed before any setpriority(2).
      scheduling_handled = true;
      const std::optional<atm::ProcessIdentity> identity =
          atm::ProcessIdentity::current(pid);
      if (!identity) {
        std::cout << "Process does not exist. "
                     "It may have disappeared before the operation "
                     "completed.\n";
        break;
      }
      const std::optional<int> current = scheduling.getNice(pid);
      if (!current) {
        std::cout << "Process does not exist. "
                     "It may have disappeared before the operation "
                     "completed.\n";
        break;
      }
      std::cout << "\nCurrent nice: " << *current << "\n"
                << "Lower values = higher scheduling priority; "
                   "higher values = lower.\n"
                << "Raising priority beyond your limit needs privileges.\n\n"
                << "Enter new nice value (" << kMinNice << " to " << kMaxNice
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
      if (priority == *current) {
        std::cout << "Nice value is already " << *current << ". No change.\n";
        break;
      }
      std::cout << "\nChange process priority?\n\n"
                << "Process: " << name << "\n"
                << "PID: " << pid << "\n"
                << "Current nice: " << *current << "\n"
                << "New nice: " << priority << "\n\n"
                << "This may affect process scheduling.\n\n";
      if (!confirm("Apply this change?", input)) {
        std::cout << "Cancelled. The process's nice value was not changed.\n";
        break;
      }
      const atm::SchedulingResult sched =
          scheduling.setNice(*identity, priority);
      if (sched.success()) {
        std::cout << "Process " << pid << " nice value set to " << priority
                  << ".\n";
      } else {
        printSchedulingFailure("set the nice value of", pid, sched);
      }
      break;
    }

    case 6:
      std::cout << "Cancelled.\n";
      break;

    default:
      std::cout << "Invalid action.\n";
      break;
  }

  if (executed && !result.success() && !scheduling_handled) {
    printActionFailure(
        action_text == "1" ? "terminate"
            : action_text == "2" ? "kill"
            : action_text == "3" ? "pause"
            : action_text == "4" ? "resume"
                                 : "operate on",
        pid, result);
  }

  std::cout << "\nRefreshing process list...\n";
  // Give the user a moment to read the outcome before the next frame clears
  // the screen.
  std::this_thread::sleep_for(1500ms);
}

/// Runs one complete "select a process, choose an action" interaction from the
/// flat process-list view.
void runProcessControl(atm::ProcessActions &actions,
                       atm::ProcessSchedulingManager &scheduling,
                       ConsoleInput &input,
                       const std::vector<atm::Process> &listed,
                       atm::ProcessSort sort) {
  showProcessSelection(listed, sort);
  const auto selected =
      selectPid(input, listed, "Select PID (blank to cancel)");
  if (selected.has_value()) {
    runActionMenu(actions, scheduling, input, selected->pid, selected->name);
  }
}

/// Manages a process chosen from the tree view. The tree only identifies the
/// selected PID; validation and the action menu are the same shared flow used
/// by the flat table, and ProcessActions performs the actual syscalls.
void manageFromTree(atm::ProcessActions &actions,
                    atm::ProcessSchedulingManager &scheduling,
                    ConsoleInput &input,
                    const atm::ProcessTree &tree,
                    const std::vector<atm::Process> &listed) {
  std::cout << "\033[2J\033[H";
  std::cout << "========================================\n"
               "ARCH TASK MANAGER — Process Tree Control\n"
               "========================================\n\n"
            << atm::renderProcessTree(tree) << "\n\n";

  const auto selected = selectPid(input, listed, "Enter PID to manage (blank to cancel)");
  if (selected.has_value()) {
    runActionMenu(actions, scheduling, input, selected->pid, selected->name);
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
                           atm::ProcessActions &actions,
                           atm::ProcessSchedulingManager &scheduling,
                           ConsoleInput &input,
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

  // Optional path filter for the read-only memory-maps section; local to this
  // inspection session, applied only to the displayed mapping rows.
  std::string section_filter;

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
      details.forgetBaseline(pid);
      return;
    }

    std::cout << "\033[2J\033[H";
    std::cout << "========================================\n"
                 "ARCH TASK MANAGER — Process Details\n"
                 "========================================\n\n"
              << renderProcessDetails(*info, &section_filter) << "\n\n"
              << "[1] Refresh\n"
                 "[2] Terminate\n"
                 "[3] Kill\n"
                 "[4] Stop (Pause)\n"
                 "[5] Continue (Resume)\n"
                 "[6] Change Nice Priority\n"
                 "[7] Change CPU Affinity\n"
                 "[8] Filter Sections (Memory Maps / Environment)\n"
                 "[9] Clear Section Filter\n"
                 "[E] Export Process Details\n"
                 "[0] Back\n\n"
                 "Select action:\n> "
              << std::flush;

    const std::optional<std::string> action_line = input.readLine();
    if (!action_line) {
      std::cout << "\nInput cancelled.\n";
      details.forgetBaseline(pid);
      return;
    }
    const std::string action_text = trimWhitespace(*action_line);
    if (action_text == "0" || action_text.empty()) {
      details.forgetBaseline(pid);
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
    if (action_text == "6") {
      runChangeNice(scheduling, input, pid, name);
      continue;  // redraw details: the refresh shows the kernel-confirmed value
    }
    if (action_text == "7") {
      runChangeCpuAffinity(scheduling, input, pid, name);
      continue;  // redraw details: the refresh shows the kernel-confirmed value
    }
    if (action_text == "8") {  // filter memory maps (by path) + environment (by name)
      std::cout << "\nFilter memory maps by mapped path and environment by "
                   "variable name (Enter to clear):\n> "
                << std::flush;
      const std::optional<std::string> filter_line = input.readLine();
      section_filter =
          filter_line ? trimWhitespace(*filter_line) : std::string{};
      continue;  // redraw with the new filter
    }
    if (action_text == "9") {  // clear the section filter
      section_filter.clear();
      continue;
    }
    if (action_text == "e" || action_text == "E") {
      if (!info->starttime_ticks.has_value() || *info->starttime_ticks == 0) {
        std::cout << "\nCannot export: process identity is not available.\n"
                  << "Press Enter to continue.\n"
                  << std::flush;
        static_cast<void>(input.readLine());
        continue;
      }
      const atm::ProcessIdentity identity{pid, *info->starttime_ticks};
      const std::string default_name =
          atm::defaultReportFilename(pid, info->name);
      std::cout << "\nExport destination (blank for \"" << default_name
                << "\"):\n> " << std::flush;
      const auto dest_line = input.readLine();
      if (!dest_line) {
        continue;
      }
      std::string destination = trimWhitespace(*dest_line);
      if (destination.empty()) {
        destination = default_name;
      }
      // Overwrite protection: ask before replacing an existing file.
      {
        struct stat st {};
        if (::stat(destination.c_str(), &st) == 0) {
          std::cout << "File \"" << destination
                    << "\" already exists. Overwrite? [y/N]: " << std::flush;
          const auto confirm = input.readLine();
          if (!confirm || trimWhitespace(*confirm) != "y") {
            std::cout << "\nExport cancelled.\nPress Enter to continue.\n"
                      << std::flush;
            static_cast<void>(input.readLine());
            continue;
          }
        }
      }
      const auto result = atm::exportProcessReport(destination, *info, identity);
      if (result.status == atm::ReportStatus::Success ||
          result.status == atm::ReportStatus::SuccessProcessGone) {
        std::cout << "\nReport written: " << result.path << " ("
                  << result.bytes << " bytes)\n";
        if (result.status == atm::ReportStatus::SuccessProcessGone) {
          std::cout << "(The process exited during export; some sections may "
                       "be incomplete.)\n";
        }
      } else {
        std::cout << "\nFailed to write process report: "
                  << atm::reportStatusMessage(result.status) << "\n";
      }
      std::cout << "Press Enter to continue.\n" << std::flush;
      static_cast<void>(input.readLine());
      continue;
    }
    std::cout << "Invalid action.\n";
  }
}

/// Builds the full per-interface page: link metadata from sysfs, addresses,
/// wireless status, the traffic detail (shared with the NETWORK table) and the
/// bounded RX/TX rate histories collected by NetworkInterfaceMonitor.
std::string buildNetworkInterfacePage(
    const atm::NetworkInterfaceSnapshot &snapshot,
    const atm::NetworkInterfaceMonitor &monitor,
    const atm::NetworkInterfaceStats &traffic) {
  const auto found = std::find_if(
      snapshot.interfaces.begin(), snapshot.interfaces.end(),
      [&](const atm::NetworkInterfaceInfo &info) {
        return info.name == traffic.name;
      });
  if (found == snapshot.interfaces.end()) {
    return "";  // caller prints the "not found" message
  }

  std::ostringstream out;
  const atm::NetworkInterfaceInfo &info = *found;
  const std::string identity = info.identity();

  out << "Interface: " << info.name << "    (identity " << identity << ")\n\n";
  appendLabeled(out, "  Type:", atm::networkInterfaceTypeName(info.type));
  appendLabeled(out, "  Ifindex:",
                info.link.ifindex.has_value() ? std::to_string(*info.link.ifindex)
                                               : "unavailable");
  appendLabeled(out, "  Admin:",
                info.link.admin_up ? "up" : "down");
  appendLabeled(out, "  State:", info.link.operstate.value_or("unknown"));
  appendLabeled(out, "  Carrier:", atm::formatNetworkCarrier(info.link.carrier));
  if (info.link.mac_address.has_value()) {
    appendLabeled(out, "  MAC address:", *info.link.mac_address);
  }
  if (info.link.mtu.has_value()) {
    appendLabeled(out, "  MTU:", std::to_string(*info.link.mtu));
  }
  appendLabeled(out, "  Link speed:", atm::formatNetworkSpeed(info.link.speed_mbps));
  appendLabeled(out, "  Duplex:", atm::formatNetworkDuplex(info.link.duplex));
  if (info.link.flags.has_value()) {
    appendLabeled(out, "  Flags:",
                  atm::formatInterfaceFlagsRaw(info.link.flags) + " " +
                      atm::formatInterfaceFlagNames(*info.link.flags));
  }
  if (!info.error.empty()) {
    appendLabeled(out, "  Note:", info.error);
  }

  out << "\nAddresses\n";
  if (info.addresses.empty()) {
    out << "  none\n";
  }
  for (const atm::NetworkAddressInfo &address : info.addresses) {
    std::ostringstream line;
    line << "  " << address.address;
    if (address.prefix_length.has_value()) {
      line << "/" << *address.prefix_length;
    }
    if (address.netmask.has_value()) {
      line << "  (netmask " << *address.netmask << ")";
    }
    if (address.broadcast.has_value()) {
      line << "  broadcast " << *address.broadcast;
    }
    out << line.str() << '\n';
  }

  out << "\nWireless\n";
  if (!info.wireless.present) {
    out << "  not a wireless interface\n";
  } else {
    std::ostringstream line;
    line << "  link ";
    if (info.wireless.link.has_value()) {
      line << *info.wireless.link << "%";
    } else {
      line << "N/A";
    }
    if (info.wireless.level.has_value()) {
      line << "  signal " << *info.wireless.level << " dBm";
    }
    if (info.wireless.noise.has_value()) {
      line << "  noise " << *info.wireless.noise << " dBm";
    }
    out << line.str() << '\n';
  }

  out << "\nTraffic\n" << buildInterfaceDetail(traffic);

  atm::GraphConfig rate;
  rate.width = 40;
  rate.height = 6;
  rate.dynamic_scale = true;

  const atm::InterfaceHistory *history = monitor.historyFor(identity);
  if (history != nullptr && !history->rx_bytes_per_second.empty()) {
    out << "\nRX rate history\n"
        << atm::GraphRenderer::renderText(history->rx_bytes_per_second, rate,
                                          "RX rate", "B/s")
        << '\n'
        << "\nTX rate history\n"
        << atm::GraphRenderer::renderText(history->tx_bytes_per_second, rate,
                                          "TX rate", "B/s")
        << '\n';
  } else {
    out << "\nRate history: not available (no measured traffic yet).\n";
  }
  return out.str();
}

/// "i": shows the network tables frozen and inspects one interface in detail.
/// The snapshots are ~1 s old; reading them is safe because NetworkMonitor and
/// NetworkInterfaceMonitor own all the counter/discovery state.
void interactNetworkDetail(const atm::NetworkSnapshot &network,
                           const atm::NetworkInterfaceMonitor &interfaces,
                           ConsoleInput &input) {
  std::cout << "\033[2J\033[H";
  const atm::NetworkInterfaceSnapshot &iface_snapshot = interfaces.current();
  std::cout << "========================================\n"
               "ARCH TASK MANAGER — Network Interface Detail\n"
               "========================================\n\n"
            << renderNetworkInterfaceTableText(iface_snapshot) << "\n\n"
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

  const std::string page = buildNetworkInterfacePage(iface_snapshot, interfaces,
                                                     *found);
  if (page.empty()) {
    std::cout << "Interface does not exist (not found in the current "
                 "interface list).\n";
    return;
  }
  std::cout << '\n' << page
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

/// Edits the XDG desktop autostart preference. The persisted setting holds the
/// user's intent; the desktop entry on disk is applied immediately (enable or
/// disable). When the filesystem operation fails, a clear error is shown and
/// the setting is kept so a later startup can retry the repair automatically.
void editStartup(atm::cfg::SettingsManager &settings,
                 atm::AppAutostartManager &autostart, ConsoleInput &input) {
  atm::cfg::AppSettings next = settings.settings();
  std::cout << "\n--- Startup ---\n"
            << "  Startup autostart: "
            << (autostart.isEnabled() ? "Enabled" : "Disabled") << "\n";
  bool desired = next.general.autostart_enabled;
  if (promptBool(input,
                 "Start Arch Task Manager automatically when you log in",
                 desired, desired)) {
    next.general.autostart_enabled = desired;
    settings.updateSettings(next);
    const atm::AutostartResult result =
        desired ? autostart.enable() : autostart.disable();
    if (result.ok) {
      std::cout << "\nStartup autostart: "
                << (autostart.isEnabled() ? "Enabled" : "Disabled") << "\n";
    } else {
      std::cout << "\nUnable to " << (desired ? "enable" : "disable")
                << " startup:\n"
                << result.message << "\n";
    }
  }
}

/// Blocking settings page. Editing happens in memory; changes are validated by
/// the SettingsManager, applied to the running components, and saved when the
/// user leaves the page. All changes apply without a restart.
void interactSettings(atm::cfg::SettingsManager &settings,
                      int &refresh_interval_ms,
                      atm::HistoryManager &history,
                      atm::AlertManager &alerts,
                      atm::NotificationManager &notifications,
                      atm::AppAutostartManager &autostart,
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
              << "\nStartup:\n"
              << "  Startup autostart: "
              << (autostart.isEnabled() ? "Enabled" : "Disabled") << "\n"
              << "\nAutomatic package installation is disabled. Updates always "
                 "require\nexplicit user confirmation.\n"
              << "\nManagement:\n"
              << "[1] Edit General\n"
              << "[2] Edit History\n"
              << "[3] Edit Alerts\n"
              << "[4] Edit Notifications\n"
              << "[5] Edit Packages\n"
              << "[6] Edit Startup\n"
              << "[7] Reset to Defaults\n"
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
      editStartup(settings, autostart, input);
    } else if (choice == "7") {
      if (confirm("Reset all settings to defaults?", input)) {
        // The autostart preference is a setting: resetting it to false must
        // also remove the application's desktop entry (only its own entry).
        settings.resetToDefaults();
        if (autostart.isEnabled()) {
          const atm::AutostartResult result = autostart.disable();
          if (!result.ok) {
            std::cout << "\nUnable to disable startup:\n" << result.message
                      << "\n";
          }
        }
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

  atm::AdvancedCpuMonitor cpu_details;
  atm::AdvancedMemoryMonitor memory_details;
  atm::ProcessMonitor process_monitor;
  atm::ProcessStatisticsAggregator process_statistics;
  atm::DiskMonitor disk_monitor;
  atm::DiskHealthMonitor disk_health;
  atm::FilesystemMonitor filesystem_monitor;
  atm::NetworkInterfaceMonitor network_interface_details;
  atm::NetworkTrafficHistory network_traffic_history;
  atm::NetworkMonitor network_monitor;
  atm::GpuMonitor gpu_monitor;
  atm::SensorMonitor sensor_monitor;
  atm::SystemPressureMonitor pressure_monitor;
  atm::SystemLoadMonitor load_monitor;
  atm::SystemdManager systemd_manager;
  atm::StartupManager startup_manager;
  atm::SystemInfoProvider system_info;
  atm::ProcessActions actions;
  atm::ProcessSchedulingManager scheduling;
  atm::ProcessDetails process_details;
  atm::HistoryManager history(
      static_cast<std::size_t>(settings.settings().history.max_samples));
  atm::AlertManager alerts;
  atm::NotificationManager notifications;
  atm::PackageManager packages;
  atm::PackageTransaction package_transaction;
  atm::AppAutostartManager autostart;
  ConsoleInput input;

  // Apply the loaded settings to the runtime components and to the main loop.
  atm::cfg::applySettingsToRuntime(settings.settings(), refresh_interval_ms,
                                   history, alerts, notifications);
  filesystem_monitor.setHistoryMaxSamples(
      static_cast<std::size_t>(settings.settings().history.max_samples));
  network_interface_details.setHistoryMaxSamples(
      static_cast<std::size_t>(settings.settings().history.max_samples));
  network_traffic_history.setHistoryMaxSamples(
      static_cast<std::size_t>(settings.settings().history.max_samples));

  // Forward alert state transitions to desktop notifications.
  g_notification_manager = &notifications;
  alerts.setNotificationSink(&onAlertEvent);

  // Reconcile the persisted autostart preference with the desktop entry on
  // disk. This only repairs/removes arch-task-manager.desktop in the user's
  // autostart directory; it never touches system-wide startup and a failure
  // here never prevents the application from launching. The outcome is logged.
  static_cast<void>(autostart.synchronizeWithSettings(
      settings.settings().general.autostart_enabled));

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
  FilesystemFilter fs_filter = FilesystemFilter::Physical;
  std::string network_traffic_selection =
      std::string(atm::kNetworkTrafficAllIdentity);

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
  static_cast<void>(cpu_details.read());
  static_cast<void>(process_monitor.read(0));
  static_cast<void>(disk_monitor.read());
  static_cast<void>(network_monitor.read());
  static_cast<void>(gpu_monitor.read());
  sensor_monitor.discover();
  static_cast<void>(sensor_monitor.read());
  systemd_manager.discover();
  static_cast<void>(pressure_monitor.read());
  static_cast<void>(load_monitor.read());

  // Disk health uses the same whole-disks discovered from /sys/block as the
  // storage section, then starts its first read-only health pass in the
  // background immediately so the initial frame shows real status. Health
  // reads never run on the monitoring tick: they are (re)triggered on demand
  // from the 'h' page only.
  disk_health.setDevices(atm::listWholeDisks());
  static_cast<void>(disk_health.requestRefresh(true));

  // First filesystem capture: parsed /proc/self/mountinfo + statvfs for every
  // mount. Must happen before the first frame so the FILESYSTEMS section and
  // history have real data.
  static_cast<void>(filesystem_monitor.read());

  std::this_thread::sleep_for(std::chrono::milliseconds(refresh_interval_ms));

  atm::AdvancedCpuSnapshot cpu = cpu_details.read();
  if (!cpu.proc_stat_readable) {
    std::cerr << "ERROR: could not read CPU usage from /proc/stat\n";
    return EXIT_FAILURE;
  }

  atm::AdvancedMemorySnapshot memory = memory_details.read();
  if (!memory.meminfo_readable) {
    std::cerr << "Error: Unable to read /proc/meminfo\n";
    return EXIT_FAILURE;
  }
  const atm::MemoryInfo first_memory = memory.toMemoryInfo();

  const atm::DiskSnapshot first_disk = disk_monitor.read();
  const atm::NetworkSnapshot first_network = network_monitor.read();
  static_cast<void>(network_interface_details.read(first_network));
  network_traffic_history.record(network_interface_details.current());
  const atm::GpuSnapshot first_gpu = gpu_monitor.read();
  const atm::SensorSnapshot first_sensors = sensor_monitor.read();
  const atm::SystemPressureSnapshot first_pressure = pressure_monitor.read();
  const atm::SystemLoadSnapshot first_load = load_monitor.read();
  const atm::SystemdSnapshot first_systemd = systemd_manager.read();
  const atm::StartupSnapshot first_startup = startup_manager.read();
  system_info.load(first_gpu);
  atm::SystemInfo sysinfo = system_info.read();
  auto snapshot = process_monitor.read(first_memory.total);
  atm::sortProcesses(snapshot.processes, sort);
  atm::ProcessTree tree = atm::buildProcessTree(snapshot.processes);
  const atm::SystemProcessStatistics proc_stats =
      process_statistics.update(snapshot);

  // Seed the resource history with the first sample so graphs show data from
  // the very first frame.
  {
    std::vector<std::pair<std::string, double>> gpu_utils;
    std::vector<std::pair<std::string, double>> gpu_vrams;
    std::vector<std::pair<std::string, double>> temps;
    buildGpuMetricVectors(first_gpu, gpu_utils, gpu_vrams);
    buildTemperatureVector(first_sensors, temps);
    history.update(aggregateCpuPercent(cpu), first_memory.usagePercent(),
                   static_cast<double>(first_memory.used()),
                   static_cast<double>(first_memory.available),
                   first_memory.swapUsagePercent(),
                   static_cast<double>(first_disk.total_read_bytes_per_second),
                   static_cast<double>(first_disk.total_write_bytes_per_second),
                   static_cast<double>(first_network.total_rx_bytes_per_second),
                   static_cast<double>(first_network.total_tx_bytes_per_second),
                   gpu_utils, gpu_vrams, temps);
    history.updateCpuHistories(cpuHistoryVector(cpu));
    history.updateAdvancedMemory(buildAdvancedMemoryMetrics(memory));
    history.updatePressure(buildPressureMetrics(first_pressure));
    history.updateLoad(buildLoadMetrics(first_load, onlineLogicalCpuCount(cpu)));
    history.updateProcessStats(proc_stats);
    updateAlerts(alerts, aggregateCpuPercent(cpu), first_memory, first_disk,
                 first_network, first_gpu, first_sensors, history);
  }

  renderView(aggregateCpuPercent(cpu), first_memory, snapshot.processes,
             proc_stats, sort, view, tree, first_disk, first_network, first_gpu,
             first_sensors, first_pressure, first_load, first_systemd,
             first_startup, sysinfo, history, show_history, alerts, alert_filter,
             service_search, service_sort, startup_search, startup_sort,
             packages, refresh_interval_ms, cpu, memory, disk_health,
             filesystem_monitor, fs_filter, network_interface_details,
             network_traffic_history, network_traffic_selection);

  atm::NetworkSnapshot network = first_network;
  atm::GpuSnapshot gpu = first_gpu;
  atm::SensorSnapshot sensors = first_sensors;
  atm::SystemPressureSnapshot pressure = first_pressure;
  atm::SystemLoadSnapshot load = first_load;
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
      case ConsoleInput::Command::SortThreads:
        sort = atm::ProcessSort::Threads;
        break;
      case ConsoleInput::Command::SortReadRate:
        sort = atm::ProcessSort::ReadRate;
        break;
      case ConsoleInput::Command::SortWriteRate:
        sort = atm::ProcessSort::WriteRate;
        break;
      case ConsoleInput::Command::ViewList:
        view = ViewMode::List;
        break;
      case ConsoleInput::Command::ViewTree:
        view = ViewMode::Tree;
        break;
      case ConsoleInput::Command::Manage:
        if (view == ViewMode::Tree) {
          manageFromTree(actions, scheduling, input, tree, snapshot.processes);
        } else {
          runProcessControl(actions, scheduling, input, snapshot.processes, sort);
        }
        // Refresh immediately so the effect of the action is visible without
        // waiting for the next 1 s tick.
        {
          cpu = cpu_details.read();
          memory = memory_details.read();
          if (cpu.proc_stat_readable && memory.meminfo_readable) {
            const atm::MemoryInfo mem_info = memory.toMemoryInfo();
            const atm::DiskSnapshot disk = disk_monitor.read();
            const atm::NetworkSnapshot network = network_monitor.read();
            static_cast<void>(network_interface_details.read(network));
            network_traffic_history.record(network_interface_details.current());
            static_cast<void>(filesystem_monitor.read());
            gpu = gpu_monitor.read();
            sensors = sensor_monitor.read();
            systemd = systemd_manager.read();
            startup = startup_manager.read();
            snapshot = process_monitor.read(mem_info.total);
            atm::sortProcesses(snapshot.processes, sort);
            tree = atm::buildProcessTree(snapshot.processes);
            const atm::SystemProcessStatistics proc_stats =
                process_statistics.update(snapshot);
            {
              std::vector<std::pair<std::string, double>> gpu_utils;
              std::vector<std::pair<std::string, double>> gpu_vrams;
              std::vector<std::pair<std::string, double>> temps;
              buildGpuMetricVectors(gpu, gpu_utils, gpu_vrams);
              buildTemperatureVector(sensors, temps);
              history.update(aggregateCpuPercent(cpu), mem_info.usagePercent(),
                             static_cast<double>(mem_info.used()),
                             static_cast<double>(mem_info.available),
                             mem_info.swapUsagePercent(),
                             static_cast<double>(disk.total_read_bytes_per_second),
                             static_cast<double>(disk.total_write_bytes_per_second),
                             static_cast<double>(network.total_rx_bytes_per_second),
                             static_cast<double>(network.total_tx_bytes_per_second),
                             gpu_utils, gpu_vrams, temps);
              history.updateCpuHistories(cpuHistoryVector(cpu));
              history.updateAdvancedMemory(buildAdvancedMemoryMetrics(memory));
              history.updateProcessStats(proc_stats);
              updateAlerts(alerts, aggregateCpuPercent(cpu), mem_info, disk,
                           network, gpu, sensors, history);
            }
            renderView(aggregateCpuPercent(cpu), mem_info, snapshot.processes,
                       proc_stats, sort, view, tree, disk, network, gpu,
                       sensors, pressure, load, systemd, startup, sysinfo,
                       history, show_history, alerts, alert_filter,
                       service_search, service_sort, startup_search,
                       startup_sort, packages, refresh_interval_ms, cpu,
                       memory, disk_health, filesystem_monitor, fs_filter,
                       network_interface_details, network_traffic_history,
                       network_traffic_selection);
          }
        }
        continue;
      case ConsoleInput::Command::InspectProcess:
        if (view == ViewMode::List) {
          const std::uint64_t total_kib =
              memory.mem_total.has_value() ? *memory.mem_total / 1024 : 0;
          interactProcessDetail(process_details, actions, scheduling, input,
                                snapshot.processes, sort, total_kib);
        }
        break;
      case ConsoleInput::Command::InspectNetwork:
        if (view == ViewMode::List) {
          interactNetworkDetail(network, network_interface_details, input);
        }
        break;
      case ConsoleInput::Command::InspectDiskHealth:
        if (view == ViewMode::List) {
          interactDiskHealth(disk_health, input);
        }
        break;
      case ConsoleInput::Command::InspectFilesystems:
        if (view == ViewMode::List) {
          interactFilesystems(filesystem_monitor, fs_filter, input);
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
                           notifications, autostart, input);
          filesystem_monitor.setHistoryMaxSamples(
              static_cast<std::size_t>(settings.settings().history.max_samples));
          network_interface_details.setHistoryMaxSamples(
              static_cast<std::size_t>(settings.settings().history.max_samples));
          network_traffic_history.setHistoryMaxSamples(
              static_cast<std::size_t>(settings.settings().history.max_samples));
        }
        break;
      case ConsoleInput::Command::CycleNetworkTraffic:
        if (view == ViewMode::List) {
          // Cycle through the aggregate and each tracked interface in stable
          // discovery order via the component's selection list. A selection
          // whose interface vanished is pruned from the list; cycling always
          // starts/ends at "all".
          const std::vector<std::string> selectable =
              network_traffic_history.selectableIdentities();
          const auto current = std::find(selectable.begin(), selectable.end(),
                                         network_traffic_selection);
          if (current != selectable.end() &&
              std::next(current) != selectable.end()) {
            network_traffic_selection = *std::next(current);
          } else {
            network_traffic_selection =
                std::string(atm::kNetworkTrafficAllIdentity);
          }
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
          filesystem_monitor.setHistoryMaxSamples(
              static_cast<std::size_t>(settings.settings().history.max_samples));
          network_interface_details.setHistoryMaxSamples(
              static_cast<std::size_t>(settings.settings().history.max_samples));
          network_traffic_history.setHistoryMaxSamples(
              static_cast<std::size_t>(settings.settings().history.max_samples));
        }
        break;
      case ConsoleInput::Command::None:
        break;
    }

    cpu = cpu_details.read();
    if (!cpu.proc_stat_readable) {
      std::cerr << "\nERROR: could not read CPU usage from /proc/stat\n";
      return EXIT_FAILURE;
    }

    memory = memory_details.read();
    if (!memory.meminfo_readable) {
      std::cerr << "\nError: Unable to read /proc/meminfo\n";
      return EXIT_FAILURE;
    }
    const atm::MemoryInfo mem_info = memory.toMemoryInfo();

    snapshot = process_monitor.read(mem_info.total);
    atm::sortProcesses(snapshot.processes, sort);
    tree = atm::buildProcessTree(snapshot.processes);
    const atm::SystemProcessStatistics proc_stats =
        process_statistics.update(snapshot);
    const atm::DiskSnapshot disk = disk_monitor.read();
    network = network_monitor.read();
    static_cast<void>(network_interface_details.read(network));
    network_traffic_history.record(network_interface_details.current());
    static_cast<void>(filesystem_monitor.read());
    gpu = gpu_monitor.read();
    sensors = sensor_monitor.read();
    pressure = pressure_monitor.read();
    load = load_monitor.read();
    systemd = systemd_manager.read();
    startup = startup_manager.read();

    {
      std::vector<std::pair<std::string, double>> gpu_utils;
      std::vector<std::pair<std::string, double>> gpu_vrams;
      std::vector<std::pair<std::string, double>> temps;
      buildGpuMetricVectors(gpu, gpu_utils, gpu_vrams);
      buildTemperatureVector(sensors, temps);
      history.update(aggregateCpuPercent(cpu), mem_info.usagePercent(),
                     static_cast<double>(mem_info.used()),
                     static_cast<double>(mem_info.available),
                     mem_info.swapUsagePercent(),
                     static_cast<double>(disk.total_read_bytes_per_second),
                     static_cast<double>(disk.total_write_bytes_per_second),
                     static_cast<double>(network.total_rx_bytes_per_second),
                     static_cast<double>(network.total_tx_bytes_per_second),
                     gpu_utils, gpu_vrams, temps);
      history.updateCpuHistories(cpuHistoryVector(cpu));
      history.updateAdvancedMemory(buildAdvancedMemoryMetrics(memory));
      history.updatePressure(buildPressureMetrics(pressure));
      history.updateLoad(buildLoadMetrics(load, onlineLogicalCpuCount(cpu)));
      history.updateProcessStats(proc_stats);
      updateAlerts(alerts, aggregateCpuPercent(cpu), mem_info, disk, network,
                   gpu, sensors, history);
    }

    renderView(aggregateCpuPercent(cpu), mem_info, snapshot.processes, proc_stats,
               sort, view, tree, disk, network, gpu, sensors, pressure, load,
               systemd, startup, sysinfo, history, show_history, alerts,
               alert_filter, service_search, service_sort, startup_search,
               startup_sort, packages, refresh_interval_ms, cpu, memory,
               disk_health, filesystem_monitor, fs_filter, network_interface_details,
               network_traffic_history, network_traffic_selection);
  }

  // Clean shutdown: persist any pending settings changes.
  if (settings.isDirty()) {
    settings.save();
  }
}
