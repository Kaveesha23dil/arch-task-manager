#include "memory_monitor.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>

namespace atm {

std::uint64_t MemoryInfo::used() const {
  // MemAvailable is always <= MemTotal, but clamp defensively instead of
  // letting unsigned arithmetic wrap around.
  return total > available ? total - available : 0;
}

std::uint64_t MemoryInfo::swapUsed() const {
  return swap_total > swap_free ? swap_total - swap_free : 0;
}

double MemoryInfo::usagePercent() const {
  if (total == 0) {
    return 0.0;
  }
  const double ratio = static_cast<double>(used()) / static_cast<double>(total);
  return std::clamp(ratio * 100.0, 0.0, 100.0);
}

double MemoryInfo::swapUsagePercent() const {
  if (swap_total == 0) {
    return 0.0;  // no swap configured — avoid division by zero
  }
  const double ratio =
      static_cast<double>(swapUsed()) / static_cast<double>(swap_total);
  return std::clamp(ratio * 100.0, 0.0, 100.0);
}

std::optional<MemoryInfo> readMemoryInfo() {
  MemoryInfo info;

  // RAII: the file is closed when `file` goes out of scope.
  std::ifstream file("/proc/meminfo");
  if (!file.is_open()) {
    return std::nullopt;
  }

  std::string line;
  while (std::getline(file, line)) {
    // Each line has the form "FieldName:       <value> kB".
    std::istringstream parser(line);
    std::string key;
    std::uint64_t value = 0;
    std::string unit;
    if (!(parser >> key >> value >> unit)) {
      continue;  // malformed line — skip; the field keeps its default
    }
    if (unit != "kB") {
      continue;  // unexpected unit — do not trust the value
    }

    if (key == "MemTotal:") {
      info.total = value;
    } else if (key == "MemAvailable:") {
      info.available = value;
    } else if (key == "MemFree:") {
      info.free = value;
    } else if (key == "Buffers:") {
      info.buffers = value;
    } else if (key == "Cached:") {
      info.cached = value;
    } else if (key == "SwapTotal:") {
      info.swap_total = value;
    } else if (key == "SwapFree:") {
      info.swap_free = value;
    }
  }

  // MemAvailable exists on all kernels Arch ships, but fall back to the
  // classic approximation (MemFree + Buffers + Cached) when it is missing.
  if (info.available == 0 && info.total != 0) {
    info.available = info.free + info.buffers + info.cached;
  }

  return info;
}

std::optional<MemoryInfo> MemoryMonitor::read() {
  return readMemoryInfo();
}

}  // namespace atm