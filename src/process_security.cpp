#include "process_security.hpp"

#include <fcntl.h>
#include <grp.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace atm {

namespace {

/// Reads a bounded file content. Returns std::nullopt on failure with errno
/// in `err_out`. Retries once on EINTR.
std::optional<std::string> readBoundedFile(const std::string &path,
                                           std::size_t max_bytes,
                                           int *err_out) {
  int fd = -1;
  for (int attempt = 0; attempt < 2; ++attempt) {
    fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
      break;
    }
    if (errno != EINTR || attempt == 1) {
      if (err_out != nullptr) {
        *err_out = errno;
      }
      return std::nullopt;
    }
  }
  if (fd < 0) {
    return std::nullopt;
  }

  std::string contents;
  contents.reserve(std::min(max_bytes, std::size_t{4096}));
  while (contents.size() < max_bytes) {
    char buffer[4096];
    const std::size_t want =
        std::min(sizeof(buffer), max_bytes - contents.size());
    const ssize_t n = ::read(fd, buffer, want);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      const int err = errno;
      ::close(fd);
      if (err_out != nullptr) {
        *err_out = err;
      }
      return std::nullopt;
    }
    if (n == 0) {
      break;
    }
    contents.append(buffer, static_cast<std::size_t>(n));
  }
  ::close(fd);
  if (err_out != nullptr) {
    *err_out = 0;
  }
  return contents;
}

/// Parses a leading unsigned 64-bit integer from a view; returns std::nullopt
/// when the view is empty or does not begin with a digit.
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

/// Skips leading whitespace and returns the first token from the remaining
/// string. Returns an empty view when no token is found.
std::string_view nextToken(std::string_view &line) {
  std::size_t pos = 0;
  while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
    ++pos;
  }
  const std::size_t token_start = pos;
  while (pos < line.size() && line[pos] != ' ' && line[pos] != '\t') {
    ++pos;
  }
  const std::string_view token = line.substr(token_start, pos - token_start);
  line.remove_prefix(pos);
  return token;
}

/// Parses a 64-bit hexadecimal token. Accepts both the plain form the kernel
/// uses ("0000000000001000") and an optional "0x"/"0X" prefix. Returns
/// std::nullopt for empty, malformed or overflowing input.
std::optional<std::uint64_t> parseHex64(std::string_view token) {
  if (token.size() >= 2 && token[0] == '0' &&
      (token[1] == 'x' || token[1] == 'X')) {
    token.remove_prefix(2);
  }
  if (token.empty()) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  const auto result =
      std::from_chars(token.data(), token.data() + token.size(), value, 16);
  if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
    return std::nullopt;
  }
  return value;
}

/// Parses "Uid: 1000 1000 1000 1000" or "Uid:\t1000 1000 1000 1000" etc.
/// Returns four optional values: real, effective, saved, filesystem.
IdValues parseUidLine(std::string_view line) {
  IdValues values;
  std::string_view token;

  token = nextToken(line);
  if (!token.empty()) {
    values.real = parseU64(token);
  }
  token = nextToken(line);
  if (!token.empty()) {
    values.effective = parseU64(token);
  }
  token = nextToken(line);
  if (!token.empty()) {
    values.saved = parseU64(token);
  }
  token = nextToken(line);
  if (!token.empty()) {
    values.filesystem = parseU64(token);
  }
  return values;
}

/// Parses "Groups: 4 24 27 30 44 46 100 1000 1001"
/// Returns the list of supplementary group GIDs.
std::vector<std::uint64_t> parseGroupsLine(std::string_view line) {
  std::vector<std::uint64_t> groups;
  while (!line.empty()) {
    std::string_view token = nextToken(line);
    if (token.empty()) {
      break;
    }
    std::optional<std::uint64_t> gid = parseU64(token);
    if (gid.has_value()) {
      groups.push_back(*gid);
    }
  }
  return groups;
}

/// Resolves a numeric GID to a group name via getgrgid_r. Returns empty
/// string when the group cannot be resolved (no /etc/group entry).
std::string groupName(gid_t gid) {
  char buffer[256];
  struct group grp;
  struct group *result = nullptr;
  if (::getgrgid_r(gid, &grp, buffer, sizeof(buffer), &result) != 0 ||
      result == nullptr) {
    return {};
  }
  return result->gr_name ? std::string(result->gr_name) : std::string{};
}

}  // namespace

// ---------------------------------------------------------------------------
// Capability name table
// ---------------------------------------------------------------------------

namespace {

/// Standard Linux capability names indexed by bit number. The table covers the
/// capabilities the kernel actually names (bits 0-40 in kernel 6.x); bits
/// beyond that are reserved or unknown and are reported as "Unknown capability
/// bit N" rather than fabricated names. A capability bit set but not in this
/// table is never silently discarded.
///
/// Reference: linux/include/uapi/linux/capability.h (kernel 6.x).
constexpr const char *kCapabilityNames[64] = {
    "CAP_CHOWN",            //  0
    "CAP_DAC_OVERRIDE",     //  1
    "CAP_DAC_READ_SEARCH",  //  2
    "CAP_FOWNER",           //  3
    "CAP_FSETID",           //  4
    "CAP_KILL",             //  5
    "CAP_SETGID",           //  6
    "CAP_SETUID",           //  7
    "CAP_SETPCAP",          //  8
    "CAP_LINUX_IMMUTABLE",  //  9
    "CAP_NET_BIND_SERVICE", // 10
    "CAP_NET_BROADCAST",    // 11
    "CAP_NET_ADMIN",        // 12
    "CAP_NET_RAW",          // 13
    "CAP_IPC_LOCK",         // 14
    "CAP_IPC_OWNER",        // 15
    "CAP_SYS_MODULE",       // 16
    "CAP_SYS_RAWIO",        // 17
    "CAP_SYS_CHROOT",       // 18
    "CAP_SYS_PTRACE",       // 19
    "CAP_SYS_PACCT",        // 20
    "CAP_SYS_ADMIN",        // 21
    "CAP_SYS_BOOT",         // 22
    "CAP_SYS_NICE",         // 23
    "CAP_SYS_RESOURCE",     // 24
    "CAP_SYS_TIME",         // 25
    "CAP_SYS_TTY_CONFIG",   // 26
    "CAP_MKNOD",            // 27
    "CAP_LEASE",            // 28
    "CAP_AUDIT_WRITE",      // 29
    "CAP_AUDIT_CONTROL",    // 30
    "CAP_SETFCAP",          // 31
    "CAP_MAC_OVERRIDE",     // 32
    "CAP_MAC_ADMIN",        // 33
    "CAP_SYSLOG",           // 34
    "CAP_WAKE_ALARM",       // 35
    "CAP_BLOCK_SUSPEND",    // 36
    "CAP_AUDIT_READ",       // 37
    "CAP_PERFMON",          // 38
    "CAP_BPF",              // 39
    "CAP_CHECKPOINT_RESTORE",  // 40
    // Bits 41-63 are not named by the current kernel; any set bit is reported
    // as an unknown capability bit.
};

}  // namespace

const char *capabilitySetTypeName(CapabilitySetType type) {
  switch (type) {
    case CapabilitySetType::Inheritable: return "Inheritable";
    case CapabilitySetType::Permitted:   return "Permitted";
    case CapabilitySetType::Effective:   return "Effective";
    case CapabilitySetType::Bounding:    return "Bounding";
    case CapabilitySetType::Ambient:     return "Ambient";
  }
  return "Unknown";
}

const char *seccompModeName(SeccompMode mode) {
  switch (mode) {
    case SeccompMode::Disabled:  return "Disabled";
    case SeccompMode::Strict:    return "Strict";
    case SeccompMode::Filter:    return "Filter";
    case SeccompMode::Unknown:   return "Unknown";
  }
  return "Unknown";
}

const char *noNewPrivsName(NoNewPrivsState state) {
  switch (state) {
    case NoNewPrivsState::Disabled:    return "Disabled";
    case NoNewPrivsState::Enabled:     return "Enabled";
    case NoNewPrivsState::Unavailable: return "Unavailable";
  }
  return "Unavailable";
}

const char *securityStatusMessage(SecurityStatus status) {
  switch (status) {
    case SecurityStatus::Success:
      return "Security information loaded successfully.";
    case SecurityStatus::InvalidPid:
      return "Invalid PID.";
    case SecurityStatus::ProcessNotFound:
      return "Process no longer exists.";
    case SecurityStatus::IdentityUnknown:
      return "Process identity could not be determined.";
    case SecurityStatus::ProcessReused:
      return "The PID was reused; security data was discarded.";
    case SecurityStatus::PermissionDenied:
      return "Permission denied.";
    case SecurityStatus::ReadError:
      return "Failed to read security information.";
  }
  return "Unknown error.";
}

CapabilitySet decodeCapabilities(CapabilitySetType type, std::uint64_t mask) {
  CapabilitySet set;
  set.type = type;
  set.raw_mask = mask;
  set.available = true;

  for (std::uint32_t bit = 0; bit < 64; ++bit) {
    const std::uint64_t bit_mask = std::uint64_t{1} << bit;
    if (mask & bit_mask) {
      if (bit < 64 && kCapabilityNames[bit] != nullptr) {
        set.decoded_names.emplace_back(kCapabilityNames[bit]);
      } else {
        set.unknown_bits.push_back(bit);
      }
    }
  }

  return set;
}

ProcessSecurityInfo parseStatusSecurity(std::string_view contents) {
  ProcessSecurityInfo info;
  std::istringstream lines{std::string(contents)};
  std::string line;

  while (std::getline(lines, line)) {
    std::istringstream head(line);
    std::string key;
    if (!(head >> key)) {
      continue;
    }

    if (key == "Uid:") {
      // Re-read the full line after the key to get the raw remainder.
      std::string remainder;
      std::getline(head, remainder);  // rest of line after "Uid:"
      // The remainder starts with whitespace; parseUidLine handles that.
      info.uid = parseUidLine(remainder);
    } else if (key == "Gid:") {
      std::string remainder;
      std::getline(head, remainder);
      info.gid = parseUidLine(remainder);
    } else if (key == "Groups:") {
      std::string remainder;
      std::getline(head, remainder);
      std::vector<std::uint64_t> gids = parseGroupsLine(remainder);
      info.groups_available = true;
      info.supplementary_groups.reserve(gids.size());
      for (const std::uint64_t g : gids) {
        SupplementaryGroup sg;
        sg.gid = g;
        sg.name = groupName(static_cast<gid_t>(g));
        info.supplementary_groups.push_back(std::move(sg));
      }
    } else if (key == "CapInh:" || key == "CapPrm:" || key == "CapEff:" ||
               key == "CapBnd:" || key == "CapAmb:") {
      std::string hex_str;
      head >> hex_str;
      const std::optional<std::uint64_t> mask = parseHex64(hex_str);
      if (mask.has_value()) {
        CapabilitySetType type = [&key]() {
          if (key == "CapInh:") return CapabilitySetType::Inheritable;
          if (key == "CapPrm:") return CapabilitySetType::Permitted;
          if (key == "CapEff:") return CapabilitySetType::Effective;
          if (key == "CapBnd:") return CapabilitySetType::Bounding;
          return CapabilitySetType::Ambient;  // CapAmb:
        }();
        info.capabilities.push_back(decodeCapabilities(type, *mask));
      }
    } else if (key == "NoNewPrivs:") {
      long value = 0;
      if (head >> value) {
        if (value == 0) {
          info.no_new_privs = NoNewPrivsState::Disabled;
        } else if (value == 1) {
          info.no_new_privs = NoNewPrivsState::Enabled;
        }
        // Any other value: keep as Unknown (the default from Unavailable is
        // close enough, but let's explicitly handle known values).
      }
    } else if (key == "Seccomp:") {
      long value = 0;
      if (head >> value) {
        switch (value) {
          case 0:  info.seccomp = SeccompMode::Disabled; break;
          case 1:  info.seccomp = SeccompMode::Strict; break;
          case 2:  info.seccomp = SeccompMode::Filter; break;
          default: info.seccomp = SeccompMode::Unknown; break;
        }
      }
    } else if (key == "Seccomp_filters:") {
      long value = 0;
      if (head >> value && value >= 0) {
        info.seccomp_filters = static_cast<std::uint32_t>(value);
      }
    } else if (key == "TracerPid:") {
      long value = 0;
      if (head >> value && value >= 0) {
        info.tracer_pid = static_cast<std::uint32_t>(value);
      }
    } else if (key == "Umask:") {
      std::string remainder;
      std::getline(head, remainder);
      // Trim leading whitespace from remainder
      std::size_t start = 0;
      while (start < remainder.size() &&
             (remainder[start] == ' ' || remainder[start] == '\t')) {
        ++start;
      }
      std::string_view token(remainder.data() + start,
                             remainder.size() - start);
      if (!token.empty()) {
        std::optional<std::uint64_t> val = parseU64(token);
        if (val.has_value()) {
          info.umask = *val;
        }
      }
    } else if (key == "CoreDumping:") {
      long value = 0;
      if (head >> value) {
        info.core_dumping = (value != 0);
      }
    }
  }

  return info;
}

std::optional<std::string> readSecurityAttr(const std::string &path,
                                            std::size_t max_bytes,
                                            int *err_out) {
  return readBoundedFile(path, max_bytes, err_out);
}

std::optional<std::uint32_t> readLoginUid(const std::string &path,
                                          std::size_t max_bytes,
                                          int *err_out) {
  const std::optional<std::string> content =
      readBoundedFile(path, max_bytes, err_out);
  if (!content) {
    return std::nullopt;
  }
  // Trim whitespace
  std::string_view view(*content);
  while (!view.empty() &&
         (view.front() == ' ' || view.front() == '\t' || view.front() == '\n' ||
          view.front() == '\r')) {
    view.remove_prefix(1);
  }
  while (!view.empty() &&
         (view.back() == ' ' || view.back() == '\t' || view.back() == '\n' ||
          view.back() == '\r')) {
    view.remove_suffix(1);
  }
  if (view.empty()) {
    return std::nullopt;
  }
  std::uint32_t value = 0;
  auto [ptr, ec] = std::from_chars(view.data(), view.data() + view.size(),
                                   value);
  if (ec != std::errc{} || ptr == view.data()) {
    return std::nullopt;
  }
  return value;
}

ProcessSecurityResult
ProcessSecurityManager::inspect(const ProcessIdentity &identity) const {
  if (identity.pid <= 0) {
    return {SecurityStatus::InvalidPid, 0, {}};
  }

  // Identity gate (before the read).
  std::optional<ProcessIdentity> current =
      ProcessIdentity::current(identity.pid);
  if (!current) {
    return {SecurityStatus::IdentityUnknown, 0, {}};
  }
  if (current->starttime_ticks != identity.starttime_ticks) {
    return {SecurityStatus::ProcessReused, 0, {}};
  }

  const std::string dir =
      "/proc/" + std::to_string(identity.pid);

  // Read /proc/<pid>/status
  int status_errno = 0;
  const std::optional<std::string> status_content =
      readBoundedFile(dir + "/status", kDefaultMaxStatusBytes, &status_errno);
  if (!status_content) {
    if (status_errno == EACCES || status_errno == EPERM) {
      return {SecurityStatus::PermissionDenied, status_errno, {}};
    }
    if (status_errno == ENOENT || status_errno == ESRCH) {
      return {SecurityStatus::ProcessNotFound, status_errno, {}};
    }
    return {SecurityStatus::ReadError, status_errno, {}};
  }

  ProcessSecurityInfo info = parseStatusSecurity(*status_content);

  // Read /proc/<pid>/attr/current (best-effort, not all processes have it)
  {
    int attr_errno = 0;
    std::optional<std::string> ctx = readSecurityAttr(
        dir + "/attr/current", kDefaultMaxSecurityAttrBytes, &attr_errno);
    if (ctx) {
      // Trim trailing whitespace/newlines
      while (!ctx->empty() &&
             (ctx->back() == '\n' || ctx->back() == '\r' ||
              ctx->back() == ' ' || ctx->back() == '\t')) {
        ctx->pop_back();
      }
      info.security_context = std::move(*ctx);
      info.security_context_available = true;
    } else {
      info.security_context_errno = attr_errno;
    }
  }

  // Read /proc/<pid>/attr/exec (best-effort)
  {
    int attr_errno = 0;
    std::optional<std::string> ctx = readSecurityAttr(
        dir + "/attr/exec", kDefaultMaxSecurityAttrBytes, &attr_errno);
    if (ctx) {
      while (!ctx->empty() &&
             (ctx->back() == '\n' || ctx->back() == '\r' ||
              ctx->back() == ' ' || ctx->back() == '\t')) {
        ctx->pop_back();
      }
      info.exec_context = std::move(*ctx);
      info.exec_context_available = true;
    } else {
      info.exec_context_errno = attr_errno;
    }
  }

  // Read /proc/<pid>/loginuid (best-effort)
  {
    int login_errno = 0;
    std::optional<std::uint32_t> luid =
        readLoginUid(dir + "/loginuid", kDefaultMaxLoginUidBytes, &login_errno);
    if (luid) {
      info.login_uid = *luid;
      info.login_uid_available = true;
    } else {
      info.login_uid_errno = login_errno;
    }
  }

  // Identity gate (after the read): discard a stale result if the PID was
  // reused while we were inspecting.
  current = ProcessIdentity::current(identity.pid);
  if (!current) {
    return {SecurityStatus::IdentityUnknown, 0, {}};
  }
  if (current->starttime_ticks != identity.starttime_ticks) {
    return {SecurityStatus::ProcessReused, 0, {}};
  }

  return {SecurityStatus::Success, 0, std::move(info)};
}

}  // namespace atm
