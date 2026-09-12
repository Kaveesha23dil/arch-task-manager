#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "resource_history.hpp"

namespace atm {

/// Default bound for the per-filesystem history ring buffers (samples).
inline constexpr std::size_t kDefaultFilesystemHistorySamples = 120;

/// How a mounted filesystem can be reached. This monitor never modifies mount
/// state; it only reports what the kernel says (mount options / statvfs
/// flags). Read-only mounts are shown as "ro".
enum class MountAccess {
  ReadWrite,  // mounted read-write (rw)
  ReadOnly,   // mounted read-only (ro)
  Unknown,    // no clear signal
};
[[nodiscard]] const char *mountAccessName(MountAccess access);

/// Informational classification of a mounted filesystem, derived from its
/// type string and, for unfamiliar types, its device numbers. Purely
/// informational: filesystems are never hidden just because the type is not
/// in any of the lists below, and the lists are deliberately not exhaustive.
enum class FilesystemClass {
  Physical,   // real on-disk storage: ext4, xfs, btrfs, vfat, ...
  Network,    // nfs, cifs, 9p, sshfs, ...
  Temporary,  // tmpfs / ramfs — RAM-backed
  Pseudo,     // proc, sysfs, devpts, cgroup, ... — kernel/synthetic
  Container,  // stacked/namespaced layout (reserved; auto-detection is not
              // attempted because there is no reliable in-band signal)
  Overlay,    // overlayfs stacked filesystem
  Unknown,    // unclassified (shown, never hidden)
};
[[nodiscard]] const char *filesystemClassName(FilesystemClass type);

/// Structured error for one mounted filesystem (mirrors the project's other
/// structured monitor errors). `None` means the mount was read successfully.
enum class FilesystemError {
  None,
  StatvfsFailed,  // statvfs(2) failed — mount vanished, unreadable or unmounted
};
[[nodiscard]] const char *filesystemErrorName(FilesystemError error);

/// One fully-parsed line of /proc/self/mountinfo. Path fields that arrive in
/// the kernel's octal-escape form (\040 space, \011 tab, \012 newline,
/// \134 backslash) are decoded; option fields are preserved verbatim.
struct MountInfo {
  long mount_id = -1;      // kernel-assigned mount identity
  long parent_id = -1;     // parent mount identity (0 for the root)
  unsigned major = 0;      // device numbers of the backing device (0:0 if none)
  unsigned minor = 0;
  std::string root;               // path inside the backing filesystem (decoded)
  std::string mount_point;        // where it is mounted (decoded)
  std::string mount_options;      // comma-separated, e.g. "rw,relatime"
  std::vector<std::string> optional_fields;  // e.g. "shared:1", "master:5"
  std::string filesystem_type;    // e.g. "ext4"
  std::string mount_source;       // source device/authority (decoded)
  std::string super_options;      // superblock options, e.g. "rw"
};

/// Decodes the octal escapes the kernel uses in mountinfo path fields into a
/// plain string. A backslash that is not part of a valid \xxx octal sequence
/// is kept verbatim. Never throws.
[[nodiscard]] std::string unescapeMountInfo(std::string_view token);

/// Parses one /proc/self/mountinfo line into `out`. Returns false (leaving
/// `out` untouched) for empty/malformed lines: fewer than six fixed fields, a
/// non-numeric or negative mount/parent ID, malformed major:minor, or a
/// missing '-' separator. Unknown optional fields are kept verbatim and never
/// make the line invalid.
[[nodiscard]] bool parseMountInfoLine(const std::string &line, MountInfo &out);

/// Reads and parses every line of /proc/self/mountinfo. On success `readable`
/// is true and `error` is cleared; on an unreadable file (not present or not
/// permitted) `readable` is false, `error` describes the failure and the
/// result is empty.
[[nodiscard]] std::vector<MountInfo> readMountInfo(bool &readable,
                                                   std::string &error);

/// Maps a raw filesystem type string to a classification. The lists are
/// deliberately non-exhaustive; unknown types return `Unknown` rather than
/// being forced into a bucket.
[[nodiscard]] FilesystemClass classifyFilesystemType(const std::string &type);

/// Refines `classifyFilesystemType` with device information: a type that is
/// not recognized but is backed by a real block device (/dev/* source with a
/// non-zero major number) is treated as physical storage.
[[nodiscard]] FilesystemClass classifyMount(const MountInfo &mount);

/// Derives the read-only state from the comma-separated mount options.
[[nodiscard]] MountAccess mountAccessFromOptions(const std::string &mount_options);

/// Capacity and inode figures for one mounted filesystem. Every field is
/// optional: a field with no value is unavailable and is never faked as zero.
struct FilesystemCapacity {
  std::optional<std::uint64_t> total_bytes;
  std::optional<std::uint64_t> used_bytes;      // total − free (root view)
  std::optional<std::uint64_t> free_bytes;      // f_bfree
  std::optional<std::uint64_t> available_bytes; // f_bavail — user-available
  std::optional<double> usage_percentage;       // used/total*100, clamped 0–100
  std::optional<double> available_percentage;   // available/total*100
  std::optional<std::uint64_t> total_inodes;    // f_files
  std::optional<std::uint64_t> used_inodes;     // f_files − f_ffree
  std::optional<std::uint64_t> free_inodes;     // f_ffree
  std::optional<double> inode_usage_percentage; // used/total*100, clamped
};

/// Overflow-safe capacity computation from raw statvfs(2) counters.
///
///   total = f_blocks   * block_size
///   free  = f_bfree    * block_size
///   avail = f_bavail   * block_size
///   used  = total   − free
///
/// `fragment_size` (f_frsize) is the preferred unit and wins over `block_size`
/// (f_bsize); a non-zero unit falls back to block_size; both zero means the
/// filesystem reports no usable unit and capacity is unavailable. Multiplications
/// that would overflow uint64 are treated as unavailable rather than wrapped.
/// Free/available are clamped to total, percentages only when total > 0 and are
/// clamped into [0, 100]. Inodes need no scaling: used = total − free with free
/// clamped to total; when f_files is zero, inode figures are unavailable.
[[nodiscard]] FilesystemCapacity computeFilesystemCapacity(
    std::uint64_t total_blocks, std::uint64_t free_blocks,
    std::uint64_t available_blocks, std::uint64_t total_inodes,
    std::uint64_t free_inodes, std::uint64_t fragment_size,
    std::uint64_t block_size);

/// One discovered mount plus its live capacity. Every successfully parsed
/// mount is retained, whether or not capacity could be read; capacity and
/// access are absent when the read failed.
struct FilesystemInfo {
  MountInfo mount;
  FilesystemClass classification = FilesystemClass::Unknown;
  MountAccess access = MountAccess::Unknown;
  FilesystemError error = FilesystemError::None;
  std::string error_detail;              // strerror text when statvfs failed
  FilesystemCapacity capacity;
  std::chrono::system_clock::time_point refreshed_at;

  /// Stable identity for history and selection: "mountID:mountPoint". Keyed by
  /// this rather than a vector index so rows stay stable across refreshes and
  /// bind mounts plus rebinding mount points stay distinct records.
  [[nodiscard]] std::string identity() const {
    return std::to_string(mount.mount_id) + ":" + mount.mount_point;
  }
};

/// Complete discovery result (one read of mountinfo plus one statvfs(2) per
/// mount).
struct FilesystemSnapshot {
  std::vector<FilesystemInfo> filesystems;  // all discovered mounts
  bool mountinfo_readable = true;           // false when mountinfo was missing
  std::string error;                        // set when mountinfo was unreadable
};

/// Bounded per-mount time-series for selected capacity metrics. One sample is
/// added per refresh (never more); the ring buffers are keyed by stable
/// identity so history survives the mount set changing shape.
struct FilesystemHistory {
  std::string identity;
  std::string mount_point;
  ResourceHistory<TimedSample> usage_percent{0};
  ResourceHistory<TimedSample> available_bytes{0};
  ResourceHistory<TimedSample> free_bytes{0};
  ResourceHistory<TimedSample> inode_usage_percent{0};
};

/// Discovered-filesystem monitor.
///
/// Reads /proc/self/mountinfo once per refresh (the existing single monitoring
/// loop calls read() exactly once per tick — no second polling loop), then asks
/// statvfs(2) for each mount. All filesystems discovered are kept; per-mount
/// history is maintained under the project's Step 14 ResourceHistory ring
/// buffers, bounded, keyed by stable identity, and one sample per refresh. A
/// failed statvfs on one mount never discards the others.
class FilesystemMonitor {
 public:
  /// Upper bound on the number of distinct history series. On hosts with many
  /// mounts this stops per-mount series from growing without limit; the most
  /// relevant mounts (everything except pseudo-filesystems, in discovery
  /// order) are tracked.
  static constexpr std::size_t kMaxTrackedFilesystemSeries = 8;

  FilesystemMonitor() = default;
  ~FilesystemMonitor() = default;

  // A monitor owns its snapshot and history; copying would duplicate state.
  FilesystemMonitor(const FilesystemMonitor &) = delete;
  FilesystemMonitor &operator=(const FilesystemMonitor &) = delete;

  /// Re-reads mountinfo, refreshes statvfs for every mount and appends one
  /// history sample per tracked mount. Returns the fresh snapshot (which is
  /// also available via current()). Cheap: O(number of mounts).
  [[nodiscard]] FilesystemSnapshot read();

  /// The last computed snapshot. Renderers read this without triggering I/O.
  [[nodiscard]] const FilesystemSnapshot &current() const { return current_; }

  /// Adds one history sample per mount in `snapshot` (one per refresh; no
  /// duplicates). Pseudo-filesystems and mounts whose capacity read failed
  /// contribute no samples; vanished mounts are pruned. Public so tests can
  /// drive it hermetically; read() calls it automatically.
  void recordHistory(const FilesystemSnapshot &snapshot);

  /// History of one mount by stable identity, or nullptr when the mount is not
  /// tracked.
  [[nodiscard]] const FilesystemHistory *historyFor(
      const std::string &identity) const;

  [[nodiscard]] const std::unordered_map<std::string, FilesystemHistory> &
  histories() const {
    return histories_;
  }

  void setHistoryMaxSamples(std::size_t max_samples);
  [[nodiscard]] std::size_t historyMaxSamples() const { return max_samples_; }

  /// Temporarily stops collecting samples; nothing is recorded while paused.
  void setHistoryPaused(bool paused) { history_paused_ = paused; }
  [[nodiscard]] bool historyPaused() const { return history_paused_; }

  void clearHistory() { histories_.clear(); }

 private:
  FilesystemSnapshot current_;
  std::unordered_map<std::string, FilesystemHistory> histories_;
  std::size_t max_samples_ = kDefaultFilesystemHistorySamples;
  bool history_paused_ = false;

  void pruneHistory(const std::vector<std::string> &present_identities);
};

}  // namespace atm