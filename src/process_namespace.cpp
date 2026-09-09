#include "process_namespace.hpp"

#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "process_scheduling.hpp"

namespace atm {

namespace {

std::optional<std::uint64_t> parseUInt(std::string_view view, int base) {
  if (view.empty()) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  const char *begin = view.data();
  const char *end = view.data() + view.size();
  const auto result = std::from_chars(begin, end, value, base);
  if (result.ec != std::errc{} || result.ptr != end) {
    return std::nullopt;
  }
  return value;
}

/// Deterministic display rank for the known namespace types (logical order:
/// cgroup, ipc, mnt, net, pid, pid_for_children, time, time_for_children, user,
/// uts). Unknown types sort after all known ones, alphabetically by name.
std::size_t sortRank(const ProcessNamespace &ns) {
  switch (ns.type) {
    case NamespaceType::Cgroup:
      return 0;
    case NamespaceType::Ipc:
      return 1;
    case NamespaceType::Mount:
      return 2;
    case NamespaceType::Network:
      return 3;
    case NamespaceType::Pid:
      return 4;
    case NamespaceType::PidForChildren:
      return 5;
    case NamespaceType::Time:
      return 6;
    case NamespaceType::TimeForChildren:
      return 7;
    case NamespaceType::User:
      return 8;
    case NamespaceType::Uts:
      return 9;
    case NamespaceType::Unknown:
      break;
  }
  return 10;
}

/// readlink(2) that retries on EINTR and otherwise returns the length (or -1
/// with errno set).
ssize_t readlinkNoIntr(const char *path, char *buf, std::size_t size) {
  ssize_t len = 0;
  do {
    len = ::readlink(path, buf, size);
  } while (len < 0 && errno == EINTR);
  return len;
}

}  // namespace

const char *namespaceTypeName(NamespaceType type) {
  switch (type) {
    case NamespaceType::Cgroup:
      return "Cgroup";
    case NamespaceType::Ipc:
      return "IPC";
    case NamespaceType::Mount:
      return "Mount";
    case NamespaceType::Network:
      return "Network";
    case NamespaceType::Pid:
      return "PID";
    case NamespaceType::PidForChildren:
      return "PID for Children";
    case NamespaceType::Time:
      return "Time";
    case NamespaceType::TimeForChildren:
      return "Time for Children";
    case NamespaceType::User:
      return "User";
    case NamespaceType::Uts:
      return "UTS";
    case NamespaceType::Unknown:
    default:
      return "Unknown";
  }
}

const char *namespaceTypeShortName(NamespaceType type) {
  switch (type) {
    case NamespaceType::Cgroup:
      return "cgroup";
    case NamespaceType::Ipc:
      return "ipc";
    case NamespaceType::Mount:
      return "mnt";
    case NamespaceType::Network:
      return "net";
    case NamespaceType::Pid:
      return "pid";
    case NamespaceType::PidForChildren:
      return "pid_for_children";
    case NamespaceType::Time:
      return "time";
    case NamespaceType::TimeForChildren:
      return "time_for_children";
    case NamespaceType::User:
      return "user";
    case NamespaceType::Uts:
      return "uts";
    case NamespaceType::Unknown:
    default:
      return "";
  }
}

NamespaceType namespaceTypeFromName(std::string_view name) {
  if (name == "cgroup") {
    return NamespaceType::Cgroup;
  }
  if (name == "ipc") {
    return NamespaceType::Ipc;
  }
  if (name == "mnt") {
    return NamespaceType::Mount;
  }
  if (name == "net") {
    return NamespaceType::Network;
  }
  if (name == "pid") {
    return NamespaceType::Pid;
  }
  if (name == "pid_for_children") {
    return NamespaceType::PidForChildren;
  }
  if (name == "time") {
    return NamespaceType::Time;
  }
  if (name == "time_for_children") {
    return NamespaceType::TimeForChildren;
  }
  if (name == "user") {
    return NamespaceType::User;
  }
  if (name == "uts") {
    return NamespaceType::Uts;
  }
  return NamespaceType::Unknown;
}

std::optional<NamespaceTarget> parseNamespaceTarget(std::string_view target) {
  const std::size_t colon = target.find(':');
  if (colon == std::string_view::npos || colon == 0) {
    return std::nullopt;
  }
  if (colon + 1 >= target.size() || target[colon + 1] != '[') {
    return std::nullopt;
  }
  if (target.back() != ']') {
    return std::nullopt;
  }
  const std::string_view digits =
      target.substr(colon + 2, target.size() - colon - 3);
  if (digits.empty()) {
    return std::nullopt;
  }
  const std::optional<std::uint64_t> value = parseUInt(digits, 10);
  if (!value || *value == 0) {
    return std::nullopt;
  }
  NamespaceTarget parsed;
  parsed.type_name = std::string(target.substr(0, colon));
  parsed.id = *value;
  return parsed;
}

ProcessNamespaceResult ProcessNamespaceManager::inspect(
    const ProcessIdentity &identity, std::size_t max_namespaces) const {
  if (identity.pid <= 0) {
    return {NamespaceStatus::InvalidPid, 0, {}, false};
  }

  // Identity gate (before the read).
  std::optional<ProcessIdentity> current =
      ProcessIdentity::current(identity.pid);
  if (!current) {
    return {NamespaceStatus::IdentityUnknown, 0, {}, false};
  }
  if (current->starttime_ticks != identity.starttime_ticks) {
    return {NamespaceStatus::ProcessReused, 0, {}, false};
  }

  const std::string ns_dir =
      "/proc/" + std::to_string(identity.pid) + "/ns";

  DIR *dir = ::opendir(ns_dir.c_str());
  if (dir == nullptr) {
    // One bounded retry for EINTR (the calling thread was interrupted while
    // opening); a still-failing open maps to a distinct state below.
    if (errno == EINTR) {
      dir = ::opendir(ns_dir.c_str());
    }
    if (dir == nullptr) {
      const int err = errno;
      if (err == EACCES || err == EPERM) {
        return {NamespaceStatus::PermissionDenied, err, {}, false};
      }
      if (err == ENOENT || err == ESRCH) {
        return {NamespaceStatus::ProcessNotFound, err, {}, false};
      }
      return {NamespaceStatus::ReadError, err, {}, false};
    }
  }

  ProcessNamespaceResult result;
  result.status = NamespaceStatus::Success;

  char link_target[64];
  struct ::dirent *entry = nullptr;
  while ((entry = ::readdir(dir)) != nullptr) {
    const char *name = entry->d_name;
    if (name[0] == '.' &&
        (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
      continue;
    }
    ++result.detected_count;
    if (result.namespaces.size() >= max_namespaces) {
      result.truncated = true;
      continue;  // keep counting entries, stop collecting records
    }

    ProcessNamespace ns;
    ns.name = name;
    ns.type = namespaceTypeFromName(name);

    const std::string link_path = ns_dir + "/" + name;
    const ssize_t len = readlinkNoIntr(link_path.c_str(), link_target,
                                       sizeof(link_target) - 1);
    if (len <= 0) {
      // The entry exists but its target could not be read (permission denied,
      // or it vanished with the process). Report it as unavailable rather than
      // failing the whole inspection; the identity gate below catches a process
      // that actually exited.
      ns.unavailable = true;
      ns.errno_value = errno;
      ++result.unavailable_count;
    } else {
      link_target[len] = '\0';
      ns.target =
          std::string(link_target, static_cast<std::size_t>(len));
      if (const std::optional<NamespaceTarget> parsed =
              parseNamespaceTarget(ns.target)) {
        ns.id = parsed->id;
      }
    }
    result.namespaces.push_back(std::move(ns));
  }
  ::closedir(dir);

  // Deterministic order: the fixed logical order of the known namespace types,
  // then any unrecognised types, sorted by name so output never shuffles
  // between refreshes.
  std::sort(result.namespaces.begin(), result.namespaces.end(),
            [](const ProcessNamespace &a, const ProcessNamespace &b) {
              const std::size_t ra = sortRank(a);
              const std::size_t rb = sortRank(b);
              if (ra != rb) {
                return ra < rb;
              }
              return a.name < b.name;
            });

  std::set<std::uint64_t> seen;
  for (const ProcessNamespace &ns : result.namespaces) {
    if (ns.id.has_value()) {
      seen.insert(*ns.id);
    }
  }
  result.unique_id_count = seen.size();

  // Identity gate (after the read): discard a stale result if the PID was
  // reused while we were inspecting.
  current = ProcessIdentity::current(identity.pid);
  if (!current) {
    return {NamespaceStatus::IdentityUnknown, 0, {}, false};
  }
  if (current->starttime_ticks != identity.starttime_ticks) {
    return {NamespaceStatus::ProcessReused, 0, {}, false};
  }
  return result;
}

}  // namespace atm