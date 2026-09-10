#include "process_report.hpp"

#include <errno.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "format_bytes.hpp"
#include "process_cgroup.hpp"
#include "process_details.hpp"
#include "process_environment.hpp"
#include "process_io_details.hpp"
#include "process_memory_map.hpp"
#include "process_monitor.hpp"
#include "process_namespace.hpp"
#include "process_network.hpp"
#include "process_resources.hpp"
#include "process_scheduling.hpp"
#include "process_security.hpp"

namespace atm {

namespace {

/// Deterministic local timestamp "YYYY-MM-DD HH:MM:SS". Uses the C++ time
/// facilities and localtime_r; never shells out to `date` or any tool.
std::string formatTimePoint(std::chrono::system_clock::time_point tp) {
  const std::time_t seconds = std::chrono::system_clock::to_time_t(tp);
  std::tm local{};
  if (::localtime_r(&seconds, &local) == nullptr) {
    return "N/A";
  }
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local);
  return buffer;
}

std::string nowTimestamp() {
  return formatTimePoint(std::chrono::system_clock::now());
}

/// "Xd Xh Xm Xs" style duration, matching the inspector's rendering.
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

/// "0x..." rendering of an address / id.
std::string hexAddr(std::uint64_t value) {
  std::ostringstream out;
  out << "0x" << std::hex << value;
  return out.str();
}

/// Formats a device id as "major:minor" (hex, zero-padded minor).
std::string formatDevice(std::uint32_t major, std::uint32_t minor) {
  std::ostringstream out;
  out << std::hex << major << ':' << std::setw(2) << std::setfill('0')
      << std::hex << minor;
  return out.str();
}

/// Bounded text sink: everything written is counted; once the total exceeds
/// `max_bytes` further appends are dropped and an explicit truncation marker
/// is appended by finish(). Never throws.
class ReportWriter {
 public:
  explicit ReportWriter(std::size_t max_bytes) : max_bytes_(max_bytes) {}

  void put(std::string_view text) {
    if (over_limit_ || text.empty()) {
      return;
    }
    const std::streampos pos = out_.tellp();
    if (pos >= 0 &&
        static_cast<std::uint64_t>(pos) + text.size() > max_bytes_) {
      over_limit_ = true;
      return;
    }
    out_ << text;
  }

  void line(std::string_view text) {
    put(text);
    put("\n");
  }

  [[nodiscard]] bool truncated() const { return over_limit_; }

  [[nodiscard]] std::string finish() {
    std::string result = out_.str();
    if (over_limit_) {
      result += "\n[truncated: report exceeded the size safety bound]\n";
    }
    return result;
  }

  /// Appends a fixed "Label: value" row, aligned across the report.
  void labeled(const std::string &label, const std::string &value) {
    std::ostringstream row;
    row << std::left << std::setw(kLabelWidth) << label << value;
    line(row.str());
  }

  static constexpr std::size_t kLabelWidth = 24;

 private:
  std::size_t max_bytes_;
  std::ostringstream out_;
  bool over_limit_ = false;
};

/// Appends an explicit "[N omitted — truncated]" row when `total` exceeded
/// the per-section bound.
void appendOmittedNote(ReportWriter &out, std::size_t total,
                       std::size_t shown, std::string_view what) {
  if (total > shown) {
    out.line("[" + std::to_string(total - shown) + " more " +
             std::string(what) + " omitted — truncated]");
  }
}

/// One read-only resource limit row, mirroring the inspector's rendering
/// (soft / hard, "Unlimited" for the RLIM_INFINITY marker).
void appendLimitRow(ReportWriter &out, const std::string &label,
                    const std::optional<ResourceLimit> &limit,
                    bool as_count) {
  if (!limit.has_value() || limit->empty()) {
    out.labeled(label, "N/A");
    return;
  }
  const auto side = [as_count](std::optional<std::uint64_t> value,
                               bool unlimited) {
    if (unlimited) {
      return std::string("Unlimited");
    }
    if (!value.has_value()) {
      return std::string("N/A");
    }
    return as_count ? std::to_string(*value) : formatBytes(*value);
  };
  out.labeled(label,
              side(limit->soft, limit->soft_unlimited) + " / " +
                  side(limit->hard, limit->hard_unlimited));
}

/// Renders a capability bitmask with its decoded CAP_* names.
std::string formatCapabilitySet(const CapabilitySet &set) {
  std::ostringstream out;
  out << capabilitySetTypeName(set.type) << " (" << set.raw_mask << " / "
      << hexAddr(set.raw_mask) << ")";
  if (set.available) {
    if (set.decoded_names.empty() && set.unknown_bits.empty()) {
      out << " — 0";
      return out.str();
    }
    out << " — ";
    bool first = true;
    for (const std::string &name : set.decoded_names) {
      if (!first) {
        out << ", ";
      }
      first = false;
      out << name;
    }
    for (const std::uint32_t bit : set.unknown_bits) {
      if (!first) {
        out << ", ";
      }
      first = false;
      out << "unknown_bit_" << bit;
    }
  } else {
    out << " — unavailable";
  }
  return out.str();
}

std::string formatId(std::optional<std::uint64_t> value) {
  return value.has_value() ? std::to_string(*value) : std::string("N/A");
}

std::string environmentStatusText(EnvironmentStatus status) {
  switch (status) {
    case EnvironmentStatus::Success:
    case EnvironmentStatus::EmptyEnvironment:
      return "loaded";
    case EnvironmentStatus::InvalidPid:
      return "invalid PID";
    case EnvironmentStatus::ProcessNotFound:
      return "process no longer exists";
    case EnvironmentStatus::IdentityUnknown:
      return "the process identity could not be re-read";
    case EnvironmentStatus::ProcessReused:
      return "the PID was reused by a different process";
    case EnvironmentStatus::PermissionDenied:
      return "permission denied";
    case EnvironmentStatus::MalformedData:
      return "the environment data was malformed";
    case EnvironmentStatus::ReadError:
      return "read failed";
  }
  return "unknown";
}

std::string memoryMapStatusText(MemoryMapStatus status) {
  switch (status) {
    case MemoryMapStatus::Success:
      return "loaded";
    case MemoryMapStatus::InvalidPid:
      return "invalid PID";
    case MemoryMapStatus::ProcessNotFound:
      return "process no longer exists";
    case MemoryMapStatus::IdentityUnknown:
      return "the process identity could not be re-read";
    case MemoryMapStatus::ProcessReused:
      return "the PID was reused by a different process";
    case MemoryMapStatus::PermissionDenied:
      return "permission denied";
    case MemoryMapStatus::ReadError:
      return "read failed";
  }
  return "unknown";
}

std::string networkStatusText(NetworkConnectionsStatus status) {
  switch (status) {
    case NetworkConnectionsStatus::Success:
      return "loaded";
    case NetworkConnectionsStatus::InvalidPid:
      return "invalid PID";
    case NetworkConnectionsStatus::ProcessNotFound:
      return "process no longer exists";
    case NetworkConnectionsStatus::IdentityUnknown:
      return "the process identity could not be re-read";
    case NetworkConnectionsStatus::ProcessReused:
      return "the PID was reused by a different process";
    case NetworkConnectionsStatus::PermissionDenied:
      return "permission denied";
    case NetworkConnectionsStatus::ReadError:
      return "read failed";
  }
  return "unknown";
}

std::string namespaceStatusText(NamespaceStatus status) {
  switch (status) {
    case NamespaceStatus::Success:
      return "loaded";
    case NamespaceStatus::InvalidPid:
      return "invalid PID";
    case NamespaceStatus::ProcessNotFound:
      return "process no longer exists";
    case NamespaceStatus::IdentityUnknown:
      return "the process identity could not be re-read";
    case NamespaceStatus::ProcessReused:
      return "the PID was reused by a different process";
    case NamespaceStatus::PermissionDenied:
      return "permission denied";
    case NamespaceStatus::ReadError:
      return "read failed";
  }
  return "unknown";
}

std::string cgroupStatusText(CgroupStatus status) {
  switch (status) {
    case CgroupStatus::Success:
      return "loaded";
    case CgroupStatus::InvalidPid:
      return "invalid PID";
    case CgroupStatus::ProcessNotFound:
      return "process no longer exists";
    case CgroupStatus::IdentityUnknown:
      return "the process identity could not be re-read";
    case CgroupStatus::ProcessReused:
      return "the PID was reused by a different process";
    case CgroupStatus::PermissionDenied:
      return "permission denied";
    case CgroupStatus::Unavailable:
      return "no cgroup filesystem available";
    case CgroupStatus::MalformedData:
      return "the cgroup data was malformed";
    case CgroupStatus::ReadError:
      return "read failed";
  }
  return "unknown";
}

}  // namespace

const char *reportStatusMessage(ReportStatus status) {
  switch (status) {
    case ReportStatus::Success:
      return "report written";
    case ReportStatus::SuccessProcessGone:
      return "report written (the process exited during the export)";
    case ReportStatus::InvalidPid:
      return "invalid PID";
    case ReportStatus::InvalidPath:
      return "invalid destination path";
    case ReportStatus::ProcessReused:
      return "the process identity changed (the PID was reused); no report "
             "was written";
    case ReportStatus::IdentityUnknown:
      return "the process identity could not be re-verified";
    case ReportStatus::WriteError:
      return "failed to write the report";
    case ReportStatus::EmptyReport:
      return "nothing to export";
  }
  return "unknown export status";
}

std::string sanitizeReportName(std::string_view raw) {
  std::string out;
  out.reserve(raw.size());
  // Replace every unsafe byte with '_'. ASCII letters, digits, '_' and '-'
  // survive; '.' is deliberately unsafe so the result can never form "..",
  // hide an extension, or be misread as a directory.
  bool previous_underscore = false;
  for (const unsigned char c : raw) {
    const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!safe) {
      if (!previous_underscore) {
        out.push_back('_');
        previous_underscore = true;
      }
      continue;
    }
    out.push_back(static_cast<char>(c));
    previous_underscore = false;
  }
  while (out.size() > 1 && out.back() == '_') {
    out.pop_back();
  }
  while (!out.empty() && out.front() == '_') {
    out.erase(out.begin());
  }
  if (out.empty()) {
    return "process";
  }
  return out;
}

std::string defaultReportFilename(pid_t pid, std::string_view name) {
  return "process-" + std::to_string(pid) + "-" + sanitizeReportName(name) +
         ".txt";
}

// ---------------------------------------------------------------------------
// Section renderers (all pure formatting of existing models; never /proc).
// ---------------------------------------------------------------------------

namespace {

void renderProcessSection(ReportWriter &out, const ProcessDetailsInfo &info) {
  out.line("[Process]");
  out.line("");
  out.labeled("PID", std::to_string(info.pid));
  out.labeled("Name", info.name.empty() ? std::string("N/A") : info.name);
  out.labeled("State",
              std::string(processStateName(info.state)) + " (code '" +
                  info.state_char + "')");
  out.labeled("User", info.user.empty() ? std::string("N/A") : info.user);
  out.labeled("UID", formatId(info.uid));
  out.labeled("GID", formatId(info.gid));
  out.labeled("Parent PID", std::to_string(info.parent_pid));
  out.labeled("Threads", info.thread_count.has_value()
                             ? std::to_string(*info.thread_count)
                             : std::string("N/A"));
  out.labeled("Priority", info.priority.has_value()
                              ? std::to_string(*info.priority)
                              : std::string("N/A"));
  out.labeled("Start Time",
              info.start_time.has_value() ? formatTimePoint(*info.start_time)
                                          : std::string("N/A"));
  out.labeled("Running For",
              info.process_uptime_seconds.has_value()
                  ? formatDuration(*info.process_uptime_seconds)
                  : std::string("N/A"));
  out.labeled("Start Tick (identity)",
              info.starttime_ticks.has_value()
                  ? std::to_string(*info.starttime_ticks)
                  : std::string("N/A"));
  out.line("");
  out.labeled("Executable",
              info.executable_path.empty() ? std::string("N/A")
                                           : info.executable_path);
  out.labeled("Working Directory",
              info.working_directory.empty() ? std::string("N/A")
                                             : info.working_directory);
  out.line("");
  out.line("Command Line:");
  out.line(info.command_line.empty() ? std::string("N/A") : info.command_line);
  out.line("");
}

void renderCpuMemorySection(ReportWriter &out,
                            const ProcessDetailsInfo &info) {
  out.line("[CPU & Memory]");
  out.line("");
  const long ticks = std::max(1L, ::sysconf(_SC_CLK_TCK));
  out.labeled("CPU Usage",
              info.cpu_usage_percent.has_value()
                  ? (std::to_string(info.cpu_usage_percent.value()) + "%")
                  : std::string("N/A"));
  out.labeled("User CPU Time", formatDuration(info.user_cpu_time /
                                              static_cast<std::uint64_t>(ticks)));
  out.labeled("System CPU Time",
              formatDuration(info.system_cpu_time /
                             static_cast<std::uint64_t>(ticks)));
  out.line("");
  out.labeled("Virtual Memory", formatBytes(info.virtual_memory_bytes));
  out.labeled("Resident Memory", formatBytes(info.resident_memory_bytes));
  out.labeled("Shared Memory",
              info.shared_memory_bytes.has_value()
                  ? formatBytes(*info.shared_memory_bytes)
                  : std::string("N/A"));
  out.labeled("Text Memory",
              info.text_memory_bytes.has_value()
                  ? formatBytes(*info.text_memory_bytes)
                  : std::string("N/A"));
  out.labeled("Data Memory",
              info.data_memory_bytes.has_value()
                  ? formatBytes(*info.data_memory_bytes)
                  : std::string("N/A"));
  out.labeled("Stack Memory",
              info.stack_memory_bytes.has_value()
                  ? formatBytes(*info.stack_memory_bytes)
                  : std::string("N/A"));
  out.labeled("Memory Usage",
              info.memory_percent.has_value()
                  ? (std::to_string(info.memory_percent.value()) + "%")
                  : std::string("N/A"));
  out.line("");
  out.labeled("Voluntary Context Switches",
              info.voluntary_context_switches.has_value()
                  ? std::to_string(*info.voluntary_context_switches)
                  : std::string("N/A"));
  out.labeled("Non-voluntary Context Switches",
              info.nonvoluntary_context_switches.has_value()
                  ? std::to_string(*info.nonvoluntary_context_switches)
                  : std::string("N/A"));
  out.line("");
}

void renderIoSection(ReportWriter &out, const ProcessDetailsInfo &info) {
  out.line("[I/O]");
  out.line("");

  if (info.io_details.has_value()) {
    const ProcessIoDetailsResult &io = *info.io_details;
    if (io.success()) {
      out.labeled("Characters Read",
                  io.info.chars_read.has_value()
                      ? formatBytes(*io.info.chars_read)
                      : std::string("N/A"));
      out.labeled("Characters Written",
                  io.info.chars_written.has_value()
                      ? formatBytes(*io.info.chars_written)
                      : std::string("N/A"));
      out.labeled("Read System Calls",
                  io.info.read_syscalls.has_value()
                      ? std::to_string(*io.info.read_syscalls)
                      : std::string("N/A"));
      out.labeled("Write System Calls",
                  io.info.write_syscalls.has_value()
                      ? std::to_string(*io.info.write_syscalls)
                      : std::string("N/A"));
      out.labeled("Bytes Read",
                  io.info.bytes_read.has_value()
                      ? formatBytes(*io.info.bytes_read)
                      : std::string("N/A"));
      out.labeled("Bytes Written",
                  io.info.bytes_written.has_value()
                      ? formatBytes(*io.info.bytes_written)
                      : std::string("N/A"));
      out.labeled("Cancelled Write",
                  io.info.cancelled_write_bytes.has_value()
                      ? formatBytes(*io.info.cancelled_write_bytes)
                      : std::string("N/A"));
      out.labeled("Char Read Rate",
                  io.info.chars_read_rate > 0.0
                      ? (formatBytes(static_cast<std::uint64_t>(
                                     io.info.chars_read_rate)) +
                         "/s")
                      : std::string("N/A"));
      out.labeled("Char Write Rate",
                  io.info.chars_written_rate > 0.0
                      ? (formatBytes(static_cast<std::uint64_t>(
                                     io.info.chars_written_rate)) +
                         "/s")
                      : std::string("N/A"));
      out.labeled("Storage Read Rate",
                  io.info.bytes_read_rate > 0.0
                      ? (formatBytes(static_cast<std::uint64_t>(
                                     io.info.bytes_read_rate)) +
                         "/s")
                      : std::string("N/A"));
      out.labeled("Storage Write Rate",
                  io.info.bytes_written_rate > 0.0
                      ? (formatBytes(static_cast<std::uint64_t>(
                                     io.info.bytes_written_rate)) +
                         "/s")
                      : std::string("N/A"));
    } else {
      out.line("I/O details unavailable: " +
               std::string(ioDetailsStatusMessage(io.status)));
    }
  }

  // Top-level per-process counters & rates reused from the detail snapshot.
  if (info.read_bytes.has_value() || info.write_bytes.has_value()) {
    out.labeled("Read Bytes",
                info.read_bytes.has_value() ? formatBytes(*info.read_bytes)
                                            : std::string("N/A"));
    out.labeled("Write Bytes",
                info.write_bytes.has_value() ? formatBytes(*info.write_bytes)
                                             : std::string("N/A"));
    out.labeled("Read System Calls",
                info.read_syscalls.has_value()
                    ? std::to_string(*info.read_syscalls)
                    : std::string("N/A"));
    out.labeled("Write System Calls",
                info.write_syscalls.has_value()
                    ? std::to_string(*info.write_syscalls)
                    : std::string("N/A"));
  } else if (!info.io_details.has_value()) {
    out.line("I/O counters unavailable (not readable for this process).");
  }
  out.labeled("Read Rate",
              info.read_rate > 0.0
                  ? (formatBytes(static_cast<std::uint64_t>(info.read_rate)) +
                     "/s")
                  : std::string("N/A"));
  out.labeled("Write Rate",
              info.write_rate > 0.0
                  ? (formatBytes(static_cast<std::uint64_t>(info.write_rate)) +
                     "/s")
                  : std::string("N/A"));
  out.line("I/O counters are read-only kernel accounting (rchar/wchar are "
           "system-call level and include page-cache hits; read_bytes/"
           "write_bytes reflect actual storage activity). Rates are deltas "
           "between consecutive samples of the same process identity.");
  out.line("");
}

void renderSchedulingSection(ReportWriter &out,
                             const ProcessDetailsInfo &info) {
  out.line("[Scheduling]");
  out.line("");
  out.labeled("Nice",
              info.nice_priority.has_value()
                  ? std::to_string(*info.nice_priority)
                  : (info.nice_value.has_value()
                         ? std::to_string(*info.nice_value)
                         : std::string("N/A")));
  out.labeled("CPU Affinity", info.allowed_cpus.has_value()
                                  ? formatCpuList(*info.allowed_cpus)
                                  : std::string("N/A"));
  out.labeled("Allowed CPU Count",
              info.allowed_cpus.has_value()
                  ? std::to_string(info.allowed_cpus->size())
                  : std::string("N/A"));
  out.line("Scheduling values are read-only observations of the running "
           "process; the report never modifies them.");
  out.line("");
}

void renderLimitsSection(ReportWriter &out, const ProcessDetailsInfo &info) {
  out.line("[Resource Limits]");
  out.line("");
  if (info.limits.empty()) {
    out.line("Resource limits unavailable.");
  } else {
    appendLimitRow(out, "Open Files", info.limits.open_files, true);
    appendLimitRow(out, "Max Processes", info.limits.max_processes, true);
    appendLimitRow(out, "Stack Size", info.limits.max_stack_size, false);
    appendLimitRow(out, "Locked Memory", info.limits.locked_memory, false);
    appendLimitRow(out, "Address Space", info.limits.address_space, false);
    appendLimitRow(out, "Core File Size", info.limits.core_file_size, false);
    appendLimitRow(out, "Pending Signals", info.limits.pending_signals, true);
    appendLimitRow(out, "POSIX Message Queues",
                   info.limits.posix_message_queues, true);
    appendLimitRow(out, "Realtime Priority", info.limits.realtime_priority,
                   true);
    appendLimitRow(out, "Realtime Timeout", info.limits.realtime_timeout,
                   false);
    out.line("Resource limits are read-only /proc/<pid>/limits values; this "
             "application never modifies them.");
  }
  out.line("");
}

void renderSecuritySection(ReportWriter &out,
                           const ProcessDetailsInfo &info) {
  out.line("[Security & Credentials]");
  out.line("");
  if (!info.security.has_value()) {
    out.line("Security information unavailable.");
    out.line("");
    return;
  }
  const ProcessSecurityResult &security = *info.security;
  if (!security.success()) {
    out.line("Security information unavailable: " +
             std::string(securityStatusMessage(security.status)));
    out.line("");
    return;
  }
  const ProcessSecurityInfo &sec = security.info;

  out.line("UID (real/effective/saved/filesystem): " + formatId(sec.uid.real) +
           " / " + formatId(sec.uid.effective) + " / " +
           formatId(sec.uid.saved) + " / " + formatId(sec.uid.filesystem));
  out.line("GID (real/effective/saved/filesystem): " + formatId(sec.gid.real) +
           " / " + formatId(sec.gid.effective) + " / " +
           formatId(sec.gid.saved) + " / " + formatId(sec.gid.filesystem));

  if (sec.groups_available) {
    std::ostringstream groups;
    bool first = true;
    for (const SupplementaryGroup &group : sec.supplementary_groups) {
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
    out.line("Supplementary Groups (" +
             std::to_string(sec.supplementary_groups.size()) + "): " +
             (first ? std::string("(none)") : groups.str()));
  } else {
    out.line("Supplementary Groups: unavailable");
  }

  for (const CapabilitySet &set : sec.capabilities) {
    out.line(formatCapabilitySet(set));
  }
  if (sec.capabilities.empty()) {
    out.line("Capabilities: unavailable");
  }

  out.labeled("NoNewPrivs", noNewPrivsName(sec.no_new_privs));
  out.labeled("Seccomp", seccompModeName(sec.seccomp));
  out.labeled("Seccomp Filters",
              sec.seccomp_filters.has_value()
                  ? std::to_string(*sec.seccomp_filters)
                  : std::string("N/A"));
  out.labeled("Tracer PID",
              sec.tracer_pid.has_value()
                  ? (*sec.tracer_pid == 0
                         ? std::string("None")
                         : std::to_string(*sec.tracer_pid))
                  : std::string("N/A"));
  out.labeled("Umask", sec.umask.has_value()
                           ? (std::string("0") + std::to_string(*sec.umask))
                           : std::string("N/A"));
  out.labeled("Core Dumping",
              sec.core_dumping.has_value()
                  ? (*sec.core_dumping ? std::string("yes")
                                       : std::string("no"))
                  : std::string("N/A"));
  out.labeled("Security Context (Current)",
              sec.security_context_available ? sec.security_context
                                             : std::string("unavailable"));
  out.labeled("Security Context (Exec)",
              sec.exec_context_available ? sec.exec_context
                                         : std::string("unavailable"));
  out.labeled("Login UID",
              sec.login_uid_available
                  ? (sec.login_uid.has_value() &&
                             *sec.login_uid == kLoginUidUnset
                         ? std::string("unset")
                         : std::to_string(sec.login_uid.value_or(0)))
                  : std::string("unavailable"));
  out.line("Security data is read-only observation; the report never modifies "
           "credentials or capabilities.");
  out.line("");
}

void renderEnvironmentSection(ReportWriter &out,
                              const ProcessDetailsInfo &info,
                              std::size_t max_rows) {
  out.line("[Environment]");
  out.line("");
  if (!info.environment.has_value()) {
    out.line("Environment unavailable.");
    out.line("");
    return;
  }
  const ProcessEnvironmentResult &env = *info.environment;
  if (!env.success()) {
    out.line("Environment unavailable: " + environmentStatusText(env.status));
    out.line("");
    return;
  }

  // Defense in depth: the inspector already masks sensitive values, but the
  // generator re-classifies every entry by name and always renders the mask
  // for a sensitive name, so a plaintext secret can never reach the report
  // text even if an upstream model were ever mis-populated.
  std::size_t masked = 0;
  std::size_t shown = 0;
  for (const ProcessEnvironmentEntry &entry : env.entries) {
    if (shown >= max_rows) {
      break;
    }
    const bool sensitive =
        entry.sensitive || isSensitiveVariableName(entry.name);
    out.line(entry.name + "=" +
             (sensitive ? std::string(kMaskedSecretPlaceholder) : entry.value));
    if (sensitive) {
      ++masked;
    }
    ++shown;
  }
  appendOmittedNote(out, env.entries.size(), shown, "variables");

  out.line("");
  out.line("Potentially sensitive environment variables are masked "
           "(\"" +
           std::string(kMaskedSecretPlaceholder) +
           "\"); plaintext values are never exported. " +
           std::to_string(masked) + " of " +
           std::to_string(env.entries.size()) +
           " variables were classified as sensitive. Entries are sorted by "
           "variable name.");
  if (env.size_truncated || env.variable_truncated) {
    out.line("[truncated] the environment exceeded the collection bounds.");
  }
  out.line("");
}

void renderFileDescriptorsSection(ReportWriter &out,
                                  const ProcessDetailsInfo &info,
                                  std::size_t max_rows) {
  out.line("[File Descriptors]");
  out.line("");
  out.line("The application's installed inspectors expose the selected "
           "process's socket file descriptors, the open-files limit, and the "
           "memory-mapped descriptor inodes. A dedicated open-files/fdinfo "
           "inspector is not installed, so per-descriptor position, flags and "
           "mount IDs are not available.");

  if (info.limits.open_files.has_value() && !info.limits.open_files->empty()) {
    appendLimitRow(out, "Max Open Files", info.limits.open_files, true);
  }

  if (!info.network_connections.has_value()) {
    out.line("Socket descriptors unavailable.");
  } else {
    const ProcessNetworkConnectionsResult &conns = *info.network_connections;
    if (!conns.success()) {
      out.line("Socket descriptors unavailable: " +
               networkStatusText(conns.status));
    } else {
      out.line("Socket descriptors (" +
               std::to_string(conns.connections.size()) + "):");
      std::vector<const ProcessNetworkConnection *> ordered;
      for (const ProcessNetworkConnection &c : conns.connections) {
        ordered.push_back(&c);
      }
      std::sort(
          ordered.begin(), ordered.end(),
          [](const ProcessNetworkConnection *a,
             const ProcessNetworkConnection *b) {
            if (a->fd != b->fd) {
              return a->fd < b->fd;
            }
            return a->inode < b->inode;
          });
      std::size_t shown = 0;
      for (const ProcessNetworkConnection *c : ordered) {
        if (shown >= max_rows) {
          break;
        }
        std::ostringstream row;
        row << "  fd " << c->fd << "  " << connectionProtocolName(c->protocol);
        if (c->protocol == ConnectionProtocol::Unix) {
          row << "  path "
              << (c->local_address.empty() ? std::string("-")
                                           : c->local_address);
        } else {
          row << "  "
              << (c->local_address.empty() ? std::string("[::]")
                                           : c->local_address)
              << ':' << c->local_port;
          if (c->tcp_state.has_value()) {
            row << "  " << tcpStateName(*c->tcp_state);
          } else if (c->udp) {
            row << "  UDP";
          }
        }
        row << "  inode " << c->inode;
        out.line(row.str());
        ++shown;
      }
      appendOmittedNote(out, ordered.size(), shown, "socket descriptors");
    }
  }

  out.line("");
  out.line("Descriptor metadata shown here is read-only; the export never "
           "opens, closes, dupes or intercepts any descriptor of the selected "
           "process.");
  out.line("");
}

void renderLocksSection(ReportWriter &out) {
  out.line("[Locks]");
  out.line("");
  out.line("Kernel file-lock information is not collected by any installed "
           "inspector, so no /proc/locks data is exported. Nothing in this "
           "report modifies or acquires locks on the selected process.");
  out.line("");
}

void renderMemoryMapsSection(ReportWriter &out,
                             const ProcessDetailsInfo &info,
                             std::size_t max_rows) {
  out.line("[Memory Maps]");
  out.line("");
  if (!info.memory_maps.has_value()) {
    out.line("Memory maps unavailable.");
    out.line("");
    return;
  }
  const ProcessMemoryMapsResult &maps = *info.memory_maps;
  if (!maps.success()) {
    out.line("Memory maps unavailable: " + memoryMapStatusText(maps.status));
    out.line("");
    return;
  }

  out.line("Virtual mapping statistics (the virtual address space layout, not "
           "physical RAM):");
  out.labeled("Mappings", std::to_string(maps.maps.size()));
  out.labeled("Total mapped", formatBytes(maps.total_bytes));
  out.labeled("Executable mappings", std::to_string(maps.executable_count));
  out.labeled("Writable mappings", std::to_string(maps.writable_count));
  out.labeled("File-backed mappings", std::to_string(maps.file_backed_count));
  out.labeled("Anonymous mappings", std::to_string(maps.anonymous_count));
  out.line("");

  if (maps.maps.empty()) {
    out.line("No memory mappings.");
  } else {
    out.line("Mapping rows: start-end permissions offset device inode "
             "pathname (size).");
    std::vector<const ProcessMemoryMap *> ordered;
    for (const ProcessMemoryMap &m : maps.maps) {
      ordered.push_back(&m);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const ProcessMemoryMap *a, const ProcessMemoryMap *b) {
                if (a->start != b->start) {
                  return a->start < b->start;
                }
                return a->end < b->end;
              });
    std::size_t shown = 0;
    for (const ProcessMemoryMap *m : ordered) {
      if (shown >= max_rows) {
        break;
      }
      std::ostringstream row;
      row << "  " << hexAddr(m->start) << '-' << hexAddr(m->end) << "  "
          << m->permissions << "  " << hexAddr(m->offset) << "  "
          << formatDevice(m->device_major, m->device_minor) << "  "
          << (m->inode != 0 ? std::to_string(m->inode) : std::string("-"))
          << "  "
          << (m->pathname.empty() ? std::string("(anonymous)")
                                  : m->pathname)
          << "  [size " << formatBytes(m->size()) << "]";
      out.line(row.str());
      ++shown;
    }
    appendOmittedNote(out, ordered.size(), shown, "mappings");
  }
  out.line("");
}

void renderNetworkSection(ReportWriter &out, const ProcessDetailsInfo &info,
                          std::size_t max_rows) {
  out.line("[Network Connections]");
  out.line("");
  if (!info.network_connections.has_value()) {
    out.line("Network connections unavailable.");
    out.line("");
    return;
  }
  const ProcessNetworkConnectionsResult &conns = *info.network_connections;
  if (!conns.success()) {
    out.line("Network connections unavailable: " +
             networkStatusText(conns.status));
    out.line("");
    return;
  }

  out.labeled("TCP", std::to_string(conns.tcp_count));
  out.labeled("UDP", std::to_string(conns.udp_count));
  out.labeled("Unix", std::to_string(conns.unix_count));
  out.labeled("Listening", std::to_string(conns.listening_count));
  out.labeled("Established", std::to_string(conns.established_count));
  out.line("");

  if (conns.connections.empty()) {
    out.line("No network connections.");
  } else {
    out.labeled("Total", std::to_string(conns.connections.size()));
    std::vector<const ProcessNetworkConnection *> ordered;
    for (const ProcessNetworkConnection &c : conns.connections) {
      ordered.push_back(&c);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const ProcessNetworkConnection *a,
                 const ProcessNetworkConnection *b) {
                if (a->fd != b->fd) {
                  return a->fd < b->fd;
                }
                return a->inode < b->inode;
              });
    std::size_t shown = 0;
    for (const ProcessNetworkConnection *c : ordered) {
      if (shown >= max_rows) {
        break;
      }
      std::ostringstream row;
      row << "  fd " << c->fd << "  " << connectionProtocolName(c->protocol);
      if (c->protocol == ConnectionProtocol::Unix) {
        row << "  path "
            << (c->local_address.empty() ? std::string("-")
                                         : c->local_address)
            << "  inode " << c->inode;
      } else {
        row << "  "
            << (c->local_address.empty() ? std::string("[::]")
                                         : c->local_address)
            << ':' << c->local_port << " -> "
            << (c->remote_address.empty() ? std::string("*")
                                          : c->remote_address)
            << ':' << c->remote_port;
        if (c->tcp_state.has_value()) {
          row << "  " << tcpStateName(*c->tcp_state);
        } else if (c->udp) {
          row << "  UDP";
        }
        row << "  inode " << c->inode;
      }
      out.line(row.str());
      ++shown;
    }
    appendOmittedNote(out, ordered.size(), shown, "connections");
  }
  out.line("");
}

void renderNamespacesSection(ReportWriter &out,
                             const ProcessDetailsInfo &info,
                             std::size_t max_rows) {
  out.line("[Namespaces]");
  out.line("");
  if (!info.namespaces.has_value()) {
    out.line("Namespaces unavailable.");
    out.line("");
    return;
  }
  const ProcessNamespaceResult &ns = *info.namespaces;
  if (!ns.success()) {
    out.line("Namespaces unavailable: " + namespaceStatusText(ns.status));
    out.line("");
    return;
  }

  out.labeled("Detected", std::to_string(ns.detected_count));
  out.labeled("Distinct IDs", std::to_string(ns.unique_id_count));
  out.labeled("Unreadable", std::to_string(ns.unavailable_count));
  out.line("");

  if (ns.namespaces.empty()) {
    out.line("No namespaces.");
  } else {
    std::vector<const ProcessNamespace *> ordered;
    for (const ProcessNamespace &n : ns.namespaces) {
      ordered.push_back(&n);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const ProcessNamespace *a, const ProcessNamespace *b) {
                return a->name < b->name;
              });
    std::size_t shown = 0;
    for (const ProcessNamespace *n : ordered) {
      if (shown >= max_rows) {
        break;
      }
      std::ostringstream row;
      row << "  " << namespaceTypeName(n->type) << " (" << n->name << ")";
      if (n->unavailable) {
        row << "  unavailable";
      } else {
        row << "  id "
            << (n->id.has_value() ? std::to_string(*n->id)
                                  : std::string("N/A"))
            << "  target "
            << (n->target.empty() ? std::string("(unreadable)") : n->target);
      }
      out.line(row.str());
      ++shown;
    }
    appendOmittedNote(out, ordered.size(), shown, "namespaces");
  }
  out.line("");
}

void renderCgroupsSection(ReportWriter &out, const ProcessDetailsInfo &info,
                          std::size_t max_rows) {
  out.line("[Cgroups]");
  out.line("");
  if (!info.cgroups.has_value()) {
    out.line("Cgroups unavailable.");
    out.line("");
    return;
  }
  const ProcessCgroupResult &cg = *info.cgroups;
  if (!cg.success()) {
    out.line("Cgroups unavailable: " + cgroupStatusText(cg.status));
    out.line("");
    return;
  }

  out.labeled("Cgroup Version", cgroupVersionName(cg.version));
  if (cg.hierarchies.empty()) {
    out.line("No cgroup hierarchies.");
  } else {
    std::size_t shown = 0;
    for (const ProcessCgroupHierarchy &h : cg.hierarchies) {
      if (shown >= max_rows) {
        break;
      }
      out.line("Hierarchy " + std::to_string(h.hierarchy_id) + ": " +
               (h.relative_path.empty() ? std::string("/")
                                        : h.relative_path));
      if (!h.mount_point.empty()) {
        out.line("  mount point: " + h.mount_point);
      }
      if (!h.controllers.empty()) {
        std::ostringstream ctrl;
        bool first = true;
        for (const std::string &c : h.controllers) {
          if (!first) {
            ctrl << ", ";
          }
          first = false;
          ctrl << c;
        }
        out.line("  controllers: " + ctrl.str());
      }
      ++shown;
    }
    appendOmittedNote(out, cg.hierarchies.size(), shown, "hierarchies");
  }

  if (cg.version == CgroupVersion::V2) {
    const ProcessCgroupResources &r = cg.resources;
    out.line("");
    out.line("cgroup v2 resource values describe the whole cgroup (which may "
             "include other processes and threads), never only the selected "
             "process:");
    out.labeled("cpu.weight",
                r.cpu_weight.available
                    ? (r.cpu_weight.unlimited
                           ? std::string("max")
                           : std::to_string(r.cpu_weight.value))
                    : std::string("N/A"));
    out.labeled("cpu.max",
                r.cpu_max.available
                    ? (r.cpu_max.unlimited
                           ? std::string("max")
                           : (std::to_string(r.cpu_max.quota_usec) +
                              " us / " +
                              std::to_string(r.cpu_max.period_usec) +
                              " us"))
                    : std::string("N/A"));
    out.labeled("memory.current",
                r.memory_current.available
                    ? (r.memory_current.unlimited
                           ? std::string("max")
                           : formatBytes(r.memory_current.value))
                    : std::string("N/A"));
    out.labeled("memory.max",
                r.memory_max.available
                    ? (r.memory_max.unlimited
                           ? std::string("max")
                           : formatBytes(r.memory_max.value))
                    : std::string("N/A"));
    out.labeled("memory.high",
                r.memory_high.available
                    ? (r.memory_high.unlimited
                           ? std::string("max")
                           : formatBytes(r.memory_high.value))
                    : std::string("N/A"));
    out.labeled("pids.current",
                r.pids_current.available
                    ? std::to_string(r.pids_current.value)
                    : std::string("N/A"));
    out.labeled("pids.max",
                r.pids_max.available
                    ? (r.pids_max.unlimited ? std::string("max")
                                            : std::to_string(r.pids_max.value))
                    : std::string("N/A"));
    out.labeled("readable control files",
                std::to_string(r.readable_file_count));
  }
  out.line("");
}

void renderSummarySection(ReportWriter &out, const ProcessDetailsInfo &info) {
  out.line("[Summary]");
  out.line("");
  std::size_t env_count = 0;
  std::size_t env_sensitive = 0;
  if (info.environment.has_value() && info.environment->success()) {
    env_count = info.environment->entries.size();
    env_sensitive = info.environment->sensitive_count;
  }
  out.labeled("Environment variables",
              std::to_string(env_count) + " (" +
                  std::to_string(env_sensitive) + " masked)");
  if (info.network_connections.has_value()) {
    const ProcessNetworkConnectionsResult &conns = *info.network_connections;
    if (conns.success()) {
      out.labeled("Socket descriptors",
                  std::to_string(conns.connections.size()));
    }
  }
  if (info.memory_maps.has_value()) {
    const ProcessMemoryMapsResult &maps = *info.memory_maps;
    if (maps.success()) {
      out.labeled("Memory mappings", std::to_string(maps.maps.size()));
    }
  }
  if (info.namespaces.has_value()) {
    const ProcessNamespaceResult &ns = *info.namespaces;
    if (ns.success()) {
      out.labeled("Namespaces",
                  std::to_string(ns.detected_count) + " detected, " +
                      std::to_string(ns.unavailable_count) + " unreadable");
    }
  }
  if (info.cgroups.has_value()) {
    const ProcessCgroupResult &cg = *info.cgroups;
    if (cg.success()) {
      out.labeled("Cgroup hierarchies",
                  std::to_string(cg.hierarchies.size()) + " (" +
                      cgroupVersionName(cg.version) + ")");
    }
  }
  out.line("");
  out.line("This report is a read-only snapshot of the process's /proc data "
           "collected by the application's inspectors at export time. It does "
           "not modify the selected process. Some sections can be unavailable "
           "because of Linux permissions or because the process exited during "
           "the export.");
}

}  // namespace

std::string generateProcessReport(const ProcessDetailsInfo &info,
                                  std::size_t max_rows,
                                  std::size_t max_bytes) {
  ReportWriter out(max_bytes);
  out.line("Arch Linux Task Manager");
  out.line("Process Details Report");
  out.line("===========================");
  out.line("");
  out.labeled("Generated", nowTimestamp());
  out.line("");
  out.line("Notes:");
  out.line("  - Read-only snapshot; the selected process is never modified.");
  out.line("  - Potentially sensitive environment values are masked; their "
           "plaintext is never exported.");
  out.line("  - Some information may be unavailable because of Linux "
           "permissions or the process exiting.");
  out.line("  - Large sections are truncated for safety/performance with an "
           "explicit marker.");
  out.line("");

  renderProcessSection(out, info);
  renderCpuMemorySection(out, info);
  renderIoSection(out, info);
  renderSchedulingSection(out, info);
  renderLimitsSection(out, info);
  renderSecuritySection(out, info);
  renderEnvironmentSection(out, info, max_rows);
  renderFileDescriptorsSection(out, info, max_rows);
  renderLocksSection(out);
  renderMemoryMapsSection(out, info, max_rows);
  renderNetworkSection(out, info, max_rows);
  renderNamespacesSection(out, info, max_rows);
  renderCgroupsSection(out, info, max_rows);
  renderSummarySection(out, info);

  return out.finish();
}

ReportTargetState verifyReportTarget(const ProcessIdentity &selected) {
  if (selected.pid <= 0) {
    return ReportTargetState::Unreadable;
  }
  const std::optional<ProcessIdentity> current =
      ProcessIdentity::current(selected.pid);
  if (!current.has_value()) {
    const std::string stat_path =
        "/proc/" + std::to_string(selected.pid) + "/stat";
    errno = 0;
    const int rc = ::access(stat_path.c_str(), F_OK);
    if (rc != 0 && errno == ENOENT) {
      return ReportTargetState::Disappeared;
    }
    return ReportTargetState::Unreadable;
  }
  if (current->starttime_ticks != selected.starttime_ticks) {
    return ReportTargetState::Reused;
  }
  return ReportTargetState::Same;
}

ProcessReportResult exportProcessReport(const std::string &path,
                                        const ProcessDetailsInfo &info,
                                        const ProcessIdentity &selected,
                                        std::size_t max_rows,
                                        std::size_t max_bytes) {
  if (info.pid <= 0 || selected.pid <= 0) {
    return ProcessReportResult{ReportStatus::InvalidPid, 0, 0, false, {}};
  }
  if (!info.starttime_ticks.has_value() || *info.starttime_ticks == 0 ||
      selected.starttime_ticks == 0) {
    return ProcessReportResult{ReportStatus::EmptyReport, 0, 0, false, {}};
  }

  std::string contents = generateProcessReport(info, max_rows, max_bytes);
  if (contents.empty()) {
    return ProcessReportResult{ReportStatus::EmptyReport, 0, 0, false, {}};
  }

  ReportStatus outcome = ReportStatus::Success;
  switch (verifyReportTarget(selected)) {
    case ReportTargetState::Same:
      break;
    case ReportTargetState::Disappeared:
      contents +=
          "\nNote: the process exited while this report was being generated; "
          "some sections may be incomplete or missing.\n";
      outcome = ReportStatus::SuccessProcessGone;
      break;
    case ReportTargetState::Reused:
      return ProcessReportResult{ReportStatus::ProcessReused, 0, 0, false, {}};
    case ReportTargetState::Unreadable:
      return ProcessReportResult{ReportStatus::IdentityUnknown, 0, 0, false,
                                 {}};
  }

  ProcessReportResult write = writeProcessReport(path, contents);
  if (write.status != ReportStatus::Success) {
    return write;
  }
  write.status = outcome;
  write.path = path;
  return write;
}

ProcessReportResult writeProcessReport(const std::string &path,
                                       const std::string &contents) {
  ProcessReportResult result;
  if (path.empty()) {
    result.status = ReportStatus::InvalidPath;
    result.errno_value = EINVAL;
    return result;
  }
  if (contents.empty()) {
    result.status = ReportStatus::EmptyReport;
    result.errno_value = 0;
    return result;
  }

  std::error_code ec;
  if (std::filesystem::is_directory(path, ec) && !ec) {
    result.status = ReportStatus::InvalidPath;
    result.errno_value = EISDIR;
    return result;
  }

  const std::filesystem::path tmp =
      std::filesystem::path(path).string() + ".tmp";

  {
    std::ofstream out(tmp, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!out) {
      result.status = ReportStatus::WriteError;
      result.errno_value = errno != 0 ? errno : EIO;
      return result;
    }
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    out.flush();
    if (!out) {
      result.status = ReportStatus::WriteError;
      result.errno_value = errno != 0 ? errno : EIO;
      std::filesystem::remove(tmp, ec);
      return result;
    }
  }

  ec.clear();
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    result.status = ReportStatus::WriteError;
    result.errno_value = errno != 0 ? errno : EIO;
    std::filesystem::remove(tmp, ec);
    return result;
  }

  result.status = ReportStatus::Success;
  result.errno_value = 0;
  result.bytes = contents.size();
  result.path = path;
  return result;
}

}  // namespace atm