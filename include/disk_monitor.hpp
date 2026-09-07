#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace atm {

/// Broad category of a mounted filesystem, derived from its type string.
///
/// Only `Physical` mounts (real on-disk storage such as ext4/btrfs/xfs/vfat)
/// are presented as capacity in the UI; everything else is counted as
/// "excluded" so /proc, /sys, /dev, tmpfs, overlay and network mounts are
/// never mistaken for physical disks.
enum class DiskFsType {
  Physical,   // ext4, btrfs, xfs, vfat, ntfs, ... — real on-disk storage
  Temporary,  // tmpfs / ramfs — RAM-backed, no physical backing
  Network,    // nfs, cifs, sshfs, ... — mounted over the network
  Virtual,    // proc, sysfs, devpts, devtmpfs, overlay, fuse, ... — kernel/synthetic
};

/// Human-readable name of a filesystem category.
[[nodiscard]] const char *diskFsTypeName(DiskFsType type);

/// Capacity and usage of one mounted filesystem, obtained via `statvfs(2)`.
struct DiskUsage {
  std::string device;        // e.g. "/dev/sda4"; "none"/"proc" for synthetic mounts
  std::string mount_point;   // e.g. "/", "/boot" (no trailing slash except root)
  std::string filesystem;    // raw type string from /proc/mounts (ext4, vfat, ...)
  DiskFsType type = DiskFsType::Virtual;
  std::uint64_t total_bytes = 0;
  std::uint64_t used_bytes = 0;
  std::uint64_t available_bytes = 0;
  double usage_percentage = 0.0;
};

/// Read/write rates in bytes per second for one block device.
struct BlockActivity {
  std::uint64_t read_bytes_per_second = 0;
  std::uint64_t write_bytes_per_second = 0;
};

/// A whole physical disk enumerated from /sys/block (e.g. sda, nvme0n1).
struct BlockDevice {
  std::string name;            // kernel name without the /dev prefix
  std::uint64_t size_bytes = 0;
  BlockActivity activity;
};

/// One complete disk/storage snapshot, produced by DiskMonitor::read().
struct DiskSnapshot {
  std::vector<DiskUsage> filesystems;  // physical mounts, sorted by mount point
  std::size_t excluded_mounts = 0;     // temp/network/virtual mounts filtered out
  std::vector<BlockDevice> devices;    // whole disks from /sys/block, sorted by name
  std::uint64_t total_read_bytes_per_second = 0;   // sum over `devices`
  std::uint64_t total_write_bytes_per_second = 0;
};

/// One line of /proc/mounts, with octal escapes already decoded.
struct MountEntry {
  std::string device;
  std::string mount_point;
  std::string filesystem;
};

/// Parses /proc/mounts. Mount paths with spaces are escaped there as octal
/// sequences (\040, \011, ...) and are decoded. Returns an empty vector when
/// the file cannot be read; malformed lines are skipped.
[[nodiscard]] std::vector<MountEntry> readMounts();

/// Classifies a filesystem type string into a DiskFsType.
[[nodiscard]] DiskFsType classifyFilesystem(const std::string &filesystem);

/// Runs statvfs(2) on a mount and fills in capacity fields. Returns
/// std::nullopt when the call fails (unreadable or vanished mount) or the
/// filesystem reports zero block size.
///
/// Used = Total − Available matches the "free for applications" number that
/// df(1) shows under Avail (f_bavail, which excludes root-reserved blocks).
[[nodiscard]] std::optional<DiskUsage> usageForMount(const MountEntry &mount);

/// Raw sector counters for every device line of /proc/diskstats.
///
/// The kernel reports three counters per device in 512-byte sector units:
/// reads completed, reads merged, sectors read; then writes completed, writes
/// merged, sectors written. Newer kernels append discard/flush fields after
/// these, which are intentionally ignored.
struct DiskCounters {
  std::string name;
  std::uint64_t read_sectors = 0;
  std::uint64_t written_sectors = 0;
};

/// Parses /proc/diskstats. Malformed lines are skipped; an unreadable file
/// yields an empty vector.
[[nodiscard]] std::vector<DiskCounters> readDiskCounters();

/// Whole-disk names from /sys/block (sda, nvme0n1, mmcblk0, ...), sorted by
/// name. Obvious virtual devices (loop*, ram*, zram*, dm-*, md*, fd*, drbd,
/// rbd) are filtered out so they are not presented as physical disks.
[[nodiscard]] std::vector<std::string> listWholeDisks();

/// Physical size of a block device since boot, in bytes. Reads the 512-byte
/// sector count from /sys/class/block/<name>/size. Returns 0 when unreadable.
[[nodiscard]] std::uint64_t readDeviceSizeBytes(const std::string &name);

/// Sector size in bytes used to scale /proc/diskstats counters into bytes.
/// Prefers the kernel-reported logical block size
/// (/sys/class/block/<name>/queue/logical_block_size, falling back to
/// hw_sector_size) and defaults to 512 bytes — the unit defined by the
/// diskstats ABI — when neither is available.
[[nodiscard]] std::uint64_t readSectorSizeBytes(const std::string &name);

/// Formats a byte count with the largest whole prefix on a 1024 base,
/// e.g. formatBytes(80'000'000'000) -> "74.5 GB", formatBytes(0) -> "0 B".
[[nodiscard]] std::string formatBytes(std::uint64_t bytes);

/**
 * Monitors disk storage and activity by reading kernel-provided interfaces.
 *
 * Filesystem capacities come from statvfs(2) over the mounts discovered in
 * /proc/mounts (virtual/temporary/network mounts excluded). Throughput comes
 * from two successive samples of /proc/diskstats: the sector counter deltas
 * are scaled by the device's logical sector size and divided by the real
 * elapsed time. The first read only records a baseline, so like CpuMonitor
 * it returns zero rates once and real figures from the second sample on.
 */
class DiskMonitor {
 public:
  DiskMonitor() = default;
  ~DiskMonitor() = default;

  // Monitors hold diffing state; copying/moving one would duplicate baselines.
  DiskMonitor(const DiskMonitor &) = delete;
  DiskMonitor &operator=(const DiskMonitor &) = delete;

  [[nodiscard]] DiskSnapshot read();

 private:
  struct DeviceCounters {
    std::uint64_t read_sectors = 0;
    std::uint64_t written_sectors = 0;
  };

  std::optional<std::chrono::steady_clock::time_point> previous_time_;
  std::unordered_map<std::string, DeviceCounters> previous_;
};

}  // namespace atm