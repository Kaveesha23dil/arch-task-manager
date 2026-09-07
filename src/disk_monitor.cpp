#include "disk_monitor.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <sys/statvfs.h>

namespace atm {

namespace {

namespace fs = std::filesystem;

/// True when `c` is an octal digit (0-7).
bool isOctal(char c) {
  return c >= '0' && c <= '7';
}

/**
 * Decodes the octal escape sequences /proc/mounts uses for special bytes in
 * device names and mount points (\040 = space, \011 = tab, \012 = newline,
 * \134 = backslash, ...). A '\' that is not followed by exactly three octal
 * digits is kept verbatim. Because every literal space in those fields is
 * escaped, the fields can otherwise be tokenized by plain whitespace.
 */
std::string unescapeMountToken(const std::string &token) {
  std::string out;
  out.reserve(token.size());
  const std::size_t n = token.size();
  for (std::size_t i = 0; i < n;) {
    if (token[i] == '\\' && i + 3 < n && isOctal(token[i + 1]) &&
        isOctal(token[i + 2]) && isOctal(token[i + 3])) {
      out.push_back(static_cast<char>(
          (token[i + 1] - '0') * 64 + (token[i + 2] - '0') * 8 +
          (token[i + 3] - '0')));
      i += 4;
    } else {
      out.push_back(token[i]);
      ++i;
    }
  }
  return out;
}

/// One of the virtual block-device name prefixes that are not physical disks.
bool isVirtualDeviceName(const std::string &name) {
  static constexpr std::array kVirtualPrefixes = {
      "loop", "ram", "zram", "dm-", "md", "fd", "drbd", "rbd"};
  for (const char *prefix : kVirtualPrefixes) {
    if (name.rfind(prefix, 0) == 0) {
      return true;
    }
  }
  return false;
}

/// Parses a whole-file integer, e.g. the raw 512-byte sector counts the
/// kernel exposes under /sys. Returns std::nullopt on any error.
std::optional<std::uint64_t> readSysU64(const std::string &path) {
  std::ifstream file(path);
  std::uint64_t value = 0;
  if (!(file >> value)) {
    return std::nullopt;
  }
  return value;
}

}  // namespace

const char *diskFsTypeName(DiskFsType type) {
  switch (type) {
    case DiskFsType::Physical:
      return "Physical";
    case DiskFsType::Temporary:
      return "Temporary";
    case DiskFsType::Network:
      return "Network";
    case DiskFsType::Virtual:
      return "Virtual";
  }
  return "Virtual";
}

std::vector<MountEntry> readMounts() {
  // RAII: the file is closed when `file` goes out of scope.
  std::ifstream file("/proc/mounts");
  if (!file.is_open()) {
    return {};
  }

  std::vector<MountEntry> mounts;
  std::string line;
  while (std::getline(file, line)) {
    std::istringstream parser(line);
    std::string device;
    std::string mount_point;
    std::string filesystem;
    if (!(parser >> device >> mount_point >> filesystem)) {
      continue;  // malformed line — skip
    }
    // Literal spaces/punctuation arrive as octal escapes; decode them back.
    MountEntry entry;
    entry.device = unescapeMountToken(device);
    entry.mount_point = unescapeMountToken(mount_point);
    entry.filesystem = filesystem;  // never escaped
    mounts.push_back(std::move(entry));
  }
  return mounts;
}

DiskFsType classifyFilesystem(const std::string &filesystem) {
  if (filesystem == "tmpfs" || filesystem == "ramfs") {
    return DiskFsType::Temporary;
  }

  static constexpr std::array kNetwork{"nfs",      "nfs4",       "nfsd",
                                       "cifs",     "smbfs",       "ncpfs",
                                       "afpfs",    "afs",         "9p",
                                       "ceph",     "glusterfs",   "sshfs",
                                       "fuse.sshfs", "fuse.ceph"};
  if (std::find(kNetwork.begin(), kNetwork.end(), filesystem) !=
      kNetwork.end()) {
    return DiskFsType::Network;
  }

  static constexpr std::array kPhysical{
      "ext2",      "ext3",      "ext4",      "btrfs",      "xfs",
      "f2fs",      "jfs",       "reiserfs",  "reiser4",    "vfat",
      "exfat",     "ntfs",      "ntfs3",     "hfsplus",    "hfs",
      "minix",     "nilfs2",    "ocfs2",     "bcachefs",   "iso9660",
      "udf",       "zfs"};
  if (std::find(kPhysical.begin(), kPhysical.end(), filesystem) !=
      kPhysical.end()) {
    return DiskFsType::Physical;
  }

  // Everything else — proc, sysfs, devpts, devtmpfs, cgroup/cgroup2, pstore,
  // bpf, autofs, overlay, generic fuse, debugfs, tracefs, configfs, ... — is
  // kernel-synthetic or stacked and must not be shown as physical capacity.
  return DiskFsType::Virtual;
}

std::optional<DiskUsage> usageForMount(const MountEntry &mount) {
  struct statvfs vfs;
  if (::statvfs(mount.mount_point.c_str(), &vfs) != 0) {
    return std::nullopt;  // unreadable or vanished between scan and call
  }

  // f_frsize is the fundamental block size; f_bsize is a "preferred" size
  // that can be larger (e.g. 4096 vs 512). statvfs(2) sizes are in f_frsize
  // units, so prefer it and only fall back to f_bsize when it is zero.
  const std::uint64_t block_size =
      vfs.f_frsize != 0 ? static_cast<std::uint64_t>(vfs.f_frsize)
                        : static_cast<std::uint64_t>(vfs.f_bsize);
  if (block_size == 0) {
    return std::nullopt;  // nothing is reported in this filesystem's units
  }
  const std::uint64_t total =
      static_cast<std::uint64_t>(vfs.f_blocks) * block_size;
  const std::uint64_t available =
      static_cast<std::uint64_t>(vfs.f_bavail) * block_size;
  // "Used = Total − Available" per the project definition. f_bavail (space
  // available to unprivileged users, excluding root-reserved blocks) is what
  // df reports as Avail.
  const std::uint64_t used = total > available ? total - available : 0;

  DiskUsage usage;
  usage.device = mount.device;
  usage.mount_point = mount.mount_point;
  usage.filesystem = mount.filesystem;
  usage.type = classifyFilesystem(mount.filesystem);
  usage.total_bytes = total;
  usage.used_bytes = used;
  usage.available_bytes = available;
  if (total != 0) {
    // Guard: clamp instead of letting rounding drift outside [0, 100].
    usage.usage_percentage =
        std::clamp(100.0 * static_cast<double>(used) /
                       static_cast<double>(total),
                   0.0, 100.0);
  }
  return usage;
}

std::vector<DiskCounters> readDiskCounters() {
  // RAII: the file is closed when `file` goes out of scope.
  std::ifstream file("/proc/diskstats");
  if (!file.is_open()) {
    return {};
  }

  std::vector<DiskCounters> counters;
  std::string line;
  while (std::getline(file, line)) {
    std::istringstream parser(line);
    // Fields per Documentation/admin-guide/block/stat.rst:
    //   1 major, 2 minor, 3 name, 4 reads, 5 reads merged, 6 sectors read,
    //   7 time reading, 8 writes, 9 writes merged, 10 sectors written.
    // Discard/flush fields appended by newer kernels are intentionally read
    // past, never stored. A line shorter than field 10 is malformed.
    std::uint64_t major = 0;
    std::uint64_t minor = 0;
    std::string name;
    std::uint64_t reads = 0;
    std::uint64_t reads_merged = 0;
    std::uint64_t read_sectors = 0;
    std::uint64_t read_ms = 0;
    std::uint64_t writes = 0;
    std::uint64_t writes_merged = 0;
    std::uint64_t written_sectors = 0;
    if (!(parser >> major >> minor >> name >> reads >> reads_merged >>
          read_sectors >> read_ms >> writes >> writes_merged >>
          written_sectors)) {
      continue;
    }
    counters.push_back(
        DiskCounters{std::move(name), read_sectors, written_sectors});
  }
  return counters;
}

std::vector<std::string> listWholeDisks() {
  std::vector<std::string> names;
  try {
    const fs::directory_options options =
        fs::directory_options::skip_permission_denied;
    for (const fs::directory_entry &entry :
         fs::directory_iterator("/sys/block", options)) {
      const std::string name = entry.path().filename().string();
      if (!isVirtualDeviceName(name)) {
        names.push_back(name);
      }
    }
  } catch (const std::exception &) {
    return {};  // /sys/block removed or unreadable — treat as no disks
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::uint64_t readDeviceSizeBytes(const std::string &name) {
  // The kernel reports the device size as a count of 512-byte sectors.
  const std::optional<std::uint64_t> sectors =
      readSysU64("/sys/class/block/" + name + "/size");
  if (!sectors.has_value()) {
    return 0;  // disappeared between the /sys/block scan and this read
  }
  return *sectors * 512;
}

std::uint64_t readSectorSizeBytes(const std::string &name) {
  // Prefer the logical block size advertised by the device ...
  if (const std::optional<std::uint64_t> logical =
          readSysU64("/sys/class/block/" + name +
                     "/queue/logical_block_size");
      logical.has_value() && *logical != 0) {
    return *logical;
  }
  if (const std::optional<std::uint64_t> physical =
          readSysU64("/sys/class/block/" + name + "/queue/hw_sector_size");
      physical.has_value() && *physical != 0) {
    return *physical;  // older kernels: hardware sector size
  }
  return 512;  // the unit defined by the /proc/diskstats ABI
}

DiskSnapshot DiskMonitor::read() {
  DiskSnapshot snapshot;

  // --- Filesystem capacities (statvfs over /proc/mounts). ---------------
  {
    std::vector<DiskUsage> filesystems;
    for (const MountEntry &mount : readMounts()) {
      if (classifyFilesystem(mount.filesystem) != DiskFsType::Physical) {
        ++snapshot.excluded_mounts;  // /proc, /sys, tmpfs, overlay, ... filtered
        continue;
      }
      const std::optional<DiskUsage> usage = usageForMount(mount);
      if (!usage.has_value()) {
        continue;  // statvfs failed — skip silently, never spam
      }
      filesystems.push_back(*usage);
    }
    std::sort(filesystems.begin(), filesystems.end(),
              [](const DiskUsage &a, const DiskUsage &b) {
                return a.mount_point < b.mount_point;
              });
    snapshot.filesystems = std::move(filesystems);
  }

  // --- Disk activity (delta of /proc/diskstats over elapsed time). -------
  const std::vector<DiskCounters> counters = readDiskCounters();
  const auto now = std::chrono::steady_clock::now();

  std::unordered_map<std::string, BlockActivity> activity;
  if (previous_time_.has_value() && !counters.empty()) {
    const double seconds =
        std::chrono::duration<double>(now - *previous_time_).count();
    if (seconds > 0.0) {
      const auto old = previous_;
      for (const DiskCounters &counter : counters) {
        const auto previous = old.find(counter.name);
        if (previous == old.end()) {
          continue;  // new device — establish a baseline, first rate next tick
        }
        if (counter.read_sectors < previous->second.read_sectors ||
            counter.written_sectors < previous->second.written_sectors) {
          // Counter rollover or a device replaced since the last sample:
          // silently reset the baseline instead of reporting a bogus delta.
          continue;
        }
        const std::uint64_t sector_size = readSectorSizeBytes(counter.name);
        const std::uint64_t read_bytes =
            (counter.read_sectors - previous->second.read_sectors) *
            sector_size;
        const std::uint64_t written_bytes =
            (counter.written_sectors - previous->second.written_sectors) *
            sector_size;
        BlockActivity &rates = activity[counter.name];
        rates.read_bytes_per_second =
            static_cast<std::uint64_t>(static_cast<double>(read_bytes) /
                                       seconds);
        rates.write_bytes_per_second =
            static_cast<std::uint64_t>(static_cast<double>(written_bytes) /
                                       seconds);
      }
    }
  }
  previous_time_ = now;
  previous_.clear();
  previous_.reserve(counters.size());
  for (const DiskCounters &counter : counters) {
    previous_.emplace(
        counter.name,
        DeviceCounters{counter.read_sectors, counter.written_sectors});
  }

  // --- Whole disks (device size as reported since boot). -----------------
  for (const std::string &name : listWholeDisks()) {
    BlockDevice device;
    device.name = name;
    device.size_bytes = readDeviceSizeBytes(name);
    const auto found = activity.find(name);
    if (found != activity.end()) {
      device.activity = found->second;
    }
    snapshot.total_read_bytes_per_second += device.activity.read_bytes_per_second;
    snapshot.total_write_bytes_per_second +=
        device.activity.write_bytes_per_second;
    snapshot.devices.push_back(std::move(device));
  }

  return snapshot;
}

}  // namespace atm