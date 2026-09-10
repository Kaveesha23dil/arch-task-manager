#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "process_scheduling.hpp"

namespace atm {

/// Maximum bytes read from /proc/<pid>/io for safety.
constexpr std::size_t kDefaultMaxIoBytes = 131072;

/// Outcome of reading a process's I/O details.
enum class IoDetailsStatus {
  Success,          // I/O information was read (possibly with some fields
                    // unavailable)
  InvalidPid,       // pid <= 0, rejected before any system access
  ProcessNotFound,  // /proc/<pid> is absent (the process has exited / ESRCH)
  IdentityUnknown,  // the process identity could not be re-read
  ProcessReused,    // the PID was reused by a different process since selection
  PermissionDenied, // EACCES / EPERM while opening /proc/<pid>/io
  ReadError,        // any other failure reading /proc/<pid>/io
};

/// Complete I/O accounting snapshot for one process, parsed from
/// /proc/<pid>/io. Fields that could not be read are std::nullopt.
struct ProcessIoDetailsInfo {
  // Character I/O: bytes counted at the system-call level (rchar / wchar).
  // These include page-cache hits and are NOT direct storage I/O.
  std::optional<std::uint64_t> chars_read;     // rchar
  std::optional<std::uint64_t> chars_written;  // wchar

  // System call counts.
  std::optional<std::uint64_t> read_syscalls;   // syscr
  std::optional<std::uint64_t> write_syscalls;  // syscw

  // Storage I/O: bytes actually fetched from / sent to the storage layer.
  // These reflect real disk I/O and are always <= the character I/O counts.
  std::optional<std::uint64_t> bytes_read;     // read_bytes
  std::optional<std::uint64_t> bytes_written;  // write_bytes

  // Bytes cancelled before reaching stable storage.
  std::optional<std::uint64_t> cancelled_write_bytes;

  // Derived rates (bytes/sec), computed from counter deltas between
  // consecutive samples of the same process identity. Unavailable on the
  // first sample or when the process identity changed.
  double chars_read_rate = 0.0;
  double chars_written_rate = 0.0;
  double bytes_read_rate = 0.0;
  double bytes_written_rate = 0.0;
};

/// Result of one I/O details inspection.
struct ProcessIoDetailsResult {
  IoDetailsStatus status = IoDetailsStatus::ReadError;
  int errno_value = 0;
  ProcessIoDetailsInfo info;

  [[nodiscard]] bool success() const {
    return status == IoDetailsStatus::Success;
  }
};

/// Human-readable description of an IoDetailsStatus.
[[nodiscard]] const char *ioDetailsStatusMessage(IoDetailsStatus status);

/**
 * Parses the I/O accounting fields from /proc/<pid>/io content. This is a
 * pure string parser with no system calls, no process access, and no
 * subprocesses. Missing or malformed fields keep their defaults (nullopt)
 * rather than causing a failure. Unknown fields are silently skipped.
 */
[[nodiscard]] ProcessIoDetailsInfo parseIoDetails(std::string_view contents);

/**
 * Read-only I/O accounting inspector for one process.
 *
 * Reads /proc/<pid>/io natively (open + bounded read, no subprocesses, no
 * shell, no root requirement) on the inspect call and only then. Every
 * inspection is gated by the supplied ProcessIdentity (PID + kernel
 * start-time tick) both before and after the read, so a reused PID's I/O
 * data is never returned.
 */
class ProcessIoDetailsManager {
 public:
  ProcessIoDetailsManager() = default;
  ~ProcessIoDetailsManager() = default;

  ProcessIoDetailsManager(const ProcessIoDetailsManager &) = default;
  ProcessIoDetailsManager &operator=(const ProcessIoDetailsManager &) = default;

  /**
   * Reads `identity.pid`'s I/O accounting information. Individual fields
   * that cannot be read are marked unavailable rather than failing the
   * entire inspection. Reading never throws and never requires root.
   */
  [[nodiscard]] ProcessIoDetailsResult
  inspect(const ProcessIdentity &identity) const;
};

}  // namespace atm
