#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "process_scheduling.hpp"

namespace atm {

/// Maximum bytes read from any single /proc/<pid>/attr/* file. The kernel
/// security context strings are short (under 256 bytes in practice), but
/// this defensive cap prevents unbounded allocation on corrupted input.
constexpr std::size_t kDefaultMaxSecurityAttrBytes = 4096;

/// Maximum bytes read from /proc/<pid>/loginuid.
constexpr std::size_t kDefaultMaxLoginUidBytes = 64;

/// Maximum bytes read from /proc/<pid>/status for security parsing.
constexpr std::size_t kDefaultMaxStatusBytes = 131072;

/// The sentinel value that Linux uses for an unset audit login UID.
constexpr std::uint32_t kLoginUidUnset = 4294967295U;

/// Outcome of reading a process's security information.
enum class SecurityStatus {
  Success,          // security information was read (possibly with some
                    // individual fields unavailable)
  InvalidPid,       // pid <= 0, rejected before any system access
  ProcessNotFound,  // /proc/<pid> is absent (the process has exited / ESRCH)
  IdentityUnknown,  // the process identity could not be re-read
  ProcessReused,    // the PID was reused by a different process since selection
  PermissionDenied, // EACCES / EPERM while opening /proc/<pid>/status
  ReadError,        // any other failure reading /proc files
};

/// One UID/GID value from /proc/<pid>/status. Linux exposes four values for
/// each: real, effective, saved set, and filesystem.
struct IdValues {
  std::optional<std::uint64_t> real;
  std::optional<std::uint64_t> effective;
  std::optional<std::uint64_t> saved;
  std::optional<std::uint64_t> filesystem;
};

/// Linux capability set names. Each set is a bitmask stored in a 64-bit
/// unsigned integer. The names listed here are the standard Linux
/// capabilities as of kernel 6.x; unknown bits are reported separately.
enum class CapabilitySetType {
  Inheritable,  // CapInh
  Permitted,    // CapPrm
  Effective,    // CapEff
  Bounding,     // CapBnd
  Ambient,      // CapAmb
};

/// A decoded capability set: the raw mask plus a list of decoded names.
struct CapabilitySet {
  CapabilitySetType type = CapabilitySetType::Inheritable;
  std::uint64_t raw_mask = 0;
  bool available = false;  // false when the field was missing from /proc/<pid>/status
  std::vector<std::string> decoded_names;  // e.g. "CAP_NET_ADMIN"
  std::vector<std::uint32_t> unknown_bits;  // bits set but not in the known name table
};

/// Seccomp mode as exposed by the kernel.
enum class SeccompMode {
  Disabled,  // 0
  Strict,    // 1
  Filter,    // 2
  Unknown,   // any other value
};

/// Parsed NoNewPrivs flag.
enum class NoNewPrivsState {
  Disabled,  // 0
  Enabled,   // 1
  Unavailable,  // field not present
};

/// One supplementary group.
struct SupplementaryGroup {
  std::uint64_t gid = 0;
  std::string name;  // resolved via getgrgid_r; empty when unresolvable
};

/// Complete security/credential snapshot for one process.
struct ProcessSecurityInfo {
  // UIDs (real, effective, saved, filesystem)
  IdValues uid;

  // GIDs (real, effective, saved, filesystem)
  IdValues gid;

  // Supplementary groups
  std::vector<SupplementaryGroup> supplementary_groups;
  bool groups_available = false;  // false when the Groups: field was missing

  // Capabilities (five sets)
  std::vector<CapabilitySet> capabilities;

  // Security flags
  NoNewPrivsState no_new_privs = NoNewPrivsState::Unavailable;
  SeccompMode seccomp = SeccompMode::Unknown;
  std::optional<std::uint32_t> seccomp_filters;  // filter count
  std::optional<std::uint32_t> tracer_pid;  // 0 means no tracer

  // Optional fields from /proc/<pid>/status
  std::optional<std::uint64_t> umask;
  std::optional<bool> core_dumping;

  // Security context from /proc/<pid>/attr/current
  std::string security_context;  // the LSM context string; empty when unavailable
  bool security_context_available = false;
  int security_context_errno = 0;  // errno when reading attr/current failed

  // Exec security context from /proc/<pid>/attr/exec
  std::string exec_context;
  bool exec_context_available = false;
  int exec_context_errno = 0;  // errno when reading attr/exec failed

  // Audit login UID from /proc/<pid>/loginuid
  std::optional<std::uint32_t> login_uid;
  bool login_uid_available = false;
  int login_uid_errno = 0;  // errno when reading loginuid failed
};

/// Result of one security inspection.
struct ProcessSecurityResult {
  SecurityStatus status = SecurityStatus::ReadError;
  int errno_value = 0;
  ProcessSecurityInfo info;

  [[nodiscard]] bool success() const {
    return status == SecurityStatus::Success;
  }
};

/// Human-readable name for a CapabilitySetType.
[[nodiscard]] const char *capabilitySetTypeName(CapabilitySetType type);

/// Human-readable name for a SeccompMode.
[[nodiscard]] const char *seccompModeName(SeccompMode mode);

/// Human-readable name for NoNewPrivsState.
[[nodiscard]] const char *noNewPrivsName(NoNewPrivsState state);

/// Human-readable description of a SecurityStatus.
[[nodiscard]] const char *securityStatusMessage(SecurityStatus status);

/// Decodes a capability bitmask into human-readable names. Unknown capability
/// numbers are recorded in `unknown_bits` rather than causing a failure. This
/// function is a pure parser with no I/O, no process access, and no
/// subprocesses.
[[nodiscard]] CapabilitySet decodeCapabilities(CapabilitySetType type,
                                               std::uint64_t mask);

/**
 * Parses the security-relevant fields from /proc/<pid>/status content.
 * This is a pure string parser with no system calls, no process access, and
 * no subprocesses. Missing or malformed fields keep their defaults (nullopt
 * / unavailable) rather than causing a failure.
 */
[[nodiscard]] ProcessSecurityInfo parseStatusSecurity(std::string_view contents);

/**
 * Reads /proc/<pid>/attr/current. Returns std::nullopt on any failure
 * (permission denied, file missing, process gone) with the errno in `err_out`.
 * The returned string is bounded by `max_bytes`.
 */
[[nodiscard]] std::optional<std::string>
readSecurityAttr(const std::string &path, std::size_t max_bytes, int *err_out);

/**
 * Reads /proc/<pid>/loginuid. Returns std::nullopt on any failure with the
 * errno in `err_out`. The value is an unsigned 32-bit integer.
 */
[[nodiscard]] std::optional<std::uint32_t>
readLoginUid(const std::string &path, std::size_t max_bytes, int *err_out);

/**
 * Read-only, security-conscious credential inspector for one process.
 *
 * Reads /proc/<pid>/status, /proc/<pid>/attr/current, /proc/<pid>/attr/exec,
 * and /proc/<pid>/loginuid natively (open + bounded read, no subprocesses, no
 * shell, no root requirement) on the inspect call and only then. The security
 * metadata is never logged, never persisted, never sent externally, and never
 * used to modify the target process. Every inspection is gated by the supplied
 * ProcessIdentity (PID + kernel start-time tick) both before and after the
 * read, so a reused PID's security data is never returned.
 */
class ProcessSecurityManager {
 public:
  ProcessSecurityManager() = default;
  ~ProcessSecurityManager() = default;

  // Stateless wrapper object; copy/move are harmless.
  ProcessSecurityManager(const ProcessSecurityManager &) = default;
  ProcessSecurityManager &operator=(const ProcessSecurityManager &) = default;

  /**
   * Reads `identity.pid`'s security and credential information. The process
   * disappears mid-inspection, permission is denied, or the identity changed —
   * each produces a distinct status. Individual fields that cannot be read are
   * marked unavailable rather than failing the entire inspection. Reading never
   * throws and never requires root.
   */
  [[nodiscard]] ProcessSecurityResult
  inspect(const ProcessIdentity &identity) const;
};

}  // namespace atm
