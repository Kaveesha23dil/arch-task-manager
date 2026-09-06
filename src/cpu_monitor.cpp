#include "cpu_monitor.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>

namespace atm {

std::uint64_t CpuTimes::total() const {
  return user + nice + system + idle + iowait + irq + softirq + steal;
}

std::uint64_t CpuTimes::idleTime() const {
  return idle + iowait;
}

std::uint64_t CpuTimes::busy() const {
  return total() - idleTime();
}

std::optional<CpuTimes> readCpuTimes() {
  // RAII: the file is closed when `file` goes out of scope.
  std::ifstream file("/proc/stat");
  if (!file.is_open()) {
    return std::nullopt;
  }

  std::string line;
  while (std::getline(file, line)) {
    // The aggregate line starts with "cpu" followed by a space. Per-core
    // lines ("cpu0", "cpu1", ...) must not match, hence the trailing space.
    if (line.rfind("cpu ", 0) != 0) {
      continue;
    }

    std::istringstream parser(line);
    CpuTimes times;
    std::string label;
    if (!(parser >> label >> times.user >> times.nice >> times.system >>
          times.idle >> times.iowait >> times.irq >> times.softirq >>
          times.steal)) {
      return std::nullopt;  // malformed "cpu" line
    }
    return times;
  }

  return std::nullopt;  // no aggregate "cpu" line present
}

std::optional<double> cpuUtilization(const CpuTimes &previous,
                                     const CpuTimes &current) {
  if (current.total() < previous.total()) {
    return std::nullopt;  // counters reset (e.g. VM migration) — cannot diff
  }

  const std::uint64_t delta_total = current.total() - previous.total();
  if (delta_total == 0) {
    return std::nullopt;  // no ticks elapsed between the two samples
  }

  // Guard against idle delta exceeding total delta (unexpected, but clamp
  // instead of underflowing a signed subtraction).
  const std::uint64_t delta_idle =
      std::min(delta_total, current.idleTime() - previous.idleTime());
  const double busy_ratio =
      static_cast<double>(delta_total - delta_idle) / static_cast<double>(delta_total);

  return std::clamp(busy_ratio * 100.0, 0.0, 100.0);
}

std::optional<double> CpuMonitor::readUsage() {
  const std::optional<CpuTimes> current = readCpuTimes();
  if (!current.has_value()) {
    return std::nullopt;
  }

  if (!previous_.has_value()) {
    previous_ = current;  // first sample: record a baseline only
    return std::nullopt;
  }

  const std::optional<double> utilization =
      cpuUtilization(*previous_, *current);
  previous_ = current;
  return utilization;
}

}  // namespace atm