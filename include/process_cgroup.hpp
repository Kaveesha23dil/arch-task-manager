#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "process_scheduling.hpp"

namespace atm {

/// Upper bound on the number of cgroup hierarchy records collected for one
/// process. cgroup v1 may expose one record per controller hierarchy, cgroup v2
/// a single unified record; the bound guarantees bounded allocations even if
/// the file unexpectedly contained many entries.
constexpr std::size_t kDefaultMaxCgroupHierarchies = 32;

/// Outcome of reading a process's cgroup membership.
enum class CgroupStatus {
  Success,          // membership was read (possibly without resource info)
  InvalidPid,       // pid <= 0, rejected before any system access
  ProcessNotFound,  // /proc/<pid> is absent (the process has exited / ESRCH)
  IdentityUnknown,  // the process identity could not be re-read before applying
  ProcessReused,    // the PID was reused by a different process since selection
  PermissionDenied, // EACCES / EPERM while opening /proc/<pid>/cgroup
  Unavailable,      // no cgroup filesystem is mounted / readable
  MalformedData,    // /proc/<pid>/cgroup yielded no valid membership line
  ReadError,        // any other failure reading the needed files
};

/// The cgroup version the system exposes, inferred from the mounted cgroup
/// filesystems: v2 uses a single unified hierarchy, v1 uses one hierarchy per
/// controller. Unknown when no cgroup filesystem could be discovered.
enum class CgroupVersion { V1, V2, Unknown };

/// Human-readable labels for CgroupVersion.
[[nodiscard]] const char *cgroupVersionName(CgroupVersion version);

/// A read-only scalar cgroup control value: an unsigned integer or the literal
/// "max" (unlimited). "available" is false when the control file is absent
/// (controller not enabled at this level) or could not be read.
struct CgroupValue {
  bool available = false;
  bool unlimited = false;  // the control file contained "max"
  std::uint64_t value = 0;  // raw integer value (bytes, tasks, quota usec)
};

/// Parsed cpu.max control value: "<quota usec> <period usec>" or "max".
struct CgroupCpuMax {
  bool available = false;
  bool unlimited = true;  // true when the quota is "max"
  std::uint64_t quota_usec = 0;
  std::uint64_t period_usec = 0;
};

/// One cgroup hierarchy this process belongs to, parsed from /proc/<pid>/cgroup.
/// cgroup v2 produces a single unified record (hierarchy id 0, no controllers);
/// cgroup v1 may produce several records, one per controller hierarchy.
struct ProcessCgroupHierarchy {
  std::uint64_t hierarchy_id = 0;       // hierarchy id; 0 = the unified v2 hierarchy
  std::vector<std::string> controllers;  // controller/subsystem names; empty for v2
  std::string relative_path;             // path relative to the hierarchy root
  std::string mount_point;               // cgroup fs mount point; "" when not resolved
  std::string absolute_path;             // mount_point + relative_path; "" when not resolvable
  bool resolvable = false;  // absolute_path was validated to stay inside the mount
};

/// Read-only resource snapshot of the process's cgroup v2 directory. Every
/// value describes the whole cgroup (which may include other processes/threads),
/// never only the selected process. A value is "unavailable" when the controller
/// is not enabled at this level or the file could not be read.
struct ProcessCgroupResources {
  CgroupValue cpu_weight;         // cpu.weight (1..10000)
  CgroupCpuMax cpu_max;           // cpu.max (quota µs + period µs, or unlimited)
  CgroupValue memory_current;     // memory.current (bytes)
  CgroupValue memory_max;         // memory.max (bytes)
  CgroupValue memory_high;        // memory.high (bytes)
  CgroupValue pids_current;       // pids.current (tasks)
  CgroupValue pids_max;           // pids.max (tasks)

  // cgroup.controllers available at the selected cgroup, and cgroup.type.
  std::vector<std::string> controllers;
  std::string type;

  // Number of control files that were read successfully; 0 means the resource
  // files were not accessible while some may have been expected.
  std::size_t readable_file_count = 0;
};

/// Result of one cgroup inspection.
struct ProcessCgroupResult {
  CgroupStatus status = CgroupStatus::ReadError;
  int errno_value = 0;
  std::vector<ProcessCgroupHierarchy> hierarchies;  // in file order
  bool truncated = false;  // true when max_hierarchies was reached
  CgroupVersion version = CgroupVersion::Unknown;
  ProcessCgroupResources resources;  // populated for the unified v2 hierarchy

  [[nodiscard]] bool success() const {
    return status == CgroupStatus::Success;
  }
};

/**
 * Parses a single /proc/<pid>/cgroup line of the form
 * "<hierarchy>:<controller,list>:/<path>". The path is decoded from the octal
 * escapes the kernel uses for special bytes. Empty controller lists (v2
 * unified) are allowed. Malformed lines — missing separators, a non-numeric or
 * negative hierarchy id, an empty/missing path, or a path that does not start
 * with '/' — yield std::nullopt instead of crashing. Mount-point and
 * absolute-path fields are left empty here; only the raw membership data is
 * parsed. Never throws.
 */
[[nodiscard]] std::optional<ProcessCgroupHierarchy>
parseCgroupLine(std::string_view line);

/// Parses a whole cgroup control file value: an unsigned decimal integer, or
/// the literal "max" (margin: unlimited). Empty, non-numeric or otherwise
/// malformed content yields std::nullopt. Never throws.
[[nodiscard]] std::optional<CgroupValue>
parseCgroupValue(std::string_view contents);

/// Parses a cpu.max value: "<quota usec> <period usec>" (both unsigned
/// integers) or "max" / "max <period usec>" meaning an unlimited quota.
/// Malformed content yields std::nullopt. Never throws.
[[nodiscard]] std::optional<CgroupCpuMax>
parseCgroupCpuMax(std::string_view contents);

/**
 * Read-only cgroup inspector for one selected process.
 *
 * It reads /proc/<pid>/cgroup natively and, for the unified cgroup v2
 * hierarchy, the selected process's own cgroup directory (resolved from the
 * mounted cgroup filesystem discovered in /proc/self/mountinfo) and a small
 * set of read-only control files. Nothing is ever written: no limits are
 * changed, no process is moved, no controller is enabled or disabled. No
 * external command (systemd-cgls, cgget, ps, shell, ...) is used.
 *
 * Every inspection is gated by the supplied ProcessIdentity (PID + kernel
 * start-time tick) both immediately before and immediately after the read, and
 * the result is discarded when the running process is not the one the caller
 * selected — so a stale result can never be shown under a newer selection of
 * the same PID.
 */
class ProcessCgroupManager {
 public:
  ProcessCgroupManager() = default;
  ~ProcessCgroupManager() = default;

  // Stateless wrapper object; copy/move are harmless.
  ProcessCgroupManager(const ProcessCgroupManager &) = default;
  ProcessCgroupManager &operator=(const ProcessCgroupManager &) = default;

  /**
   * Reads `identity.pid`'s cgroup membership. The process disappears
   * mid-inspection, permission is denied, the cgroup filesystem is absent, or
   * the identity changed — each produces a distinct status. Resource files are
   * read where the (read-only) cgroup directory resolution allows; unreadable
   * values are reported as unavailable, never as zero or fabricated. Reading
   * never throws and never requires root.
   */
  [[nodiscard]] ProcessCgroupResult
  inspect(const ProcessIdentity &identity,
          std::size_t max_hierarchies = kDefaultMaxCgroupHierarchies) const;
};

}  // namespace atm