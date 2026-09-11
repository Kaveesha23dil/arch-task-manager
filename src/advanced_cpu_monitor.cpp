#include "advanced_cpu_monitor.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace atm {

namespace {

namespace fs = std::filesystem;

constexpr std::uint64_t kKhzInMhz = 1'000;
constexpr std::uint64_t kKhzInGhz = 1'000'000;

/// Whether the line is a CPU counter line: the aggregate "cpu " line or a
/// per-logical-CPU "cpuN" line.
bool isCpuLine(const std::string &line) {
  return line.rfind("cpu", 0) == 0;
}

/// Whether the line is the aggregate line ("cpu" followed by a space/tab).
bool isAggregateLine(const std::string &line) {
  return line.rfind("cpu ", 0) == 0;
}

/// Strictly parses one token as an unsigned 64-bit decimal value. Accepts only
/// plain digits (no sign, no trailing garbage) and rejects overflow.
bool parseTokenU64(const std::string &token, std::uint64_t &out) {
  if (token.empty() || token.front() == '-') {
    return false;
  }
  errno = 0;
  char *end = nullptr;
  const unsigned long long value = std::strtoull(token.c_str(), &end, 10);
  if (errno == ERANGE || end == token.c_str() || *end != '\0') {
    return false;
  }
  out = static_cast<std::uint64_t>(value);
  return true;
}

/// Strips a trailing newline/carriage-return off a raw sysfs line.
std::string trimNewline(std::string text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
    text.pop_back();
  }
  return text;
}

/// Reads a single-line positive integer from a sysfs file.
std::optional<std::uint64_t> readUintFile(const fs::path &path) {
  std::ifstream file(path);
  std::string text;
  if (!file || !std::getline(file, text)) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  if (!parseTokenU64(trimNewline(std::move(text)), value)) {
    return std::nullopt;
  }
  return value;
}

/// Reads a single-line sysfs string (e.g. the governor name).
std::optional<std::string> readTextFile(const fs::path &path) {
  std::ifstream file(path);
  std::string text;
  if (!file || !std::getline(file, text)) {
    return std::nullopt;
  }
  text = trimNewline(std::move(text));
  if (text.empty()) {
    return std::nullopt;
  }
  return text;
}

}  // namespace

std::uint64_t CpuCounterSnapshot::total() const {
  // guest/guest_nice are already counted inside user/nice by the kernel, so
  // adding them here would double-count guest execution.
  return user + nice + system + idle + iowait + irq + softirq + steal;
}

std::uint64_t CpuCounterSnapshot::idleTime() const {
  return idle + iowait;
}

std::uint64_t CpuCounterSnapshot::busy() const {
  return total() - idleTime();
}

bool parseCpuCounterLine(const std::string &line, CpuCounterSnapshot &out) {
  if (!isCpuLine(line)) {
    return false;
  }

  std::istringstream parser(line);
  std::string label;
  if (!(parser >> label)) {
    return false;
  }

  // The label is either the aggregate "cpu " or a logical CPU id ("cpuN").
  if (isAggregateLine(line)) {
    out.cpu_id = -1;
  } else {
    const std::string digits = label.substr(3);
    if (digits.empty()) {
      return false;  // "cpu" without an id and without the aggregate separator
    }
    std::uint64_t id = 0;
    if (!parseTokenU64(digits, id) || id > static_cast<std::uint64_t>(INT32_MAX)) {
      return false;
    }
    out.cpu_id = static_cast<int>(id);
  }

  // Standard field order: user, nice, system, idle, iowait, irq, softirq,
  // steal, guest, guest_nice. Only the first four are guaranteed to exist on
  // every kernel; fields beyond that parse as far as they are present and the
  // rest stay 0. Any extra fields are ignored. A malformed numeric field
  // invalidates the whole line (a truncated row must not be half-trusted).
  std::uint64_t *fields[] = {&out.user,    &out.nice,     &out.system,
                             &out.idle,    &out.iowait,   &out.irq,
                             &out.softirq, &out.steal,    &out.guest,
                             &out.guest_nice};
  std::string token;
  int parsed = 0;
  for (std::uint64_t *field : fields) {
    if (!(parser >> token)) {
      break;  // optional trailing fields are simply absent
    }
    if (!parseTokenU64(token, *field)) {
      return false;
    }
    ++parsed;
  }

  // user/nice/system/idle are mandatory; without them percentages cannot be
  // attributed to any category.
  return parsed >= 4;
}

bool parseProcStat(std::istream &in, ProcStatSnapshot &out) {
  out = ProcStatSnapshot{};
  std::string line;
  while (std::getline(in, line)) {
    if (!isCpuLine(line)) {
      continue;  // "intr", "ctxt", "btime" and friends are not CPU counters
    }
    CpuCounterSnapshot snapshot;
    if (!parseCpuCounterLine(line, snapshot)) {
      continue;  // a malformed/unknown "cpu*" line is skipped, not fatal
    }
    if (snapshot.cpu_id == -1) {
      out.has_aggregate = true;
    }
    out.counters[snapshot.cpu_id] = snapshot;
  }
  return true;
}

bool readProcStat(ProcStatSnapshot &out) {
  std::ifstream file("/proc/stat");
  if (!file.is_open()) {
    out = ProcStatSnapshot{};
    return false;
  }
  return parseProcStat(file, out);
}

void computeCpuDelta(const CpuCounterSnapshot &previous,
                     const CpuCounterSnapshot &current,
                     CpuDeltaPercentages &out) {
  out = CpuDeltaPercentages{};
  out.valid = false;

  if (current.total() < previous.total()) {
    return;  // counters decreased: reset, suspend/resume or a VM migration
  }

  const std::uint64_t delta_total = current.total() - previous.total();
  if (delta_total == 0) {
    return;  // no ticks elapsed between the two samples
  }

  // Each category as a share of the elapsed time, clamped to [0, 100] so
  // unexpected counter skew (e.g. a single field decreasing on its own) can
  // never produce a negative, NaN or out-of-range percentage.
  const auto share = [delta_total](std::uint64_t delta) {
    if (delta >= delta_total) {
      return 100.0;
    }
    return static_cast<double>(delta) / static_cast<double>(delta_total) *
           100.0;
  };

  out.user_percent = share(current.user - previous.user);
  out.nice_percent = share(current.nice - previous.nice);
  out.system_percent = share(current.system - previous.system);
  out.idle_percent = share(current.idle - previous.idle);
  out.iowait_percent = share(current.iowait - previous.iowait);
  out.irq_percent = share(current.irq - previous.irq);
  out.softirq_percent = share(current.softirq - previous.softirq);
  out.steal_percent = share(current.steal - previous.steal);

  // Busy time is everything that is not idle-like: total minus (idle+iowait).
  const std::uint64_t delta_busy =
      delta_total - (current.idleTime() - previous.idleTime());
  out.busy_percent = share(delta_busy);
  out.valid = true;
}

bool parseCpuList(const std::string &text, std::vector<int> &out) {
  out.clear();
  if (text.empty()) {
    return false;
  }

  const auto parseId = [](const std::string &token, std::uint64_t &id) {
    return parseTokenU64(token, id);
  };

  std::size_t pos = 0;
  while (pos < text.size()) {
    std::size_t end = text.find(',', pos);
    if (end == std::string::npos) {
      end = text.size();
    }
    const std::string token = text.substr(pos, end - pos);
    if (token.empty()) {
      return false;
    }

    const std::size_t dash = token.find('-');
    if (dash == std::string::npos) {
      std::uint64_t id = 0;
      if (!parseId(token, id) || id > static_cast<std::uint64_t>(INT32_MAX)) {
        return false;
      }
      out.push_back(static_cast<int>(id));
    } else {
      const std::string low = token.substr(0, dash);
      const std::string high = token.substr(dash + 1);
      std::uint64_t first = 0;
      std::uint64_t last = 0;
      if (!parseId(low, first) || !parseId(high, last) || first > last ||
          last > static_cast<std::uint64_t>(INT32_MAX)) {
        return false;
      }
      for (std::uint64_t id = first; id <= last; ++id) {
        out.push_back(static_cast<int>(id));
      }
    }

    if (end == text.size()) {
      break;
    }
    pos = end + 1;
  }

  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return !out.empty();
}

CpuTopologyInfo readCpuTopology() {
  CpuTopologyInfo info;
  const fs::path base = "/sys/devices/system/cpu";

  const auto readList = [](const fs::path &path, std::vector<int> &out,
                           bool &available) {
    std::ifstream file(path);
    std::string text;
    if (!file || !std::getline(file, text)) {
      return;
    }
    if (parseCpuList(trimNewline(std::move(text)), out)) {
      available = true;
    }
  };

  readList(base / "online", info.online, info.online_available);
  readList(base / "possible", info.possible, info.possible_available);
  readList(base / "present", info.present, info.present_available);
  return info;
}

CpuFrequencyInfo readCpuFrequency(int cpu_id,
                                  const std::filesystem::path &sys_root) {
  if (cpu_id < 0) {
    return CpuFrequencyInfo{};  // the aggregate has no cpufreq directory
  }

  CpuFrequencyInfo info;
  const fs::path cpufreq = sys_root / "sys" / "devices" / "system" / "cpu" /
                           ("cpu" + std::to_string(cpu_id)) / "cpufreq";

  std::error_code ec;
  if (!fs::is_directory(cpufreq, ec)) {
    return info;  // no cpufreq support for this CPU: everything stays unset
  }
  info.has_policy = true;

  // Prefer the scaling interface; cpuinfo_cur_freq is the fallback.
  if (const auto value = readUintFile(cpufreq / "scaling_cur_freq")) {
    info.current_khz = *value;
  } else if (const auto value = readUintFile(cpufreq / "cpuinfo_cur_freq")) {
    info.current_khz = *value;
  }

  if (const auto value = readUintFile(cpufreq / "scaling_min_freq")) {
    info.min_khz = *value;
  }
  if (const auto value = readUintFile(cpufreq / "scaling_max_freq")) {
    info.max_khz = *value;
  }
  if (const auto value = readTextFile(cpufreq / "scaling_governor")) {
    info.governor = *value;
  }
  return info;
}

std::string formatCpuFrequency(const std::optional<std::uint64_t> &khz) {
  if (!khz.has_value()) {
    return "N/A";
  }
  std::ostringstream out;
  out << std::fixed;
  if (*khz >= kKhzInGhz) {
    out << std::setprecision(2)
        << (static_cast<double>(*khz) / static_cast<double>(kKhzInGhz))
        << " GHz";
  } else {
    out << std::setprecision(0)
        << (static_cast<double>(*khz) / static_cast<double>(kKhzInMhz))
        << " MHz";
  }
  return out.str();
}

std::string formatCpuGovernor(const std::optional<std::string> &governor) {
  return governor.has_value() ? *governor : "N/A";
}

AdvancedCpuMonitor::AdvancedCpuMonitor(std::filesystem::path root)
    : root_(std::move(root)) {}

AdvancedCpuSnapshot AdvancedCpuMonitor::read() {
  AdvancedCpuSnapshot snap;

  // --- 1. CPU counters from /proc/stat (once per refresh) --------
  ProcStatSnapshot data;
  {
    std::ifstream file(root_ / "proc" / "stat");
    snap.proc_stat_readable = file.is_open() && parseProcStat(file, data);
  }

  // --- 2. Topology masks ------------------------------------------
  topology_ = CpuTopologyInfo{};
  const fs::path cpu_base = root_ / "sys" / "devices" / "system" / "cpu";
  const auto readList = [](const fs::path &path, std::vector<int> &out,
                           bool &available) {
    std::ifstream f(path);
    std::string text;
    if (!f || !std::getline(f, text)) {
      return;
    }
    if (parseCpuList(trimNewline(std::move(text)), out)) {
      available = true;
    }
  };
  readList(cpu_base / "online", topology_.online, topology_.online_available);
  readList(cpu_base / "possible", topology_.possible,
           topology_.possible_available);
  readList(cpu_base / "present", topology_.present,
           topology_.present_available);
  snap.topology = topology_;

  const std::set<int> online_set(topology_.online.begin(),
                                 topology_.online.end());

  // Every logical CPU this build wants to report: everything ever seen plus
  // everything the kernel currently lists as present. Offline CPUs stay listed
  // so their identity is preserved across hotplug events. Logical CPU IDs are
  // the stable key — never a position in a vector.
  std::set<int> universe = known_cpus_;
  for (const auto &[id, snapshot] : data.counters) {
    static_cast<void>(snapshot);
    if (id >= 0) {
      universe.insert(id);
      known_cpus_.insert(id);
    }
  }
  for (int id : topology_.present) {
    universe.insert(id);
    known_cpus_.insert(id);
  }

  // --- 3. Per-CPU deltas -------------------------------------------
  const auto finalizeCpu = [this](int cpu_id, bool online,
                                  const CpuCounterSnapshot *current) {
    CpuStatistics stat;
    stat.cpu_id = cpu_id;
    stat.online = online;
    if (current != nullptr) {
      const auto prev = previous_.find(cpu_id);
      if (prev == previous_.end()) {
        previous_[cpu_id] = *current;  // first sighting: establish a baseline
      } else if (current->total() < prev->second.total()) {
        previous_[cpu_id] = *current;  // counters reset: re-baseline only
      } else {
        computeCpuDelta(prev->second, *current, stat.delta);
        previous_[cpu_id] = *current;
        stat.has_sample = stat.delta.valid;
      }
      if (online && cpu_id >= 0) {
        stat.frequency = readCpuFrequency(cpu_id, root_);
      }
    } else {
      // The CPU is not in /proc/stat (offline): drop its baseline so that it
      // returning online starts from a fresh, honest baseline.
      previous_.erase(cpu_id);
    }
    return stat;
  };

  if (snap.proc_stat_readable) {
    const auto aggregate = data.counters.find(-1);
    if (aggregate != data.counters.end()) {
      snap.aggregate = finalizeCpu(-1, true, &aggregate->second);
    }
  }

  snap.cpus.reserve(universe.size());
  for (int cpu_id : universe) {
    const auto it = data.counters.find(cpu_id);
    const bool online =
        it != data.counters.end() ||
        (topology_.online_available && online_set.count(cpu_id) != 0);
    snap.cpus.push_back(finalizeCpu(
        cpu_id, online, it != data.counters.end() ? &it->second : nullptr));
  }

  return snap;
}

}  // namespace atm