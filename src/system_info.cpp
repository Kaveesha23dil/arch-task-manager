#include "system_info.hpp"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <sys/utsname.h>
#include <utility>

#include "gpu_monitor.hpp"
#include "memory_monitor.hpp"

namespace atm {

namespace {

/// Reads an entire text file safely. Returns std::nullopt when the file
/// cannot be opened, is a directory, or is empty after trimming trailing
/// whitespace. Used for all the small Linux interface files (os-release,
/// cpuinfo, DMI, uptime). Never throws or crashes on a missing/malformed
/// file.
std::optional<std::string> readFile(const std::filesystem::path &path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return std::nullopt;
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  std::string content = buffer.str();

  // Remove trailing whitespace (spaces, tabs, CR, LF) so a value like
  // "Arch Linux\n" becomes "Arch Linux". No escaping/interpretation is
  // performed — the content is treated strictly as configuration data.
  const auto not_space = [](unsigned char c) {
    return !std::isspace(c);
  };
  const auto last = std::find_if(content.rbegin(), content.rend(), not_space);
  if (last == content.rend()) {
    return std::nullopt;  // all whitespace (effectively empty)
  }
  content.erase(last.base(), content.end());
  return content;
}

/// Reads the first whitespace-delimited field of a file. Returns "" on any
/// failure.
std::string readFirstToken(const std::filesystem::path &path) {
  const auto content = readFile(path);
  if (!content) {
    return "";
  }
  std::istringstream parser(*content);
  std::string token;
  parser >> token;
  return token;
}

/// Parses a non-negative 64-bit integer token. Returns std::nullopt when the
/// token is missing or not a plain decimal number.
std::optional<std::uint64_t> parseU64(const std::string &token) {
  if (token.empty()) {
    return std::nullopt;
  }
  char *end = nullptr;
  errno = 0;
  const unsigned long long value = std::strtoull(token.c_str(), &end, 10);
  if (errno != 0 || end == token.c_str() || *end != '\0') {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(value);
}

/// Parses the leading integer portion of a token, ignoring any trailing
/// fraction, unit or junk. This is used for /proc/uptime ("909.27 ...") where
/// the whole number of seconds is the leading decimal field.
std::optional<std::uint64_t> parseLeadingU64(const std::string &token) {
  std::size_t end = 0;
  while (end < token.size() && std::isdigit(static_cast<unsigned char>(token[end]))) {
    ++end;
  }
  return parseU64(token.substr(0, end));
}

/// Trims leading/trailing ASCII whitespace from a token.
std::string trimToken(std::string s) {
  std::size_t begin = 0;
  while (begin < s.size() &&
         std::isspace(static_cast<unsigned char>(s[begin]))) {
    ++begin;
  }
  std::size_t end = s.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(s[end - 1]))) {
    --end;
  }
  return s.substr(begin, end - begin);
}

/// Parses one KEY=value line from /etc/os-release. Surrounding double quotes
/// are stripped; backslash escapes are kept literally. The file is treated
/// strictly as configuration data and never executed. An empty key is
/// returned when the line is not a KEY=value line.
std::pair<std::string, std::string> parseOsReleaseLine(
    const std::string &line) {
  const std::size_t eq = line.find('=');
  if (eq == std::string::npos) {
    return {"", ""};
  }
  std::string key = trimToken(line.substr(0, eq));
  std::string value = trimToken(line.substr(eq + 1));
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
    value = value.substr(1, value.size() - 2);
  }
  return {key, value};
}

/// Small /etc/os-release parser extracting the fields this module displays:
/// NAME, PRETTY_NAME, ID and VERSION_ID. A missing or malformed file yields
/// empty strings, never a crash.
struct OsRelease {
  std::string name;         // NAME
  std::string pretty_name;  // PRETTY_NAME
  std::string id;           // ID
  std::string version_id;   // VERSION_ID
};

OsRelease parseOsRelease(const std::filesystem::path &path) {
  const auto content = readFile(path);
  if (!content) {
    return {};
  }
  OsRelease result;
  std::istringstream stream(*content);
  std::string line;
  while (std::getline(stream, line)) {
    const auto [key, value] = parseOsReleaseLine(line);
    if (key == "NAME") result.name = value;
    else if (key == "PRETTY_NAME") result.pretty_name = value;
    else if (key == "ID") result.id = value;
    else if (key == "VERSION_ID") result.version_id = value;
  }
  return result;
}

/// Map uname(2) machine strings to a short, human-friendly architecture name.
std::string friendlyArchitecture(const std::string &machine) {
  if (machine == "x86_64" || machine == "amd64") return "x86_64";
  if (machine == "i386" || machine == "i486" || machine == "i586" ||
      machine == "i686") return "x86 (32-bit)";
  if (machine == "aarch64" || machine == "arm64") return "arm64";
  if (machine.rfind("armv", 0) == 0) return "arm";
  if (machine == "riscv64") return "riscv64";
  if (machine == "ppc64le") return "ppc64le";
  if (machine == "s390x") return "s390x";
  return machine;
}

/// Counts logical CPUs and derives the physical core count from /proc/cpuinfo.
///
/// Logical CPUs = number of "processor" lines. Physical cores are computed as
/// cores-per-package ("cpu cores") times the number of distinct physical
/// packages ("physical id"). This is reliable on x86; when the topology
/// fields are absent (e.g. some ARM systems) the physical count is left at 0
/// so the front-end renders "N/A" rather than guessing.
struct CpuTopology {
  std::uint32_t logical = 0;
  std::uint32_t physical = 0;
  std::string model;  // "model name" from the first processor block
};

CpuTopology parseCpuInfo(const std::filesystem::path &path) {
  const auto content = readFile(path);
  CpuTopology result;
  if (!content) {
    return result;
  }

  std::uint32_t cores_per_package = 0;
  std::set<std::string> packages;

  std::istringstream stream(*content);
  std::string line;
  while (std::getline(stream, line)) {
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    const std::string key = trimToken(line.substr(0, colon));
    const std::string value = trimToken(line.substr(colon + 1));

    if (key == "processor") {
      ++result.logical;
    } else if (key == "model name" && result.model.empty()) {
      result.model = value;
    } else if (key == "cpu cores") {
      if (const auto c = parseU64(value); c.has_value()) {
        cores_per_package = static_cast<std::uint32_t>(*c);
      }
    } else if (key == "physical id") {
      packages.insert(value);
    }
  }

  if (cores_per_package > 0 && !packages.empty()) {
    const auto package_count = static_cast<std::uint64_t>(packages.size());
    const auto total = static_cast<std::uint64_t>(cores_per_package) *
                       package_count;
    // Guard against an absurd value from a malformed file.
    if (total <= 4096) {
      result.physical = static_cast<std::uint32_t>(total);
    }
  }
  return result;
}

/// Reads one DMI value under /sys/class/dmi/id, returning "" when it does not
/// exist or cannot be read (kernel security: some fields are root-only).
std::string readDmiValue(const char *name) {
  return readFile(std::filesystem::path("/sys/class/dmi/id") / name)
      .value_or("");
}

}  // namespace

std::string formatUptime(std::uint64_t seconds) {
  if (seconds == 0) {
    return "0 seconds";
  }
  const std::uint64_t days = seconds / 86400;
  const std::uint64_t hours = (seconds % 86400) / 3600;
  const std::uint64_t minutes = (seconds % 3600) / 60;
  const std::uint64_t seconds_left = seconds % 60;

  // Singular/plural helpers keep the output grammatical.
  const auto plural = [](std::uint64_t n, const char *unit) {
    return std::to_string(n) + " " + unit + (n == 1 ? "" : "s");
  };

  // Report only the significant units, dropping seconds whenever a larger
  // unit is present (e.g. "2 days, 7 hours, 10 minutes", never the seconds).
  if (days > 0) {
    std::string out = plural(days, "day");
    if (hours > 0) {
      out += ", " + plural(hours, "hour");
    }
    if (minutes > 0) {
      out += ", " + plural(minutes, "minute");
    }
    return out;
  }
  if (hours > 0) {
    std::string out = plural(hours, "hour");
    if (minutes > 0) {
      out += ", " + plural(minutes, "minute");
    }
    return out;
  }
  if (minutes > 0) {
    return plural(minutes, "minute");
  }
  return plural(seconds_left, "second");
}

void SystemInfoProvider::load(const GpuSnapshot &gpu) {
  SystemInfo info;

  // Hostname via gethostname(2) — no shell command.
  char hostname_buffer[256] = {0};
  if (::gethostname(hostname_buffer, sizeof(hostname_buffer) - 1) == 0) {
    info.hostname = hostname_buffer;
  }

  // Kernel information via uname(2).
  struct ::utsname uts {};
  if (::uname(&uts) == 0) {
    info.kernel_version = uts.release;
    info.kernel_release = uts.version;
    info.architecture = uts.machine;
    info.cpu_architecture = friendlyArchitecture(uts.machine);
  }

  // OS / distribution from /etc/os-release. Any other Linux distribution is
  // identified correctly if the file reports it.
  const OsRelease os =
      parseOsRelease(std::filesystem::path("/etc/os-release"));
  info.operating_system =
      !os.pretty_name.empty() ? os.pretty_name : os.name;
  info.distribution = os.id;
  info.distribution_version = os.version_id;

  // CPU model and topology from /proc/cpuinfo.
  const CpuTopology cpu = parseCpuInfo(std::filesystem::path("/proc/cpuinfo"));
  info.cpu_model = cpu.model;
  info.cpu_logical_cores = cpu.logical;
  info.cpu_physical_cores = cpu.physical;

  // Memory / swap totals — reuse the existing memory reader so RAM capacity is
  // never parsed a second, independent way.
  if (const auto memory = readMemoryInfo(); memory.has_value()) {
    info.total_memory = memory->total * 1024ULL;    // kB -> bytes
    info.total_swap = memory->swap_total * 1024ULL;  // kB -> bytes
  }

  // Uptime from the first token of /proc/uptime (whole seconds since boot).
  if (const auto uptime = parseLeadingU64(readFirstToken("/proc/uptime"));
      uptime.has_value()) {
    info.uptime_seconds = *uptime;
  }

  // DMI hardware identity — best-effort per field; a missing or read-protected
  // file simply yields "" and never aborts the load.
  info.manufacturer = readDmiValue("sys_vendor");
  info.product_name = readDmiValue("product_name");
  info.product_version = readDmiValue("product_version");
  info.motherboard_vendor = readDmiValue("board_vendor");
  info.motherboard = readDmiValue("board_name");
  info.motherboard_version = readDmiValue("board_version");
  info.bios_vendor = readDmiValue("bios_vendor");
  info.bios_version = readDmiValue("bios_version");
  info.bios_date = readDmiValue("bios_date");

  // GPU names come from the existing GpuMonitor's snapshot (shared across the
  // application), so GPU detection is never duplicated.
  info.gpus.reserve(gpu.devices.size());
  for (const GpuStats &device : gpu.devices) {
    if (!device.name.empty()) {
      info.gpus.push_back(device.name);
    }
  }

  info_ = std::move(info);
}

SystemInfo SystemInfoProvider::read() const { return info_; }

}  // namespace atm