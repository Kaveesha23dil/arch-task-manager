#include "process_monitor.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>

#include "cpu_monitor.hpp"
#include "process_resources.hpp"

namespace atm {

namespace {

namespace fs = std::filesystem;

/// Reads a whole file into a string, or std::nullopt when it cannot be
/// opened or read through to the end (e.g. the process exited meanwhile).
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

/// True when every character is ASCII '0'..'9'. /proc/<pid> directories are
/// the only entries whose names are all digits.
bool isAllDigits(std::string_view value) {
  return !value.empty() && std::all_of(value.begin(), value.end(), [](char c) {
           return c >= '0' && c <= '9';
         });
}

/// Parses a leading integer without throwing; returns 0 on failure.
int parseInt(std::string_view view) {
  int result = 0;
  std::from_chars(view.data(), view.data() + view.size(), result);
  return result;
}

/// Reads /proc/<pid>/cmdline (NUL-separated argv) joined with spaces. Returns
/// an empty string for kernel threads, whose cmdline is empty.
std::string readCommandLine(std::string_view dir) {
  const std::optional<std::string> raw =
      readFile(std::string(dir) + "/cmdline");
  if (!raw) {
    return {};
  }
  std::string command;
  command.reserve(raw->size());
  for (const char c : *raw) {
    command.push_back(c == '\0' ? ' ' : c);
  }
  while (!command.empty() && command.back() == ' ') {
    command.pop_back();
  }
  return command;
}

/// Reads a single process from `/proc/<pid>`, or std::nullopt when the entry
/// is gone or unreadable. Never throws.
std::optional<Process> readProcess(const std::string &dir, int pid,
                                   std::uint64_t system_total_kib) {
  const std::optional<std::string> stat = readFile(dir + "/stat");
  if (!stat) {
    return std::nullopt;
  }
  const std::optional<ProcessStatParse> stat_data =
      parseProcessStat(*stat);
  if (!stat_data) {
    return std::nullopt;  // malformed stat — treat as invalid
  }

  Process process;
  process.pid = pid;
  process.name = stat_data->comm;
  process.state_char = stat_data->state;
  process.state = processStateFromChar(stat_data->state);
  process.parent_pid = stat_data->ppid;
  process.thread_count = stat_data->num_threads;
  process.cpu_ticks = stat_data->utime + stat_data->stime;
  process.user_cpu_ticks = stat_data->utime;
  process.system_cpu_ticks = stat_data->stime;
  process.starttime_ticks = stat_data->starttime_ticks;
  process.command_line = readCommandLine(dir);

  const std::optional<std::string> status = readFile(dir + "/status");
  if (status) {
    const ProcessStatusParse status_data = parseProcessStatus(*status);
    if (process.state == ProcessState::Unknown && status_data.state != '?') {
      process.state_char = status_data.state;
      process.state = processStateFromChar(status_data.state);
    }
    // Uid line is always present for a visible process; the real UID from
    // status takes precedence over the default. (0 is a valid UID: root.)
    process.uid = status_data.uid;
    if (status_data.threads != 0) {
      process.thread_count = status_data.threads;
    }
    process.memory_kib = status_data.vm_rss_kib;
    process.shared_memory_kib = status_data.shared_kib;
  }

  // /proc/<pid>/io provides per-process I/O counters. Permission-dependent:
  // when it cannot be read (EACCES/EPERM, or the process vanished) the
  // counters simply stay unavailable — never an application-wide failure.
  if (const std::optional<std::string> io = readFile(dir + "/io"); io) {
    const ProcessIoCounters io_data = parseProcessIo(*io);
    process.io_available = io_data.available;
    process.read_bytes = io_data.read_bytes;
    process.write_bytes = io_data.write_bytes;
    process.read_syscalls = io_data.read_syscalls;
    process.write_syscalls = io_data.write_syscalls;
  }

  if (system_total_kib != 0) {
    process.memory_percent =
        static_cast<double>(process.memory_kib) * 100.0 /
        static_cast<double>(system_total_kib);
  }
  return process;
}

/// Case-insensitive, locale-independent name comparison for the Name sort.
bool nameLess(const std::string &a, const std::string &b) {
  const std::size_t n = std::min(a.size(), b.size());
  for (std::size_t i = 0; i < n; ++i) {
    const char ca = static_cast<char>(
        std::tolower(static_cast<unsigned char>(a[i])));
    const char cb = static_cast<char>(
        std::tolower(static_cast<unsigned char>(b[i])));
    if (ca != cb) {
      return ca < cb;
    }
  }
  return a.size() < b.size();
}

/// Counts the process population by state.
ProcessStats computeStats(const std::vector<Process> &processes) {
  ProcessStats stats;
  stats.total = processes.size();
  for (const Process &process : processes) {
    switch (process.state) {
      case ProcessState::Running:
        ++stats.running;
        break;
      case ProcessState::Sleeping:
      case ProcessState::Idle:
      case ProcessState::DiskSleep:
        ++stats.sleeping;  // "Disk Sleep" and "Idle" are sleeping-like states
        break;
      case ProcessState::Stopped:
        ++stats.stopped;
        break;
      case ProcessState::Zombie:
        ++stats.zombie;
        break;
      case ProcessState::Unknown:
        break;
    }
  }
  return stats;
}

}  // namespace

ProcessState processStateFromChar(char state) {
  switch (state) {
    case 'R':
      return ProcessState::Running;
    case 'S':
      return ProcessState::Sleeping;
    case 'D':
      return ProcessState::DiskSleep;
    case 'T':
    case 't':
      return ProcessState::Stopped;
    case 'Z':
    case 'X':
    case 'x':
      return ProcessState::Zombie;
    case 'I':
      return ProcessState::Idle;
    default:
      return ProcessState::Unknown;
  }
}

const char *processStateName(ProcessState state) {
  switch (state) {
    case ProcessState::Running:
      return "Running";
    case ProcessState::Sleeping:
      return "Sleeping";
    case ProcessState::DiskSleep:
      return "Disk Sleep";
    case ProcessState::Stopped:
      return "Stopped";
    case ProcessState::Zombie:
      return "Zombie";
    case ProcessState::Idle:
      return "Idle";
    case ProcessState::Unknown:
      return "Unknown";
  }
  return "Unknown";
}

void sortProcesses(std::vector<Process> &processes, ProcessSort sort) {
  switch (sort) {
    case ProcessSort::Cpu:
      std::stable_sort(processes.begin(), processes.end(),
                       [](const Process &a, const Process &b) {
                         if (a.cpu_percent != b.cpu_percent) {
                           return a.cpu_percent > b.cpu_percent;
                         }
                         return a.pid < b.pid;
                       });
      break;
    case ProcessSort::Memory:
      std::stable_sort(processes.begin(), processes.end(),
                       [](const Process &a, const Process &b) {
                         if (a.memory_kib != b.memory_kib) {
                           return a.memory_kib > b.memory_kib;
                         }
                         return a.pid < b.pid;
                       });
      break;
    case ProcessSort::Pid:
      std::stable_sort(processes.begin(), processes.end(),
                       [](const Process &a, const Process &b) {
                         return a.pid < b.pid;
                       });
      break;
    case ProcessSort::Name:
      std::stable_sort(processes.begin(), processes.end(),
                       [](const Process &a, const Process &b) {
                         if (a.name != b.name) {
                           return nameLess(a.name, b.name);
                         }
                         return a.pid < b.pid;
                       });
      break;
    case ProcessSort::Threads:
      std::stable_sort(processes.begin(), processes.end(),
                       [](const Process &a, const Process &b) {
                         if (a.thread_count != b.thread_count) {
                           return a.thread_count > b.thread_count;
                         }
                         return a.pid < b.pid;
                       });
      break;
    case ProcessSort::ReadRate:
      std::stable_sort(processes.begin(), processes.end(),
                       [](const Process &a, const Process &b) {
                         if (a.read_rate != b.read_rate) {
                           return a.read_rate > b.read_rate;
                         }
                         return a.pid < b.pid;
                       });
      break;
    case ProcessSort::WriteRate:
      std::stable_sort(processes.begin(), processes.end(),
                       [](const Process &a, const Process &b) {
                         if (a.write_rate != b.write_rate) {
                           return a.write_rate > b.write_rate;
                         }
                         return a.pid < b.pid;
                       });
      break;
  }
}

const char *processSortName(ProcessSort sort) {
  switch (sort) {
    case ProcessSort::Cpu:
      return "CPU";
    case ProcessSort::Memory:
      return "Memory";
    case ProcessSort::Pid:
      return "PID";
    case ProcessSort::Name:
      return "Name";
    case ProcessSort::Threads:
      return "Threads";
    case ProcessSort::ReadRate:
      return "Read Rate";
    case ProcessSort::WriteRate:
      return "Write Rate";
  }
  return "CPU";
}

ProcessSnapshot ProcessMonitor::read(std::uint64_t system_total_kib) {
  ProcessSnapshot snapshot;

  // Sample the system-wide CPU counter once for this scan and, if we have a
  // previous sample, derive a ticks-per-percent scale. 100% means exactly one
  // full online CPU (thread): process_delta / total_delta * num_cpus * 100.
  const std::optional<CpuTimes> total = readCpuTimes();
  const std::optional<std::uint64_t> current_total =
      total.has_value() ? std::optional<std::uint64_t>(total->total())
                        : std::nullopt;

  long online = ::sysconf(_SC_NPROCESSORS_ONLN);
  const std::uint64_t num_cpus =
      online > 0 ? static_cast<std::uint64_t>(online) : 1;

  std::optional<double> ticks_to_percent;
  if (previous_total_ticks_.has_value() && current_total.has_value()) {
    const std::uint64_t total_delta = *current_total - *previous_total_ticks_;
    if (total_delta != 0) {
      ticks_to_percent =
          static_cast<double>(num_cpus) * 100.0 / static_cast<double>(total_delta);
    }
  }

  // Timestamp for the elapsed window used by I/O rate calculation.
  const auto now = std::chrono::steady_clock::now();

  // Enumerate /proc and read every numeric directory as a process.
  try {
    const fs::directory_options options =
        fs::directory_options::skip_permission_denied;
    for (const fs::directory_entry &entry :
         fs::directory_iterator("/proc", options)) {
      const std::string filename = entry.path().filename().string();
      if (!isAllDigits(filename)) {
        continue;
      }
      const int pid = parseInt(filename);
      if (pid <= 0) {
        continue;
      }

      // /proc/<pid> may disappear right here; readProcess() returns nullopt
      // and the process is silently skipped.
      std::optional<Process> process =
          readProcess(entry.path().string(), pid, system_total_kib);
      if (!process.has_value()) {
        continue;
      }

      // Per-process CPU usage is the tick delta since the previous scan,
      // scaled so that a full core reads as 100%.
      if (ticks_to_percent.has_value()) {
        const auto previous = previous_ticks_.find(process->pid);
        if (previous != previous_ticks_.end() &&
            process->cpu_ticks >= previous->second) {
          process->cpu_percent =
              static_cast<double>(process->cpu_ticks - previous->second) *
              *ticks_to_percent;
        }
      }

      // I/O rates are the I/O-counter delta over the elapsed window, gated by
      // process identity so a reused/restarted PID never reports a bogus rate.
      if (process->io_available && has_previous_scan_) {
        const Identity identity{pid, process->starttime_ticks};
        const auto previous = previous_io_.find(identity);
        const bool same_process = previous != previous_io_.end();
        const double elapsed_seconds = std::chrono::duration<double>(
            now - previous_scan_time_).count();
        const ProcessIoCounters current_io{
            true, process->read_bytes, process->write_bytes,
            process->read_syscalls, process->write_syscalls,
            /*cancelled=*/0};
        const ProcessIoCounters prev_io =
            same_process ? previous->second : ProcessIoCounters{};
        const IoRates rates =
            computeIoRates(same_process, prev_io, current_io, elapsed_seconds);
        process->read_rate = rates.read_rate;
        process->write_rate = rates.write_rate;
      }

      snapshot.processes.push_back(std::move(*process));
    }
  } catch (const std::exception &) {
    // The /proc scan itself failed (path removed, etc.): return whatever has
    // been collected so far instead of crashing.
  }

  // Record samples for the next read to diff against.
  previous_ticks_.clear();
  previous_ticks_.reserve(snapshot.processes.size());
  previous_io_.clear();
  previous_io_.reserve(snapshot.processes.size());
  for (const Process &process : snapshot.processes) {
    previous_ticks_.emplace(process.pid, process.cpu_ticks);
    if (process.io_available) {
      previous_io_.emplace(
          Identity{process.pid, process.starttime_ticks},
          ProcessIoCounters{true, process.read_bytes, process.write_bytes,
                            process.read_syscalls, process.write_syscalls,
                            /*cancelled=*/0});
    }
  }
  previous_scan_time_ = now;
  has_previous_scan_ = true;

  if (current_total.has_value()) {
    previous_total_ticks_ = current_total;
  } else {
    // /proc/stat was unreadable; drop the baseline so the next successful
    // scan starts clean instead of reporting a huge spurious delta.
    previous_total_ticks_.reset();
  }

  snapshot.stats = computeStats(snapshot.processes);
  return snapshot;
}

}  // namespace atm