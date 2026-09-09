#include "process_details.hpp"

#include <pwd.h>
#include <sys/resource.h>

#include <charconv>
#include <chrono>
#include <climits>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>

#include "process_memory_map.hpp"
#include "process_network.hpp"
#include "process_resources.hpp"
#include "process_scheduling.hpp"

namespace atm {

namespace {

namespace fs = std::filesystem;

/// Number of clock ticks per second (USER_HZ / _SC_CLK_TCK), usually 100.
long ticksPerSecond() {
  const long ticks = ::sysconf(_SC_CLK_TCK);
  return ticks > 0 ? ticks : 100;
}

/// Page size in bytes (sysconf(_SC_PAGESIZE)); defaulting to 4096 on failure.
std::uint64_t pageSizeBytes() {
  const long page = ::sysconf(_SC_PAGESIZE);
  return page > 0 ? static_cast<std::uint64_t>(page) : 4096ULL;
}

/// Reads a whole file into a string, or std::nullopt when it cannot be opened
/// or read through (e.g. the process exited, or access was denied).
std::optional<std::string> readFile(std::string_view path) {
  std::ifstream in{std::string(path)};
  if (!in.is_open()) {
    return std::nullopt;
  }
  std::ostringstream contents;
  contents << in.rdbuf();
  if (in.bad()) {
    return std::nullopt;
  }
  return contents.str();
}

/// Parses a leading unsigned 64-bit integer; returns std::nullopt when the
/// view is empty or does not begin with a digit.
std::optional<std::uint64_t> parseU64(std::string_view view) {
  if (view.empty()) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  const char *begin = view.data();
  const char *end = view.data() + view.size();
  const auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc{} || result.ptr == begin) {
    return std::nullopt;
  }
  return value;
}

/// Parses a leading signed integer; returns std::nullopt on empty/invalid.
std::optional<long> parseInt(std::string_view view) {
  if (view.empty()) {
    return std::nullopt;
  }
  long value = 0;
  const char *begin = view.data();
  const char *end = view.data() + view.size();
  const auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc{} || result.ptr == begin) {
    return std::nullopt;
  }
  return value;
}

/// Resolves the username for a UID without throwing. Returns an empty string
/// when the user cannot be resolved (e.g. no passwd entry for a system UID).
std::string userName(uid_t uid) {
  // 256 chars is enough for a passwd entry; getpwuid_r never returns a
  // truncated struct, only an overflowing buffer via ERANGE.
  char buffer[256];
  struct passwd pwd;
  struct passwd *result = nullptr;
  if (::getpwuid_r(uid, &pwd, buffer, sizeof(buffer), &result) != 0 ||
      result == nullptr) {
    return {};
  }
  return result->pw_name ? std::string(result->pw_name) : std::string{};
}

/// Follows the /proc/<pid>/exe symbolic link. Returns the target path, or a
/// marker describing why it could not be resolved. Never throws.
std::string readLinkTarget(const std::string &path) {
  char buffer[PATH_MAX];
  const ssize_t length = ::readlink(path.c_str(), buffer, sizeof(buffer) - 1);
  if (length < 0) {
    if (errno == EACCES || errno == EPERM) {
      return "Permission denied";
    }
    if (errno == ENOENT) {
      return "Process no longer exists";
    }
    return "N/A";
  }
  buffer[length] = '\0';
  std::string target(buffer);
  // A deleted executable is reported by the kernel as "<path> (deleted)".
  return target;
}

/// Parsed fields of /proc/<pid>/stat that the detail collector needs.
struct StatData {
  std::string comm;
  char state = '?';
  pid_t ppid = 0;
  std::uint64_t utime = 0;
  std::uint64_t stime = 0;
  std::uint32_t num_threads = 0;
  std::optional<int> priority;
  std::optional<int> nice;
  std::optional<std::uint64_t> starttime_ticks;
};

/**
 * Parses a /proc/<pid>/stat line. The comm field is wrapped in parentheses and
 * may itself contain spaces or parentheses, so it is located between the first
 * '(' and the last ')'. After that closing ')' every field is
 * whitespace-separated; index N there maps to kernel field N + 3, so with the
 * two leading fields (pid, comm) skipped: 0=state(3), 1=ppid(4), 11=utime(14),
 * 12=stime(15), 15=priority(18), 16=nice(19), 17=num_threads(20),
 * 19=starttime(22).
 */
std::optional<StatData> parseStat(std::string_view line) {
  const std::size_t open = line.find('(');
  const std::size_t close = line.rfind(')');
  if (open == std::string_view::npos || close == std::string_view::npos ||
      close < open) {
    return std::nullopt;
  }

  std::vector<std::string> fields;
  {
    std::istringstream tail{std::string(line.substr(close + 1))};
    std::string field;
    while (tail >> field) {
      fields.push_back(field);
    }
  }
  if (fields.size() < 13) {
    return std::nullopt;
  }

  StatData data;
  data.comm = std::string(line.substr(open + 1, close - open - 1));
  if (!fields[0].empty()) {
    data.state = fields[0].front();
  }
  data.ppid = static_cast<pid_t>(
      parseInt(fields[1]).value_or(0));
  data.utime = parseU64(fields[11]).value_or(0);
  data.stime = parseU64(fields[12]).value_or(0);
  if (fields.size() > 17) {
    data.num_threads = static_cast<std::uint32_t>(
        parseU64(fields[17]).value_or(0));
  }
  if (fields.size() > 15) {
    data.priority = static_cast<int>(parseInt(fields[15]).value_or(0));
  }
  if (fields.size() > 16) {
    data.nice = static_cast<int>(parseInt(fields[16]).value_or(0));
  }
  if (fields.size() > 19) {
    data.starttime_ticks = parseU64(fields[19]);
  }
  return data;
}

/// Fill the memory-split and context-switch values that come from
/// /proc/<pid>/status.
struct StatusData {
  char state = '?';
  std::optional<uid_t> uid;
  std::optional<gid_t> gid;
  std::uint32_t threads = 0;
  std::uint64_t vm_size_kib = 0;
  std::uint64_t vm_rss_kib = 0;
  std::uint64_t vm_shared_kib = 0;
  std::uint64_t vm_exe_kib = 0;
  std::uint64_t vm_data_kib = 0;
  std::uint64_t vm_stk_kib = 0;
  std::optional<std::uint64_t> voluntary_ctx;
  std::optional<std::uint64_t> nonvoluntary_ctx;
};

/// Parses /proc/<pid>/status for the fields the detail collector shows.
/// Unrecognised lines are skipped; missing values keep their defaults so a
/// malformed or truncated file still yields the fields that were present.
StatusData parseStatus(std::string_view contents) {
  StatusData data;
  std::istringstream lines{std::string(contents)};
  std::string line;
  while (std::getline(lines, line)) {
    std::istringstream head(line);
    std::string key;
    if (!(head >> key)) {
      continue;
    }
    if (key == "State:") {
      std::string value;
      if (head >> value && !value.empty()) {
        data.state = value.front();
      }
    } else if (key == "Uid:") {
      unsigned long value = 0;
      if (head >> value) {
        data.uid = static_cast<uid_t>(value);  // real UID (first on the line)
      }
    } else if (key == "Gid:") {
      unsigned long value = 0;
      if (head >> value) {
        data.gid = static_cast<gid_t>(value);  // real GID (first on the line)
      }
    } else if (key == "Threads:") {
      std::uint32_t value = 0;
      if (head >> value) {
        data.threads = value;
      }
    } else if (key == "VmSize:") {
      std::uint64_t value = 0;
      std::string unit;
      if (head >> value >> unit) {
        data.vm_size_kib = value;
      }
    } else if (key == "VmRSS:") {
      std::uint64_t value = 0;
      std::string unit;
      if (head >> value >> unit) {
        data.vm_rss_kib = value;
      }
    } else if (key == "RssShmem:" || key == "RssFile:") {
      // Shared memory: the kernel exposes RssShmem and RssFile; the sum is the
      // resident shared portion. Prefer the documented sum where available.
    } else if (key == "VmExe:") {
      std::uint64_t value = 0;
      std::string unit;
      if (head >> value >> unit) {
        data.vm_exe_kib = value;
      }
    } else if (key == "VmData:") {
      std::uint64_t value = 0;
      std::string unit;
      if (head >> value >> unit) {
        data.vm_data_kib = value;
      }
    } else if (key == "VmStk:") {
      std::uint64_t value = 0;
      std::string unit;
      if (head >> value >> unit) {
        data.vm_stk_kib = value;
      }
    } else if (key == "voluntary_ctxt_switches:") {
      std::uint64_t value = 0;
      if (head >> value) {
        data.voluntary_ctx = value;
      }
    } else if (key == "nonvoluntary_ctxt_switches:") {
      std::uint64_t value = 0;
      if (head >> value) {
        data.nonvoluntary_ctx = value;
      }
    }
  }
  return data;
}

/// Parsed /proc/<pid>/io values. Any of them may be missing for a given
/// process.
struct IoData {
  std::optional<std::uint64_t> read_bytes;
  std::optional<std::uint64_t> write_bytes;
  std::optional<std::uint64_t> read_syscalls;
  std::optional<std::uint64_t> write_syscalls;
  std::optional<std::uint64_t> cancelled_write_bytes;
};

/// Parses /proc/<pid>/io; unrecognised lines are skipped. Returns an IoData
/// with only the fields the kernel actually provided.
IoData parseIo(std::string_view contents) {
  IoData data;
  std::istringstream lines{std::string(contents)};
  std::string line;
  while (std::getline(lines, line)) {
    std::istringstream head(line);
    std::string key;
    if (!(head >> key)) {
      continue;
    }
    std::uint64_t value = 0;
    if (key == "rchar:" && head >> value) {
      data.read_bytes = value;
    } else if (key == "wchar:" && head >> value) {
      data.write_bytes = value;
    } else if (key == "syscr:" && head >> value) {
      data.read_syscalls = value;
    } else if (key == "syscw:" && head >> value) {
      data.write_syscalls = value;
    } else if (key == "cancelled_write_bytes:" && head >> value) {
      data.cancelled_write_bytes = value;
    }
  }
  return data;
}

/// Parses /proc/<pid>/statm: one line with page counts for size, resident,
/// shared, text, lib, data, dt. Returns nullopt when the line is malformed or
/// the process vanished.
std::optional<std::vector<std::uint64_t>> parseStatm(std::string_view contents) {
  std::vector<std::uint64_t> pages;
  std::istringstream stream{std::string(contents)};
  std::uint64_t value = 0;
  while (stream >> value) {
    pages.push_back(value);
  }
  if (pages.empty()) {
    return std::nullopt;
  }
  return pages;
}

/// Reads the first token of /proc/uptime (whole seconds since boot).
std::optional<std::uint64_t> systemUptimeSeconds() {
  const std::optional<std::string> contents = readFile("/proc/uptime");
  if (!contents) {
    return std::nullopt;
  }
  std::istringstream stream(*contents);
  std::string token;
  stream >> token;
  if (token.empty()) {
    return std::nullopt;
  }
  const std::size_t dot = token.find('.');
  if (dot != std::string::npos) {
    token = token.substr(0, dot);
  }
  return parseU64(token);
}

}  // namespace

std::optional<ProcessDetailsInfo>
ProcessDetails::getProcessDetails(pid_t pid, std::uint64_t system_total_kib,
                                  std::optional<double> cpu_percent) {
  if (pid <= 0) {
    return std::nullopt;
  }
  const std::string dir = "/proc/" + std::to_string(pid);

  ProcessDetailsInfo info;
  info.pid = pid;

  const std::optional<std::string> stat = readFile(dir + "/stat");
  if (!stat) {
    return std::nullopt;  // /proc/<pid> is gone: "Process not found."
  }
  const std::optional<StatData> stat_data = parseStat(*stat);
  if (!stat_data) {
    return std::nullopt;  // malformed stat — treat as not found
  }

  info.name = stat_data->comm;
  info.state_char = stat_data->state;
  info.state = processStateFromChar(stat_data->state);
  info.parent_pid = stat_data->ppid;
  info.user_cpu_time = stat_data->utime;
  info.system_cpu_time = stat_data->stime;
  info.total_cpu_time = stat_data->utime + stat_data->stime;
  info.priority = stat_data->priority;
  info.nice_value = stat_data->nice;
  info.thread_count = stat_data->num_threads;
  info.starttime_ticks = stat_data->starttime_ticks;

  // Command line (NUL-separated argv). Empty for kernel threads.
  if (const std::optional<std::string> raw = readFile(dir + "/cmdline"); raw) {
    std::string command_line;
    command_line.reserve(raw->size());
    for (const char c : *raw) {
      command_line.push_back(c == '\0' ? ' ' : c);
    }
    while (!command_line.empty() && command_line.back() == ' ') {
      command_line.pop_back();
    }
    info.command_line = std::move(command_line);
  }

  // Status: state fallback, UID/GID, threads, memory split, context switches.
  const std::optional<std::string> status = readFile(dir + "/status");
  if (status) {
    const StatusData status_data = parseStatus(*status);
    if (info.state == ProcessState::Unknown && status_data.state != '?') {
      info.state_char = status_data.state;
      info.state = processStateFromChar(status_data.state);
    }
    info.uid = status_data.uid;
    info.gid = status_data.gid;
    if (status_data.threads != 0) {
      info.thread_count = status_data.threads;
    }
    info.virtual_memory_bytes = status_data.vm_size_kib * 1024ULL;
    info.resident_memory_bytes = status_data.vm_rss_kib * 1024ULL;
    info.shared_memory_bytes = status_data.vm_shared_kib * 1024ULL;
    // Some kernels do not provide a shared count; leave it unset when zero and
    // unknown, letting the UI show "N/A".
    if (status_data.vm_shared_kib != 0) {
      info.shared_memory_bytes = status_data.vm_shared_kib * 1024ULL;
    }
    info.text_memory_bytes = status_data.vm_exe_kib * 1024ULL;
    info.data_memory_bytes = status_data.vm_data_kib * 1024ULL;
    info.stack_memory_bytes = status_data.vm_stk_kib * 1024ULL;
    info.voluntary_context_switches = status_data.voluntary_ctx;
    info.nonvoluntary_context_switches = status_data.nonvoluntary_ctx;
  }

  // /proc/<pid>/statm provides a more authoritative page-based memory split
  // (size, resident, shared, text, data). When present, use it to fill any
  // memory fields the status file did not give us. Values are page counts.
  if (const std::optional<std::string> statm = readFile(dir + "/statm"); statm) {
    if (const std::optional<std::vector<std::uint64_t>> pages =
            parseStatm(*statm);
        pages) {
      const std::uint64_t page_bytes = pageSizeBytes();
      // statm resident is the same as VmRSS; use it only as a cross-check and
      // prefer the /proc/<pid>/status value for consistency with the table.
      if (pages->size() > 0) {
        info.virtual_memory_bytes =
            pages->at(0) * page_bytes;  // total program size in bytes
      }
      if (pages->size() > 2 && pages->at(2) != 0) {
        info.shared_memory_bytes = pages->at(2) * page_bytes;
      }
      if (pages->size() > 3) {
        info.text_memory_bytes = pages->at(3) * page_bytes;
      }
      if (pages->size() > 5) {
        info.data_memory_bytes = pages->at(5) * page_bytes;
      }
    }
  }

  // Memory percentage mirrors ProcessMonitor: RSS / system total × 100.
  if (system_total_kib != 0) {
    info.memory_percent =
        static_cast<double>(info.resident_memory_bytes / 1024ULL) * 100.0 /
        static_cast<double>(system_total_kib);
  }

  // Reuse the Process Monitor's CPU figure when the caller supplies it.
  info.cpu_usage_percent = cpu_percent;

  // Executable and working directory via readlink(2).
  info.executable_path = readLinkTarget(dir + "/exe");
  info.working_directory = readLinkTarget(dir + "/cwd");
  // Only show a "working directory" value if it is a real directory path; for
  // permission-denied it stays as the marker string produced above.
  if (info.working_directory == "Process no longer exists") {
    info.working_directory = "N/A";
  }

  // Resolve the username from the real UID.
  if (info.uid.has_value()) {
    info.user = userName(*info.uid);
  }

  // Niceness via getpriority(2); a negative niceness is valid, so errno must
  // disambiguate -1 from an error.
  errno = 0;
  const int priority = ::getpriority(PRIO_PROCESS, pid);
  if (!(priority == -1 && errno != 0)) {
    info.nice_priority = priority;
  }

  // I/O statistics from /proc/<pid>/io (permission-dependent).
  ProcessIoCounters current_io;
  if (const std::optional<std::string> io = readFile(dir + "/io"); io) {
    const IoData io_data = parseIo(*io);
    info.read_bytes = io_data.read_bytes;
    info.write_bytes = io_data.write_bytes;
    info.read_syscalls = io_data.read_syscalls;
    info.write_syscalls = io_data.write_syscalls;
    info.cancelled_write_bytes = io_data.cancelled_write_bytes;
    current_io = ProcessIoCounters{
        true, io_data.read_bytes.value_or(0), io_data.write_bytes.value_or(0),
        io_data.read_syscalls.value_or(0), io_data.write_syscalls.value_or(0),
        io_data.cancelled_write_bytes.value_or(0)};
  }

  // I/O rates: delta over the elapsed window, gated by process identity so a
  // restarted/reused PID never produces a spurious rate.
  if (current_io.available && stat_data->starttime_ticks.has_value()) {
    const auto now = std::chrono::steady_clock::now();
    const Identity identity{pid, *stat_data->starttime_ticks};
    const auto previous = previous_io_.find(identity);
    const bool same_process =
        previous != previous_io_.end() && has_previous_scan_;
    const double elapsed =
        has_previous_scan_
            ? std::chrono::duration<double>(now - previous_scan_time_).count()
            : 0.0;
    const ProcessIoCounters prev_io =
        same_process ? previous->second : ProcessIoCounters{};
    const IoRates rates =
        computeIoRates(same_process, prev_io, current_io, elapsed);
    info.read_rate = rates.read_rate;
    info.write_rate = rates.write_rate;

    // Record the baseline for the next inspection of this identity.
    previous_io_[identity] = current_io;
    previous_scan_time_ = now;
    has_previous_scan_ = true;
  }

  // Read-only resource limits from /proc/<pid>/limits. Strictly observational;
  // the application never modifies them.
  if (const std::optional<std::string> limits = readFile(dir + "/limits");
      limits) {
    info.limits = parseProcessLimits(*limits);
  }

  // Scheduling state (read-only observation). The CPU affinity comes from the
  // kernel via sched_getaffinity(2); the online CPU count gives the UI the
  // valid id range. Neither value is ever changed by the collector — editing
  // happens only through ProcessSchedulingManager after explicit user
  // confirmation.
  {
    const ProcessSchedulingManager scheduling;
    if (const std::optional<std::vector<int>> affinity =
            scheduling.getCpuAffinity(pid);
        affinity) {
      info.allowed_cpus = affinity;
    }
    info.system_cpu_count = ProcessSchedulingManager::systemCpuCount();
  }

  // Memory mappings (read-only metadata) from /proc/<pid>/maps, collected on
  // this same refresh pass and gated by the process identity (PID + start
  // time) so a reused PID never shows another process's mappings. Reading the
  // maps belongs to the inspector refresh lifecycle only: the process monitor
  // never scans /proc/*/maps.
  if (info.starttime_ticks.has_value()) {
    const ProcessMemoryMapManager memory_maps;
    info.memory_maps = memory_maps.inspect(
        ProcessIdentity{pid, *info.starttime_ticks});
  }

  // Network connections (read-only metadata) from /proc/<pid>/fd and
  // /proc/net/*, collected on this same refresh pass and gated by the process
  // identity so a reused PID never shows another process's connections. Reading
  // the tables belongs to the inspector refresh lifecycle only.
  if (info.starttime_ticks.has_value()) {
    const ProcessNetworkConnectionManager connections;
    info.network_connections = connections.inspect(
        ProcessIdentity{pid, *info.starttime_ticks});
  }

  // Start time: boot wall-clock + (starttime ticks / USER_HZ). Running time =
  // system uptime − (starttime ticks / USER_HZ).
  if (stat_data->starttime_ticks.has_value()) {
    const double ticks = static_cast<double>(*stat_data->starttime_ticks);
    const double ticks_hz = ticksPerSecond();
    const std::uint64_t boot_uptime = systemUptimeSeconds().value_or(0);
    const std::uint64_t start_seconds_since_boot =
        static_cast<std::uint64_t>(ticks / ticks_hz);

    const auto now = std::chrono::system_clock::now();
    if (boot_uptime != 0) {
      const auto boot_time =
          now - std::chrono::seconds(static_cast<std::int64_t>(boot_uptime));
      info.start_time =
          boot_time + std::chrono::seconds(
                          static_cast<std::int64_t>(start_seconds_since_boot));
    }
    if (boot_uptime >= start_seconds_since_boot) {
      info.process_uptime_seconds = boot_uptime - start_seconds_since_boot;
    }
  }

  return info;
}

void ProcessDetails::forgetBaseline(pid_t pid) {
  for (auto it = previous_io_.begin(); it != previous_io_.end();) {
    if (it->first.pid == pid) {
      it = previous_io_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace atm
