#include "gpu_monitor.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace atm {

namespace {

namespace fs = std::filesystem;

constexpr std::uint64_t kMhzInHz = 1'000'000;
constexpr std::uint64_t kGhzInHz = 1'000'000'000;
constexpr double kMicroWattsPerWatt = 1'000'000.0;

// PCI vendor IDs for the vendors the monitor special-cases. Any other vendor
// is reported as Unknown and its metrics stay "N/A".
constexpr std::uint64_t kVendorAmd = 0x1002;
constexpr std::uint64_t kVendorNvidia = 0x10de;
constexpr std::uint64_t kVendorIntel = 0x8086;

/// Candidate locations of the read-only PCI ID database (hwdata / pciutils).
constexpr std::string_view kPciIdsCandidates[] = {
    "/usr/share/hwdata/pci.ids",
    "/usr/share/misc/pci.ids",
};

/// Strips leading ASCII whitespace from `text`.
std::string_view trimLeading(std::string_view text) {
  const std::size_t begin = text.find_first_not_of(" \t\r");
  return begin == std::string_view::npos ? text.substr(text.size())
                                         : text.substr(begin);
}

/// Parses an unsigned 64-bit integer with an optional "0x" prefix (the format
/// used by /sys vendor/device files). Returns false for empty or malformed
/// text; trailing garbage is allowed so whole tokens parse without splitting.
bool parseUInt(std::string_view text, int base, std::uint64_t &out) {
  text = trimLeading(text);
  if (text.empty()) {
    return false;
  }
  size_t pos = 0;
  if ((base == 16 && text.size() >= 2 && text[0] == '0' &&
       (text[1] == 'x' || text[1] == 'X'))) {
    pos = 2;
  }
  const char *begin = text.data() + pos;
  const char *end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, out, base);
  return ec == std::errc() && ptr > begin;
}

/// Reads a single-line sysfs value and parses it as a decimal integer.
/// Returns false when the file is missing, unreadable or malformed.
bool readUIntFile(const std::string &path, std::uint64_t &out) {
  std::ifstream file(path);
  std::string text;
  if (!file || !std::getline(file, text)) {
    return false;
  }
  return parseUInt(text, 10, out);
}

/// Reads a single-line sysfs value and parses it as a hexadecimal integer
/// (vendor/device IDs such as "0x8086").
bool readHexFile(const std::string &path, std::uint64_t &out) {
  std::ifstream file(path);
  std::string text;
  if (!file || !std::getline(file, text)) {
    return false;
  }
  return parseUInt(text, 16, out);
}

bool isDrmCardName(const std::string &name) {
  if (name.size() <= 4 || name.compare(0, 4, "card") != 0) {
    return false;
  }
  return std::all_of(name.begin() + 4, name.end(), [](unsigned char c) {
    return std::isdigit(c) != 0;
  });
}

/// Numeric card number of a "cardN" name, used for stable ordering.
int cardNumber(const std::string &card) {
  std::uint64_t number = 0;
  if (card.size() > 4 &&
      parseUInt(std::string_view(card).substr(4), 10, number)) {
    return static_cast<int>(number);
  }
  return 0;
}

/// The DRM primary nodes present, sorted by card number ascending.
std::vector<std::string> listDrmCards() {
  std::vector<std::string> cards;
  std::error_code ec;
  fs::directory_iterator it("/sys/class/drm", ec);
  const fs::directory_iterator end;
  while (!ec && it != end) {
    const std::string name = it->path().filename().string();
    if (isDrmCardName(name)) {
      cards.push_back(name);
    }
    it.increment(ec);
  }
  std::sort(cards.begin(), cards.end(),
            [](const std::string &a, const std::string &b) {
              return cardNumber(a) < cardNumber(b);
            });
  return cards;
}

/// Filename of the driver bound to a device's PCI function, e.g. "amdgpu",
/// "i915", "nouveau" or "nvidia". Empty when unbound or unreadable.
std::string readDriverName(const fs::path &device_dir) {
  std::error_code ec;
  const fs::path target = fs::read_symlink(device_dir / "driver", ec);
  if (ec) {
    return {};
  }
  return target.filename().string();
}

/// Reads PCI_SLOT_NAME from the device's uevent ("0000:00:02.0").
std::string readPciSlot(const fs::path &device_dir) {
  std::ifstream file(device_dir / "uevent");
  if (!file) {
    return {};
  }
  constexpr std::string_view kPrefix = "PCI_SLOT_NAME=";
  std::string line;
  while (std::getline(file, line)) {
    if (line.compare(0, kPrefix.size(), kPrefix) == 0) {
      return line.substr(kPrefix.size());
    }
  }
  return {};
}

GpuVendor vendorFromId(std::uint64_t vendor_id) {
  switch (vendor_id) {
    case kVendorAmd:
      return GpuVendor::Amd;
    case kVendorIntel:
      return GpuVendor::Intel;
    case kVendorNvidia:
      return GpuVendor::Nvidia;
    default:
      return GpuVendor::Unknown;
  }
}

/// Looks up a device model name in the read-only PCI ID database. Returns an
/// empty string when the database is absent or the IDs are not listed.
std::string lookupPciModel(std::uint64_t vendor_id, std::uint64_t device_id) {
  if (vendor_id == 0 || device_id == 0) {
    return {};
  }
  const std::string vendor_hex = [vendor_id] {
    std::ostringstream out;
    out << std::uppercase << std::hex << vendor_id;
    return out.str();
  }();
  const std::string device_hex = [device_id] {
    std::ostringstream out;
    out << std::uppercase << std::hex << device_id;
    return out.str();
  }();

  for (const std::string_view path : kPciIdsCandidates) {
    const std::string filename{path};
    std::ifstream file(filename);
    if (!file) {
      continue;
    }
    bool in_vendor = false;
    std::string line;
    while (std::getline(file, line)) {
      if (line.empty() || line[0] == '#') {
        continue;
      }
      if (line[0] != '\t') {
        // A vendor header: four hex digits followed by whitespace.
        in_vendor = line.size() >= 5 &&
                    line.compare(0, 4, vendor_hex) == 0 &&
                    (line[4] == ' ' || line[4] == '\t');
        continue;
      }
      if (!in_vendor || line.size() < 2 || line[1] == '\t') {
        continue;  // outside the vendor section, or a subvendor line
      }
      if (line.compare(1, 4, device_hex) == 0 &&
          line.size() >= 6 && line[5] == ' ') {
        std::size_t begin = 6;
        while (begin < line.size() && line[begin] == ' ') {
          ++begin;
        }
        std::string model = line.substr(begin);
        while (!model.empty() &&
               (model.back() == ' ' || model.back() == '\t')) {
          model.pop_back();
        }
        return model;
      }
    }
  }
  return {};
}

/// Resolves the model name for a card, falling back to a constructed label
/// when the PCI ID database is unavailable.
std::string makeGpuName(GpuVendor vendor, std::uint64_t vendor_id,
                        std::uint64_t device_id) {
  std::string model = lookupPciModel(vendor_id, device_id);
  if (!model.empty()) {
    return model;
  }
  std::ostringstream fallback;
  fallback << gpuVendorName(vendor);
  if (device_id != 0) {
    fallback << " 0x" << std::uppercase << std::hex << device_id;
  }
  return fallback.str();
}

/// All "gtN" engine-directory names under a card's gt/ folder.
std::vector<std::string> listGtNames(const std::string &card_path) {
  std::vector<std::string> gts;
  const fs::path gt_dir = fs::path(card_path) / "gt";
  std::error_code ec;
  fs::directory_iterator it(gt_dir, ec);
  const fs::directory_iterator end;
  while (!ec && it != end) {
    const std::string name = it->path().filename().string();
    if (name.size() > 2 && name.compare(0, 2, "gt") == 0 &&
        std::all_of(name.begin() + 2, name.end(), [](unsigned char c) {
          return std::isdigit(c) != 0;
        })) {
      gts.push_back(name);
    }
    it.increment(ec);
  }
  return gts;
}

/// Aggregated RC6 (deep-sleep) residency across all GT engines, in
/// milliseconds since boot. Returns false when no GT exposes it.
bool readTotalRc6Ms(const std::string &card_path, std::uint64_t &out) {
  std::uint64_t total = 0;
  bool any = false;
  for (const std::string &gt : listGtNames(card_path)) {
    const fs::path path =
        fs::path(card_path) / "gt" / gt / "rc6_residency_ms";
    std::uint64_t value = 0;
    if (readUIntFile(path.string(), value)) {
      total += value;
      any = true;
    }
  }
  out = total;
  return any;
}

/// True when any GT exposes RC6 as enabled (rc6_enable == 1).
bool rc6Enabled(const std::string &card_path) {
  for (const std::string &gt : listGtNames(card_path)) {
    std::uint64_t enabled = 0;
    const fs::path path =
        fs::path(card_path) / "gt" / gt / "rc6_enable";
    if (readUIntFile(path.string(), enabled) && enabled == 1) {
      return true;
    }
  }
  return false;
}

/// The highest currently-active engine frequency across all GT engines.
/// Reports false when no per-GT rps file exists (older kernels fall back to
/// the card-level gt_act_freq_mhz instead).
bool readMaxActFreqMhz(const std::string &card_path, std::uint64_t &out) {
  std::uint64_t max_value = 0;
  bool any = false;
  for (const std::string &gt : listGtNames(card_path)) {
    const fs::path path = fs::path(card_path) / "gt" / gt / "rps_act_freq_mhz";
    std::uint64_t value = 0;
    if (readUIntFile(path.string(), value)) {
      max_value = std::max(max_value, value);
      any = true;
    }
  }
  out = max_value;
  return any;
}

/// Parses the currently-active state out of an AMD pp_dpm_sclk dump. The
/// active line carries a '*' marker and the clock before its "Mhz" unit, e.g.
/// "4: 2000Mhz *" (older kernels "1: 1000Mhz" marked differently) or
/// "* 0: 500Mhz". Returns false when the file is absent or has no marker.
bool readAmdCurrentFreqMhz(const std::string &path, std::uint64_t &out) {
  std::ifstream file(path);
  if (!file) {
    return false;
  }
  std::string line;
  while (std::getline(file, line)) {
    if (line.find('*') == std::string::npos) {
      continue;
    }
    const std::size_t unit = line.find("Mhz");
    const std::size_t unit_upper = line.find("MHz");
    const std::size_t unit_pos =
        unit != std::string::npos ? unit
        : (unit_upper != std::string::npos ? unit_upper
                                           : std::string::npos);
    if (unit_pos == std::string::npos) {
      continue;
    }
    std::size_t begin = unit_pos;
    while (begin > 0 && std::isdigit(static_cast<unsigned char>(line[begin - 1]))) {
      --begin;
    }
    if (begin == unit_pos) {
      continue;  // no digits immediately before the unit
    }
    std::uint64_t value = 0;
    if (parseUInt(std::string_view(line).substr(begin, unit_pos - begin), 10,
                  value)) {
      out = value;
      return true;
    }
  }
  return false;
}

/// Reads average GPU power from a generic hwmon sensor (power1_input, in
/// microwatts). Returns nullopt when no sensor exposes it.
std::optional<double> readHwmonPowerWatts(const fs::path &device_dir) {
  std::error_code ec;
  fs::directory_iterator hwmon_it(device_dir / "hwmon", ec);
  const fs::directory_iterator end;
  while (!ec && hwmon_it != end) {
    if (hwmon_it->is_directory()) {
      std::uint64_t micro_watts = 0;
      if (readUIntFile((hwmon_it->path() / "power1_input").string(),
                       micro_watts)) {
        return static_cast<double>(micro_watts) / kMicroWattsPerWatt;
      }
    }
    hwmon_it.increment(ec);
  }
  return std::nullopt;
}

/// Fills the VRAM fields (total/used/free/usage%) from a pair of byte counts.
/// The percentage and free space are only derived when total is non-zero so a
/// division by zero is impossible.
void fillMemoryFields(GpuStats &stats, std::uint64_t total, std::uint64_t used) {
  stats.memory_total_bytes = total;
  stats.memory_used_bytes = used;
  stats.memory_free_bytes = total >= used ? total - used : 0;
  if (total > 0) {
    const double usage =
        (static_cast<double>(used) / static_cast<double>(total)) * 100.0;
    stats.memory_usage_percent = std::clamp(usage, 0.0, 100.0);
  }
}

/// Refreshes the AMD (amdgpu driver) metrics from the kernel's sysfs files.
void refreshAmdMetrics(GpuStats &stats, const std::string &device_dir) {
  std::uint64_t busy = 0;
  if (readUIntFile(device_dir + "/gpu_busy_percent", busy)) {
    stats.utilization_percent =
        std::clamp(static_cast<double>(busy), 0.0, 100.0);
  }

  std::uint64_t vram_total = 0;
  std::uint64_t vram_used = 0;
  if (readUIntFile(device_dir + "/mem_info_vram_total", vram_total) &&
      readUIntFile(device_dir + "/mem_info_vram_used", vram_used)) {
    fillMemoryFields(stats, vram_total, vram_used);
  }

  std::uint64_t freq_mhz = 0;
  if (readAmdCurrentFreqMhz(device_dir + "/pp_dpm_sclk", freq_mhz) &&
      freq_mhz > 0) {
    stats.frequency_hz = freq_mhz * kMhzInHz;
  }
}

/// Refreshes the Intel (i915 driver) metrics. Frequency comes from the
/// per-GT rps file (or the card-level gt_act_freq_mhz fallback on older
/// kernels); GT utilization is derived from the RC6 residency delta between
/// two samples passed in via the monitor's per-device baseline state.
void refreshIntelMetrics(GpuStats &stats, const std::string &card_path,
                         std::optional<std::uint64_t> &rc6_ms,
                         std::optional<std::chrono::steady_clock::time_point>
                             &rc6_time) {
  const fs::path card = card_path;

  std::uint64_t freq_mhz = 0;
  if (!readMaxActFreqMhz(card_path, freq_mhz)) {
    static_cast<void>(readUIntFile((card / "gt_act_freq_mhz").string(),
                                   freq_mhz));
  }
  if (freq_mhz > 0) {
    stats.frequency_hz = freq_mhz * kMhzInHz;
  }

  const auto now = std::chrono::steady_clock::now();
  std::uint64_t rc6_now_ms = 0;
  if (rc6Enabled(card_path) && readTotalRc6Ms(card_path, rc6_now_ms)) {
    if (rc6_ms.has_value() && rc6_time.has_value()) {
      const double wall_ms =
          std::chrono::duration<double>(now - *rc6_time).count() * 1000.0;
      // Guard the delta: a counter reset (smaller than before) re-seeds the
      // baseline instead of reporting a bogus busy figure, and wall_ms of 0
      // means two samples with no elapsed time.
      if (wall_ms > 0.0 && rc6_now_ms >= *rc6_ms) {
        const double idle_percent =
            100.0 * static_cast<double>(rc6_now_ms - *rc6_ms) / wall_ms;
        stats.utilization_percent = std::clamp(100.0 - idle_percent, 0.0, 100.0);
      }
    }
    rc6_ms = rc6_now_ms;
    rc6_time = now;
  } else {
    stats.utilization_percent.reset();
    rc6_ms.reset();
    rc6_time.reset();
  }
}

}  // namespace

const char *gpuVendorName(GpuVendor vendor) {
  switch (vendor) {
    case GpuVendor::Amd:
      return "AMD";
    case GpuVendor::Intel:
      return "Intel";
    case GpuVendor::Nvidia:
      return "NVIDIA";
    case GpuVendor::Unknown:
      return "Unknown";
  }
  return "Unknown";
}

void GpuMonitor::discover(const std::vector<std::string> &cards) {
  devices_.clear();
  devices_.reserve(cards.size());
  for (const std::string &card : cards) {
    const fs::path card_path = fs::path("/sys/class/drm") / card;

    GpuDevice device;
    device.sysfs_path = card_path.string();
    device.stats.card = card;

    const fs::path device_dir = card_path / "device";
    std::uint64_t vendor_id = 0;
    std::uint64_t device_id = 0;
    static_cast<void>(readHexFile((device_dir / "vendor").string(), vendor_id));
    static_cast<void>(readHexFile((device_dir / "device").string(), device_id));
    device.stats.vendor = vendorFromId(vendor_id);
    device.stats.driver = readDriverName(device_dir);
    device.stats.pci_slot = readPciSlot(device_dir);
    device.stats.name =
        makeGpuName(device.stats.vendor, vendor_id, device_id);

    devices_.push_back(std::move(device));
  }
  discovered_cards_ = cards;
}

GpuSnapshot GpuMonitor::read() {
  // Cheap directory listing each second; discovery itself (which reads a few
  // files per card) only runs when the set of DRM primary nodes changes.
  const std::vector<std::string> cards = listDrmCards();
  if (cards != discovered_cards_) {
    discover(cards);
  }

  GpuSnapshot snapshot;
  snapshot.devices.reserve(devices_.size());
  for (GpuDevice &device : devices_) {
    refreshMetrics(device);
    snapshot.devices.push_back(device.stats);
  }
  return snapshot;
}

void GpuMonitor::refreshMetrics(GpuDevice &device) {
  // Reset every dynamic value first so a metric the driver stops exposing is
  // rendered as "N/A" again instead of a stale reading.
  device.stats.utilization_percent.reset();
  device.stats.memory_total_bytes.reset();
  device.stats.memory_used_bytes.reset();
  device.stats.memory_free_bytes.reset();
  device.stats.memory_usage_percent.reset();
  device.stats.frequency_hz.reset();
  device.stats.power_watts.reset();

  const fs::path card_path = device.sysfs_path;
  const fs::path device_dir = card_path / "device";

  // Generic: any GPU whose driver exposes an average-power sensor.
  device.stats.power_watts = readHwmonPowerWatts(device_dir);

  switch (device.stats.vendor) {
    case GpuVendor::Amd:
      refreshAmdMetrics(device.stats, device_dir.string());
      break;
    case GpuVendor::Intel:
      refreshIntelMetrics(device.stats, card_path.string(), device.rc6_ms,
                          device.rc6_time);
      break;
    case GpuVendor::Nvidia:
    case GpuVendor::Unknown:
      // No generic kernel interface allows reading utilization/VRAM/clock for
      // these drivers yet; only identity (and any hwmon power) is reported.
      break;
  }
}

std::string formatGpuFrequency(const std::optional<std::uint64_t> &frequency_hz) {
  if (!frequency_hz.has_value()) {
    return "N/A";
  }
  std::ostringstream out;
  out << std::fixed;
  if (*frequency_hz >= kGhzInHz) {
    out << std::setprecision(2)
        << (static_cast<double>(*frequency_hz) / static_cast<double>(kGhzInHz))
        << " GHz";
  } else {
    out << std::setprecision(0)
        << (static_cast<double>(*frequency_hz) / static_cast<double>(kMhzInHz))
        << " MHz";
  }
  return out.str();
}

std::string formatGpuPower(const std::optional<double> &watts) {
  if (!watts.has_value()) {
    return "N/A";
  }
  std::ostringstream out;
  out << std::fixed << std::setprecision(1) << *watts << " W";
  return out.str();
}

}  // namespace atm