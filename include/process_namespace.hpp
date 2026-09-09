#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "process_scheduling.hpp"

namespace atm {

/// Upper bound on the number of namespace records collected for one process in
/// a single inspection. A running kernel exposes at most the handful of known
/// namespace types, so this generous bound guarantees bounded allocations even
/// if a future kernel exposes many more.
constexpr std::size_t kDefaultMaxNamespaces = 64;

/// Outcome of reading /proc/<pid>/ns.
enum class NamespaceStatus {
  Success,          // namespace entries were enumerated (possibly zero, possibly
                    // with one or more entries reported as unavailable)
  InvalidPid,       // pid <= 0, rejected before any system access
  ProcessNotFound,  // /proc/<pid>/ns is absent (the process has exited / ESRCH)
  IdentityUnknown,  // the process identity could not be re-read before applying
  ProcessReused,    // the PID was reused by a different process since selection
  PermissionDenied, // EACCES / EPERM while opening /proc/<pid>/ns
  ReadError,        // any other failure reading the /proc/<pid>/ns directory
};

/// The Linux namespace types a /proc/<pid>/ns directory can expose. Unknown is
/// used when the running kernel presents an entry this application does not
/// recognise, so an unexpected type is displayed (with its raw name preserved)
/// instead of making the inspection fail.
enum class NamespaceType {
  Cgroup,            // cgroup
  Ipc,               // ipc
  Mount,             // mnt
  Network,           // net
  Pid,               // pid
  PidForChildren,    // pid_for_children
  Time,              // time
  TimeForChildren,   // time_for_children
  User,              // user
  Uts,               // uts
  Unknown,           // a namespace the application does not recognise
};

/// Human-readable display label, e.g. "Mount", "PID for Children".
[[nodiscard]] const char *namespaceTypeName(NamespaceType type);

/// The kernel-provided short name, e.g. "mnt", "pid_for_children"; "" for
/// Unknown (the raw name is preserved on ProcessNamespace::name instead).
[[nodiscard]] const char *namespaceTypeShortName(NamespaceType type);

/// Maps a /proc/<pid>/ns directory-entry name to a NamespaceType. Anything that
/// is not one of the known names yields NamespaceType::Unknown.
[[nodiscard]] NamespaceType namespaceTypeFromName(std::string_view name);

/// Parsed form of a /proc/<pid>/ns symlink target such as "net:[4026531992]".
struct NamespaceTarget {
  std::string type_name;  // the prefix before ':', e.g. "net"
  std::uint64_t id = 0;   // the numeric namespace identifier, e.g. 4026531992
};

/**
 * Parses a namespace symlink target of the form "<type>:[<decimal id>]" into
 * its type prefix and numeric identifier. The identifier is parsed into an
 * unsigned 64-bit value (never overflowing a signed 32-bit integer, never
 * negative). Malformed targets — an empty id, a non-numeric/negative id, a zero
 * id, or missing brackets — yield std::nullopt instead of crashing. The caller
 * keeps the raw target string separately; this function never modifies it.
 * Never throws.
 */
[[nodiscard]] std::optional<NamespaceTarget>
parseNamespaceTarget(std::string_view target);

/// One read-only namespace record for the inspected process.
struct ProcessNamespace {
  NamespaceType type = NamespaceType::Unknown;
  std::string name;   // the directory entry name, e.g. "net"
  std::string target;  // the raw symlink target, e.g. "net:[4026531992]";
                       // empty when the entry could not be read
  std::optional<std::uint64_t> id;  // parsed identifier; nullopt when unreadable
                                    // or the target was malformed
  bool unavailable = false;   // the entry existed but its target could not be read
  int errno_value = 0;        // the errno captured when unavailable
};

/// Result of one namespace inspection, including lightweight summary counters.
struct ProcessNamespaceResult {
  NamespaceStatus status = NamespaceStatus::ReadError;
  int errno_value = 0;   // original errno for PermissionDenied / ReadError
  std::vector<ProcessNamespace> namespaces;  // deterministic order
  bool truncated = false;  // true when max_namespaces was reached

  [[nodiscard]] bool success() const {
    return status == NamespaceStatus::Success;
  }

  // Summary. detected_count is the number of namespace entries enumerated;
  // unique_id_count is the number of distinct numeric identifiers among the
  // collected entries; unavailable_count is how many entries could not be read.
  std::size_t detected_count = 0;
  std::size_t unique_id_count = 0;
  std::size_t unavailable_count = 0;
};

/**
 * Read-only /proc/<pid>/ns inspector for one selected process.
 *
 * It enumerates the namespace symlinks the running kernel actually exposes for
 * the process and reads each symlink's target natively via readlink(2). No
 * external command (lsns, nsenter, readlink(1), ps, ...) is executed, no shell
 * is launched, no namespace is entered, created or modified, and no process
 * other than the selected one is scanned.
 *
 * Every inspection is gated by the supplied ProcessIdentity (PID + kernel
 * start-time tick) both immediately before and immediately after the read, and
 * the result is discarded when the running process is not the one the caller
 * selected — so a stale result can never be shown under a newer selection of
 * the same PID.
 */
class ProcessNamespaceManager {
 public:
  ProcessNamespaceManager() = default;
  ~ProcessNamespaceManager() = default;

  // Stateless wrapper object; copy/move are harmless.
  ProcessNamespaceManager(const ProcessNamespaceManager &) = default;
  ProcessNamespaceManager &operator=(const ProcessNamespaceManager &) = default;

  /**
   * Reads `identity.pid`'s namespaces. The process disappears mid-inspection,
   * permission is denied, or the identity changed — each produces a distinct
   * status. A single unreadable entry is reported as unavailable rather than
   * failing the whole inspection. Reading never throws and never requires root.
   */
  [[nodiscard]] ProcessNamespaceResult
  inspect(const ProcessIdentity &identity,
          std::size_t max_namespaces = kDefaultMaxNamespaces) const;
};

}  // namespace atm