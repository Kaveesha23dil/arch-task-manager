#include "filesystem_monitor.hpp"

#include <sys/statvfs.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>

namespace atm {

namespace {

using std::uint64_t;

/// True when `c` is an octal digit (0-7).
bool isOctalDigit(char c) {
  return c >= '0' && c <= '7';
}

/// Splits /proc/self/mountinfo whitespace. Because the kernel octal-escapes
/// every literal space/tab/newline that appears inside path fields, no field
/// ever *contains* a real whitespace character, so plain whitespace splitting
/// is safe and cannot break paths with spaces.
std::vector<std::string> splitMountTokens(const std::string &line) {
  std::vector<std::string> tokens;
  std::size_t start = 0;
  const std::size_t n = line.size();
  while (start < n) {
    while (start < n && (line[start] == ' ' || line[start] == '\t')) {
      ++start;
    }
    std::size_t end = start;
    while (end < n && line[end] != ' ' && line[end] != '\t') {
      ++end;
    }
    if (end > start) {
      tokens.push_back(line.substr(start, end - start));
    }
    start = end;
  }
  return tokens;
}

/// Strictly parses a non-negative decimal long. Returns false for empty,
/// non-numeric, negative or overflowing input.
bool parseUnsignedLong(std::string_view token, long &value) {
  if (token.empty()) {
    return false;
  }
  constexpr std::uint64_t kMax =
      static_cast<std::uint64_t>(std::numeric_limits<long>::max());
  std::uint64_t parsed = 0;
  for (const char c : token) {
    if (c < '0' || c > '9') {
      return false;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (parsed > (kMax - digit) / 10) {
      return false;  // overflow
    }
    parsed = parsed * 10 + digit;
  }
  value = static_cast<long>(parsed);
  return true;
}

/// Strictly parses a non-negative decimal into an unsigned int (major/minor).
bool parseUnsignedU32(std::string_view token, unsigned &value) {
  if (token.empty()) {
    return false;
  }
  std::uint64_t parsed = 0;
  constexpr std::uint64_t kMax =
      static_cast<std::uint64_t>(std::numeric_limits<unsigned>::max());
  for (const char c : token) {
    if (c < '0' || c > '9') {
      return false;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (parsed > (kMax - digit) / 10) {
      return false;  // overflow
    }
    parsed = parsed * 10 + digit;
  }
  value = static_cast<unsigned>(parsed);
  return true;
}

/// Parses the "major:minor" device field.
bool parseMajorMinor(const std::string &token, unsigned &major,
                     unsigned &minor) {
  const std::size_t colon = token.find(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 == token.size()) {
    return false;
  }
  return parseUnsignedU32(std::string_view(token).substr(0, colon), major) &&
         parseUnsignedU32(std::string_view(token).substr(colon + 1), minor);
}

/// Clamps a ratio into [0, 100] so no NaN/infinity/negative/>100 value is ever
/// displayed. Only meaningful when the caller first checked the denominator.
double clampPercentage(double value) {
  if (!(value > 0.0)) {
    return 0.0;  // also catches NaN and negative values
  }
  return value > 100.0 ? 100.0 : value;
}

/// Overflow-safe blocks*unit scaling. Returns std::nullopt on overflow so the
/// field is reported unavailable rather than silently wrapped.
std::optional<uint64_t> scaleBlocks(uint64_t blocks, uint64_t unit) {
  if (blocks == 0) {
    return uint64_t{0};
  }
  if (blocks > std::numeric_limits<uint64_t>::max() / unit) {
    return std::nullopt;
  }
  return blocks * unit;
}

}  // namespace

const char *mountAccessName(MountAccess access) {
  switch (access) {
    case MountAccess::ReadWrite:
      return "rw";
    case MountAccess::ReadOnly:
      return "ro";
    case MountAccess::Unknown:
      return "unknown";
  }
  return "unknown";
}

const char *filesystemClassName(FilesystemClass type) {
  switch (type) {
    case FilesystemClass::Physical:
      return "Physical";
    case FilesystemClass::Network:
      return "Network";
    case FilesystemClass::Temporary:
      return "Temporary";
    case FilesystemClass::Pseudo:
      return "Pseudo";
    case FilesystemClass::Container:
      return "Container";
    case FilesystemClass::Overlay:
      return "Overlay";
    case FilesystemClass::Unknown:
      return "Unknown";
  }
  return "Unknown";
}

const char *filesystemErrorName(FilesystemError error) {
  switch (error) {
    case FilesystemError::None:
      return "None";
    case FilesystemError::StatvfsFailed:
      return "StatvfsFailed";
  }
  return "None";
}

std::string unescapeMountInfo(std::string_view token) {
  std::string out;
  out.reserve(token.size());
  const std::size_t n = token.size();
  for (std::size_t i = 0; i < n;) {
    if (token[i] == '\\' && i + 3 < n && isOctalDigit(token[i + 1]) &&
        isOctalDigit(token[i + 2]) && isOctalDigit(token[i + 3])) {
      out.push_back(static_cast<char>((token[i + 1] - '0') * 64 +
                                      (token[i + 2] - '0') * 8 +
                                      (token[i + 3] - '0')));
      i += 4;
    } else {
      out.push_back(token[i]);
      ++i;
    }
  }
  return out;
}

bool parseMountInfoLine(const std::string &line, MountInfo &out) {
  const std::vector<std::string> tokens = splitMountTokens(line);
  // Six fixed fields, then optional fields up to the '-' separator, then the
  // three trailing fields (filesystem type, mount source, super options).
  if (tokens.size() < 6) {
    return false;  // empty line or too short to contain the fixed fields
  }

  long mount_id = -1;
  long parent_id = -1;
  if (!parseUnsignedLong(tokens[0], mount_id) ||
      !parseUnsignedLong(tokens[1], parent_id)) {
    return false;  // non-numeric, negative or overflowing mount IDs
  }
  unsigned major = 0;
  unsigned minor = 0;
  if (!parseMajorMinor(tokens[2], major, minor)) {
    return false;  // malformed major:minor
  }

  MountInfo info;
  info.mount_id = mount_id;
  info.parent_id = parent_id;
  info.major = major;
  info.minor = minor;
  info.root = unescapeMountInfo(tokens[3]);
  info.mount_point = unescapeMountInfo(tokens[4]);
  info.mount_options = tokens[5];

  // Optional fields run until the '-' separator. Kernel lines normally include
  // the separator even when there are no optional fields; tolerate its absence
  // only when everything after the fixed fields is the trailing trio.
  std::size_t index = 6;
  bool separator_found = false;
  for (; index < tokens.size(); ++index) {
    if (tokens[index] == "-") {
      separator_found = true;
      ++index;
      break;
    }
    // Unknown optional fields (any "tag" or "tag:value") are preserved and
    // never make the line malformed.
    info.optional_fields.push_back(tokens[index]);
  }
  if (!separator_found) {
    return false;  // missing separator field
  }
  if (index + 2 >= tokens.size()) {
    return false;  // fewer than the three trailing fields (type/source/so)
  }

  info.filesystem_type = tokens[index];
  info.mount_source = unescapeMountInfo(tokens[index + 1]);
  info.super_options = tokens[index + 2];
  out = std::move(info);
  return true;
}

std::vector<MountInfo> readMountInfo(bool &readable, std::string &error) {
  std::ifstream file("/proc/self/mountinfo");
  if (!file.is_open()) {
    readable = false;
    error = "could not open /proc/self/mountinfo: " +
            std::string(std::strerror(errno));
    return {};
  }

  readable = true;
  error.clear();
  std::vector<MountInfo> mounts;
  std::string line;
  while (std::getline(file, line)) {
    MountInfo info;
    if (parseMountInfoLine(line, info)) {
      mounts.push_back(std::move(info));
    }  // malformed lines are skipped; never an application failure
  }
  return mounts;
}

FilesystemClass classifyFilesystemType(const std::string &type) {
  if (type == "tmpfs" || type == "ramfs") {
    return FilesystemClass::Temporary;
  }
  if (type == "overlay") {
    return FilesystemClass::Overlay;
  }

  static constexpr std::array kNetwork{
      "nfs",     "nfs4",     "nfsd",      "cifs",       "smbfs",
      "ncpfs",   "afpfs",    "afs",       "9p",         "ceph",
      "glusterfs", "sshfs",  "fuse.sshfs", "fuse.ceph",  "davfs",
      "davfs2",  "gfs2",     "coda",      "lustre"};
  if (std::find(kNetwork.begin(), kNetwork.end(), type) != kNetwork.end()) {
    return FilesystemClass::Network;
  }

  static constexpr std::array kPseudo{
      "proc",        "proc2",      "sysfs",      "devpts",
      "devtmpfs",    "devfs",      "cgroup",     "cgroup2",
      "pstore",      "bpf",        "binfmt_misc", "debugfs",
      "tracefs",     "configfs",   "securityfs", "fusectl",
      "mqueue",      "hugetlbfs",  "autofs",     "autofs4",
      "rpc_pipefs",  "nsfs",       "selinuxfs",  "efivarfs"};
  if (std::find(kPseudo.begin(), kPseudo.end(), type) != kPseudo.end()) {
    return FilesystemClass::Pseudo;
  }

  static constexpr std::array kPhysical{
      "ext2",    "ext3",    "ext4",     "xfs",     "btrfs",    "f2fs",
      "jfs",     "reiserfs", "reiser4", "vfat",    "exfat",    "ntfs",
      "ntfs3",   "hfs",     "hfsplus",  "minix",   "nilfs2",  "ocfs2",
      "bcachefs", "zfs",    "iso9660",  "udf",     "squashfs", "fuseblk"};
  if (std::find(kPhysical.begin(), kPhysical.end(), type) !=
      kPhysical.end()) {
    return FilesystemClass::Physical;
  }

  return FilesystemClass::Unknown;
}

FilesystemClass classifyMount(const MountInfo &mount) {
  const FilesystemClass by_type = classifyFilesystemType(mount.filesystem_type);
  if (by_type != FilesystemClass::Unknown) {
    return by_type;
  }
  // A type we do not recognize that is nonetheless backed by a real block
  // device (/dev/* with a non-zero device number) is treated as physical.
  if (mount.major != 0 && mount.mount_source.rfind("/dev/", 0) == 0) {
    return FilesystemClass::Physical;
  }
  return FilesystemClass::Unknown;
}

MountAccess mountAccessFromOptions(const std::string &mount_options) {
  std::size_t start = 0;
  const std::size_t n = mount_options.size();
  while (start < n) {
    const std::size_t comma = mount_options.find(',', start);
    const std::size_t end =
        comma == std::string::npos ? n : comma;
    if (end - start == 2 && mount_options[start] == 'r' &&
        (mount_options[start + 1] == 'w' || mount_options[start + 1] == 'o')) {
      return mount_options[start + 1] == 'w' ? MountAccess::ReadWrite
                                             : MountAccess::ReadOnly;
    }
    start = end + 1;
  }
  return MountAccess::Unknown;
}

FilesystemCapacity computeFilesystemCapacity(
    uint64_t total_blocks, uint64_t free_blocks, uint64_t available_blocks,
    uint64_t total_inodes, uint64_t free_inodes, uint64_t fragment_size,
    uint64_t block_size) {
  FilesystemCapacity capacity;
  const uint64_t unit = fragment_size != 0 ? fragment_size : block_size;
  if (unit == 0) {
    return capacity;  // no usable block-size unit — capacity unavailable
  }

  capacity.total_bytes = scaleBlocks(total_blocks, unit);
  capacity.free_bytes = scaleBlocks(free_blocks, unit);
  capacity.available_bytes = scaleBlocks(available_blocks, unit);

  if (capacity.total_bytes.has_value()) {
    // Clamp inconsistent counters so free/available never exceed total.
    if (capacity.free_bytes.has_value() &&
        *capacity.free_bytes > *capacity.total_bytes) {
      capacity.free_bytes = *capacity.total_bytes;
    }
    if (capacity.available_bytes.has_value() &&
        *capacity.available_bytes > *capacity.total_bytes) {
      capacity.available_bytes = *capacity.total_bytes;
    }
    if (capacity.free_bytes.has_value()) {
      capacity.used_bytes =
          *capacity.total_bytes - *capacity.free_bytes;  // total >= free now
    }
    if (*capacity.total_bytes != 0) {
      if (capacity.used_bytes.has_value()) {
        capacity.usage_percentage =
            clampPercentage(100.0 * static_cast<double>(*capacity.used_bytes) /
                            static_cast<double>(*capacity.total_bytes));
      }
      if (capacity.available_bytes.has_value()) {
        capacity.available_percentage =
            clampPercentage(100.0 *
                            static_cast<double>(*capacity.available_bytes) /
                            static_cast<double>(*capacity.total_bytes));
      }
    }
  }

  // Inode usage needs no byte-scaling; wrapped or zero totals mean
  // "unavailable" rather than a fake 0% (many pseudo/network filesystems
  // report f_files = 0).
  if (total_inodes != 0) {
    const uint64_t free = std::min(free_inodes, total_inodes);
    capacity.total_inodes = total_inodes;
    capacity.free_inodes = free;
    capacity.used_inodes = total_inodes - free;
    capacity.inode_usage_percentage =
        clampPercentage(100.0 * static_cast<double>(*capacity.used_inodes) /
                        static_cast<double>(total_inodes));
  }

  return capacity;
}

FilesystemSnapshot FilesystemMonitor::read() {
  FilesystemSnapshot snapshot;
  bool readable = false;
  std::string error;
  std::vector<MountInfo> mounts = readMountInfo(readable, error);
  snapshot.mountinfo_readable = readable;
  snapshot.error = std::move(error);

  const auto now = std::chrono::system_clock::now();
  snapshot.filesystems.reserve(mounts.size());
  for (MountInfo &mount : mounts) {
    FilesystemInfo info;
    info.mount = std::move(mount);
    info.classification = classifyMount(info.mount);
    info.access = mountAccessFromOptions(info.mount.mount_options);
    info.refreshed_at = now;

    struct statvfs vfs{};
    if (::statvfs(info.mount.mount_point.c_str(), &vfs) == 0) {
      if ((vfs.f_flag & ST_RDONLY) != 0) {
        info.access = MountAccess::ReadOnly;  // kernel is authoritative
      }
      info.capacity = computeFilesystemCapacity(
          static_cast<uint64_t>(vfs.f_blocks),
          static_cast<uint64_t>(vfs.f_bfree),
          static_cast<uint64_t>(vfs.f_bavail),
          static_cast<uint64_t>(vfs.f_files),
          static_cast<uint64_t>(vfs.f_ffree),
          static_cast<uint64_t>(vfs.f_frsize),
          static_cast<uint64_t>(vfs.f_bsize));
    } else {
      // One mount failing never discards the others.
      info.error = FilesystemError::StatvfsFailed;
      info.error_detail = std::strerror(errno);
    }
    snapshot.filesystems.push_back(std::move(info));
  }

  current_ = snapshot;
  recordHistory(current_);
  return snapshot;
}

void FilesystemMonitor::recordHistory(const FilesystemSnapshot &snapshot) {
  if (history_paused_) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  std::vector<std::string> present_identities;
  present_identities.reserve(snapshot.filesystems.size());

  for (const FilesystemInfo &info : snapshot.filesystems) {
    const std::string identity = info.identity();
    present_identities.push_back(identity);

    // Pseudo-filesystems and failed reads contribute no samples (nothing to
    // plot), so tracking them cannot silently create empty series.
    if (info.classification == FilesystemClass::Pseudo) {
      continue;
    }
    if (info.error != FilesystemError::None) {
      continue;
    }
    if (!info.capacity.usage_percentage.has_value() &&
        !info.capacity.available_bytes.has_value() &&
        !info.capacity.inode_usage_percentage.has_value()) {
      continue;  // nothing measurable this refresh
    }

    auto it = histories_.find(identity);
    if (it == histories_.end()) {
      if (histories_.size() >= kMaxTrackedFilesystemSeries) {
        continue;  // bound the number of series on hosts with many mounts
      }
      FilesystemHistory history;
      history.identity = identity;
      history.mount_point = info.mount.mount_point;
      history.usage_percent =
          ResourceHistory<TimedSample>(max_samples_);
      history.available_bytes =
          ResourceHistory<TimedSample>(max_samples_);
      history.free_bytes = ResourceHistory<TimedSample>(max_samples_);
      history.inode_usage_percent =
          ResourceHistory<TimedSample>(max_samples_);
      it = histories_.emplace(identity, std::move(history)).first;
    }
    it->second.mount_point = info.mount.mount_point;

    // One sample per metric per refresh; a change in mount_id or mount_point
    // yields a new identity, so the previous series is pruned below — never an
    // out-of-place append on an unrelated mount.
    if (info.capacity.usage_percentage.has_value()) {
      it->second.usage_percent.addSample(
          TimedSample{now, *info.capacity.usage_percentage});
    }
    if (info.capacity.available_bytes.has_value()) {
      it->second.available_bytes.addSample(
          TimedSample{now, static_cast<double>(*info.capacity.available_bytes)});
    }
    if (info.capacity.free_bytes.has_value()) {
      it->second.free_bytes.addSample(
          TimedSample{now, static_cast<double>(*info.capacity.free_bytes)});
    }
    if (info.capacity.inode_usage_percentage.has_value()) {
      it->second.inode_usage_percent.addSample(
          TimedSample{now, *info.capacity.inode_usage_percentage});
    }
  }

  pruneHistory(present_identities);
}

void FilesystemMonitor::pruneHistory(
    const std::vector<std::string> &present_identities) {
  std::vector<std::string> vanished;
  for (const auto &[identity, history] : histories_) {
    if (std::find(present_identities.begin(), present_identities.end(),
                  identity) == present_identities.end()) {
      vanished.push_back(identity);
    }
  }
  for (const std::string &identity : vanished) {
    histories_.erase(identity);
  }
}

const FilesystemHistory *FilesystemMonitor::historyFor(
    const std::string &identity) const {
  const auto it = histories_.find(identity);
  return it == histories_.end() ? nullptr : &it->second;
}

void FilesystemMonitor::setHistoryMaxSamples(std::size_t max_samples) {
  if (max_samples == 0) {
    max_samples = 1;  // never allow an unbounded/invalid ring
  }
  if (max_samples == max_samples_) {
    return;
  }
  max_samples_ = max_samples;
  for (auto &[identity, history] : histories_) {
    (void)identity;
    history.usage_percent =
        ResourceHistory<TimedSample>(max_samples_);
    history.available_bytes =
        ResourceHistory<TimedSample>(max_samples_);
    history.free_bytes = ResourceHistory<TimedSample>(max_samples_);
    history.inode_usage_percent =
        ResourceHistory<TimedSample>(max_samples_);
  }
}

}  // namespace atm