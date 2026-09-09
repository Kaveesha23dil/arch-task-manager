#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "process_scheduling.hpp"

namespace atm {

/// Upper bound on the number of memory mappings collected for one process in a
/// single inspection. A pathological process that maps thousands of regions is
/// reported as truncated instead of causing unbounded allocations.
constexpr std::size_t kDefaultMaxMappings = 4096;

/// Outcome of reading /proc/<pid>/maps.
enum class MemoryMapStatus {
  Success,          // mappings were read (possibly zero, possibly truncated)
  InvalidPid,       // pid <= 0, rejected before any system access
  ProcessNotFound,  // /proc/<pid>/maps was absent (process has exited / ESRCH)
  IdentityUnknown,  // the process identity could not be re-read before applying
  ProcessReused,    // the PID was reused by a different process since selection
  PermissionDenied, // EACCES / EPERM while opening or reading the maps file
  ReadError,        // any other failure reading the maps file
};

/// Coarse, purpose-driven grouping of one mapping. The original pathname and
/// permission string always remain available on the mapping; the category is
/// only a convenience label built from the pathname and permission bits.
enum class MemoryMapCategory {
  Other,        // anything that did not match one of the categories below
  Executable,   // executable code region (anonymous JIT code or a file-backed
                // executable that is not a shared library)
  SharedLibrary, // file-backed mapping of a shared library
  Heap,         // [heap]
  Stack,        // [stack]
  Anonymous,    // anonymous, non-executable mapping
  FileBacked,   // ordinary file-backed mapping that is not a library
  Vdso,         // [vdso]
  Vvar,         // [vvar]
  Vsyscall,     // [vsyscall]
};

/**
 * One mapping taken verbatim from a /proc/<pid>/maps line. Addresses, offsets,
 * device ids and inodes are parsed once; the original permission string is kept
 * for display. This is strictly metadata — nothing here reads mapped file
 * contents, memory, or process memory.
 */
struct ProcessMemoryMap {
  std::uint64_t start = 0;          // start address (inclusive)
  std::uint64_t end = 0;            // end address (exclusive), always > start
  std::uint64_t offset = 0;         // file offset of the mapping
  std::uint32_t device_major = 0;   // dev field, major part (hex)
  std::uint32_t device_minor = 0;   // dev field, minor part (hex)
  std::uint64_t inode = 0;          // inode of the mapped file (0 = none)
  std::string permissions;          // original permission string, e.g. "r-xp"
  std::string pathname;             // mapping name; empty when anonymous
  bool readable = false;            // 'r' bit
  bool writable = false;            // 'w' bit
  bool executable = false;          // 'x' bit
  bool shared = false;              // 's' (shared) vs 'p' (private)
  MemoryMapCategory category = MemoryMapCategory::Other;

  /// Mapping size in bytes (end - start); only defined when start < end.
  [[nodiscard]] std::uint64_t size() const { return end - start; }
};

/// Result of one memory-map inspection, including the virtual mapping
/// statistics summarised from the returned mappings.
struct ProcessMemoryMapsResult {
  MemoryMapStatus status = MemoryMapStatus::ReadError;
  int errno_value = 0;  // original errno for PermissionDenied / ReadError
  std::vector<ProcessMemoryMap> maps;  // sorted ascending by start address
  bool truncated = false;              // true when max_mappings was reached

  [[nodiscard]] bool success() const {
    return status == MemoryMapStatus::Success;
  }

  // Virtual address-space mapping statistics (label them clearly: these are
  // NOT physical RAM usage).
  std::uint64_t total_bytes = 0;
  std::size_t executable_count = 0;
  std::size_t writable_count = 0;
  std::size_t file_backed_count = 0;
  std::size_t anonymous_count = 0;
};

/**
 * Parses a single /proc/<pid>/maps line. Malformed lines yield std::nullopt
 * instead of crashing: a bad address range, non-hexadecimal field or missing
 * mandatory field is skipped (the spec's "do not crash on malformed lines").
 * The pathname is optional and everything after the five fixed fields is kept
 * (including spaces). Never throws.
 */
[[nodiscard]] std::optional<ProcessMemoryMap>
parseMemoryMapLine(std::string_view line);

/// Assigns the coarse category described by MemoryMapCategory. The original
/// pathname and permissions remain unchanged on the mapping.
[[nodiscard]] MemoryMapCategory
classifyMemoryMap(const ProcessMemoryMap &mapping);

/**
 * Parses a whole /proc/<pid>/maps file. Valid lines are kept in file order
 * (which is ascending by start address); malformed lines are skipped. Stops
 * collecting once `max_mappings` have been parsed and sets `truncated` so the
 * caller can report the abridgement rather than silently dropping data. Never
 * throws.
 */
[[nodiscard]] std::vector<ProcessMemoryMap>
parseMemoryMaps(std::string_view contents, std::size_t max_mappings,
                bool &truncated);

/**
 * Read-only /proc/<pid>/maps inspector for one process.
 *
 * Only /proc/<pid>/maps is read — never process memory, never the contents of
 * mapped files, and never via ptrace or an external command (pmap, ps, ...).
 *
 * Every inspection is gated by the supplied ProcessIdentity: the identity is
 * re-read from the kernel both immediately before and immediately after the
 * file read, and the result is discarded (ProcessReused / IdentityUnknown)
 * when the running process is not the one the caller selected — so a stale
 * result can never be shown under a newer selection of the same PID.
 */
class ProcessMemoryMapManager {
 public:
  ProcessMemoryMapManager() = default;
  ~ProcessMemoryMapManager() = default;

  // Stateless wrapper object; copy/move are harmless.
  ProcessMemoryMapManager(const ProcessMemoryMapManager &) = default;
  ProcessMemoryMapManager &operator=(const ProcessMemoryMapManager &) =
      default;

  /**
   * Reads `identity.pid`'s memory mappings. The process disappears while the
   * file is being read, permission is denied, or the identity changed — each
   * produces a distinct status. Reading never throws and never requires root.
   */
  [[nodiscard]] ProcessMemoryMapsResult
  inspect(const ProcessIdentity &identity,
          std::size_t max_mappings = kDefaultMaxMappings) const;
};

}  // namespace atm