#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "filesystem_monitor.hpp"

// --- Minimal standalone test harness (no external framework) -------------
namespace {
int g_checks = 0;
int g_failures = 0;

void expect(bool condition, const char *expr, const char *file, int line) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
  }
}
void run(const char *name) {
  std::fprintf(stderr, "TEST %s\n", name);
}
}  // namespace

#define CHECK(expr) ::expect((expr), #expr, __FILE__, __LINE__)

// -------------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------------

using atm::FilesystemClass;
using atm::FilesystemInfo;
using atm::FilesystemSnapshot;
using atm::MountAccess;
using atm::MountInfo;

/// Builds a synthetic mount record for history/classification tests.
atm::FilesystemInfo makeInfo(long mount_id, const std::string &point,
                             const std::string &type, double used_pct,
                             std::uint64_t available_bytes,
                             double inode_pct) {
  atm::FilesystemInfo info;
  info.mount.mount_id = mount_id;
  info.mount.mount_point = point;
  info.mount.filesystem_type = type;
  info.classification = atm::classifyFilesystemType(type);
  info.access = atm::mountAccessFromOptions("rw,relatime");
  info.refreshed_at = std::chrono::system_clock::now();
  atm::FilesystemCapacity cap;
  cap.total_bytes = 1000000000;
  cap.used_bytes = static_cast<std::uint64_t>(used_pct * 1000000000 / 100.0);
  cap.free_bytes = *cap.total_bytes - *cap.used_bytes;
  cap.available_bytes = available_bytes;
  cap.usage_percentage = used_pct;
  cap.available_percentage =
      100.0 * static_cast<double>(available_bytes) / 1000000000.0;
  cap.total_inodes = 100000;
  cap.used_inodes = static_cast<std::uint64_t>(inode_pct * 100000 / 100.0);
  cap.free_inodes = *cap.total_inodes - *cap.used_inodes;
  cap.inode_usage_percentage = inode_pct;
  info.capacity = cap;
  return info;
}

// -------------------------------------------------------------------------
// Mountinfo parsing
// -------------------------------------------------------------------------

void test_normal_line() {
  run("normal mountinfo line");
  MountInfo info;
  CHECK(atm::parseMountInfoLine(
      "19 1 0:21 / /run rw,nosuid,nodev,relatime - tmpfs tmpfs rw,mode=755",
      info));
  CHECK(info.mount_id == 19);
  CHECK(info.parent_id == 1);
  CHECK(info.major == 0);
  CHECK(info.minor == 21);
  CHECK(info.root == "/");
  CHECK(info.mount_point == "/run");
  CHECK(info.mount_options == "rw,nosuid,nodev,relatime");
  CHECK(info.optional_fields.empty());
  CHECK(info.filesystem_type == "tmpfs");
  CHECK(info.mount_source == "tmpfs");
  CHECK(info.super_options == "rw,mode=755");
}

void test_optional_fields_and_separator() {
  run("optional fields + separator detection");
  MountInfo info;
  CHECK(atm::parseMountInfoLine(
      "36 35 98:0 /mnt1 /mnt2 rw,noatime master:1 shared:5 - ext3 /dev/root "
      "rw,errors=continue",
      info));
  CHECK(info.mount_id == 36);
  CHECK(info.parent_id == 35);
  CHECK(info.major == 98);
  CHECK(info.minor == 0);
  CHECK(info.optional_fields.size() == 2);
  CHECK(info.optional_fields[0] == "master:1");
  CHECK(info.optional_fields[1] == "shared:5");
  CHECK(info.filesystem_type == "ext3");
  CHECK(info.mount_source == "/dev/root");
  CHECK(info.super_options == "rw,errors=continue");
}

void test_unknown_optional_fields() {
  run("unknown optional fields are preserved");
  MountInfo info;
  CHECK(atm::parseMountInfoLine(
      "50 1 0:40 / /mnt/data rw,relatime what:ever weird tag - xfs /dev/sda5 "
      "rw",
      info));
  CHECK(info.optional_fields.size() == 3);
  CHECK(info.optional_fields[0] == "what:ever");
  CHECK(info.optional_fields[1] == "weird");
  CHECK(info.optional_fields[2] == "tag");
  CHECK(info.filesystem_type == "xfs");
}

void test_escaped_spaces_etc() {
  run("escaped octal sequences in paths");
  MountInfo info;
  // \040 = space, \011 = tab, \134 = backslash in mount point + root + source.
  CHECK(atm::parseMountInfoLine(
      "70 1 8:1 /mnt\\040root /mnt\\040with\\040spaces\\134dir\\011x rw - "
      "ext4 /dev/disk\\040by-uuid/abc rw",
      info));
  CHECK(info.root == "/mnt root");
  CHECK(info.mount_point == "/mnt with spaces\\dir\tx");
  CHECK(info.mount_source == "/dev/disk by-uuid/abc");
  CHECK(info.filesystem_type == "ext4");
}

void test_missing_separator() {
  run("missing separator field");
  MountInfo info;
  CHECK(!atm::parseMountInfoLine("19 1 0:21 / /run rw tmpfs tmpfs rw", info));
}

void test_missing_fields() {
  run("too few fields");
  MountInfo info;
  CHECK(!atm::parseMountInfoLine("19 1 0:21 / /run", info));
}

void test_malformed_ids() {
  run("malformed mount/parent IDs");
  MountInfo info;
  CHECK(!atm::parseMountInfoLine("abc 1 0:21 / /run rw - x y z", info));
  CHECK(!atm::parseMountInfoLine("-1 1 0:21 / /run rw - x y z", info));
  CHECK(!atm::parseMountInfoLine("19 x 0:21 / /run rw - x y z", info));
  CHECK(!atm::parseMountInfoLine(
      "9999999999999999999999 1 0:21 / /run rw - x y z", info));
}

void test_malformed_major_minor() {
  run("malformed major:minor");
  MountInfo info;
  CHECK(!atm::parseMountInfoLine("19 1 / /run rw - x y z", info));
  CHECK(!atm::parseMountInfoLine("19 1 :21 / /run rw - x y z", info));
  CHECK(!atm::parseMountInfoLine("19 1 8: / /run rw - x y z", info));
  CHECK(!atm::parseMountInfoLine("19 1 a:b / /run rw - x y z", info));
  CHECK(!atm::parseMountInfoLine("19 1 99999999999999999999:1 / /run rw - x y "
                                 "z",
                                 info));
}

void test_empty_and_blank_input() {
  run("empty and blank lines");
  MountInfo info;
  CHECK(!atm::parseMountInfoLine("", info));
  CHECK(!atm::parseMountInfoLine("   \t  ", info));
}

void test_unescape() {
  run("unescapeMountInfo round-trips the required escapes");
  CHECK(atm::unescapeMountInfo("a\\040b") == "a b");
  CHECK(atm::unescapeMountInfo("a\\011b") == "a\tb");
  CHECK(atm::unescapeMountInfo("a\\012b") == "a\nb");
  CHECK(atm::unescapeMountInfo("a\\134b") == "a\\b");
  // A '\' not followed by exactly three octal digits is kept verbatim.
  CHECK(atm::unescapeMountInfo("a\\04b") == "a\\04b");
  CHECK(atm::unescapeMountInfo("a\\x40b") == "a\\x40b");
  CHECK(atm::unescapeMountInfo("plain") == "plain");
  CHECK(atm::unescapeMountInfo("") == "");
}

// -------------------------------------------------------------------------
// Capacity calculations
// -------------------------------------------------------------------------

void test_capacity_normal() {
  run("normal capacity");
  const atm::FilesystemCapacity c = atm::computeFilesystemCapacity(
      100, 30, 25, 1000, 700, 4096, 4096);
  CHECK(c.total_bytes.has_value() && *c.total_bytes == 409600);
  CHECK(c.free_bytes.has_value() && *c.free_bytes == 122880);
  CHECK(c.available_bytes.has_value() && *c.available_bytes == 102400);
  CHECK(c.used_bytes.has_value() && *c.used_bytes == 286720);
  CHECK(c.usage_percentage.has_value() && c.usage_percentage.value() > 69.9 &&
        c.usage_percentage.value() < 70.1);
  CHECK(c.available_percentage.has_value() &&
        c.available_percentage.value() > 24.9 &&
        c.available_percentage.value() < 25.1);
}

void test_capacity_frsize_preference() {
  run("f_frsize preferred over f_bsize");
  // f_frsize = 512 is the fundamental unit; values scale by it.
  const atm::FilesystemCapacity c =
      atm::computeFilesystemCapacity(100, 50, 50, 0, 0, 512, 4096);
  CHECK(c.total_bytes.has_value() && *c.total_bytes == 51200);
}

void test_capacity_bsize_fallback() {
  run("f_bsize fallback when f_frsize is zero");
  const atm::FilesystemCapacity c =
      atm::computeFilesystemCapacity(100, 50, 50, 0, 0, 0, 4096);
  CHECK(c.total_bytes.has_value() && *c.total_bytes == 409600);
}

void test_capacity_overflow() {
  run("overflow detection");
  const atm::FilesystemCapacity c = atm::computeFilesystemCapacity(
      std::numeric_limits<std::uint64_t>::max(),
      std::numeric_limits<std::uint64_t>::max(),
      std::numeric_limits<std::uint64_t>::max(), 0, 0, 1024, 1024);
  CHECK(!c.total_bytes.has_value());
  CHECK(!c.free_bytes.has_value());
  CHECK(!c.available_bytes.has_value());
  CHECK(!c.used_bytes.has_value());
  CHECK(!c.usage_percentage.has_value());
  CHECK(!c.available_percentage.has_value());
}

void test_capacity_zero_block_size() {
  run("zero block size means capacity unavailable");
  const atm::FilesystemCapacity c =
      atm::computeFilesystemCapacity(100, 50, 50, 0, 0, 0, 0);
  CHECK(!c.total_bytes.has_value() && !c.free_bytes.has_value() &&
        !c.available_bytes.has_value() && !c.used_bytes.has_value());
  CHECK(!c.usage_percentage.has_value());
}

void test_capacity_free_greater_than_total() {
  run("free greater than total is clamped and used is zero");
  const atm::FilesystemCapacity c =
      atm::computeFilesystemCapacity(100, 200, 50, 0, 0, 1, 1);
  CHECK(c.total_bytes.has_value() && *c.total_bytes == 100);
  CHECK(c.free_bytes.has_value() && *c.free_bytes == 100);  // clamped
  CHECK(c.used_bytes.has_value() && *c.used_bytes == 0);
  CHECK(c.usage_percentage.has_value() && *c.usage_percentage == 0.0);
}

void test_capacity_available_greater_than_free() {
  run("available greater than free is clamped to total");
  const atm::FilesystemCapacity c =
      atm::computeFilesystemCapacity(100, 10, 500, 0, 0, 1, 1);
  CHECK(c.free_bytes.has_value() && *c.free_bytes == 10);
  CHECK(c.available_bytes.has_value() && *c.available_bytes == 100);  // clamp
  CHECK(c.used_bytes.has_value() && *c.used_bytes == 90);
  CHECK(c.usage_percentage.has_value() && *c.usage_percentage == 90.0);
  CHECK(c.available_percentage.has_value() &&
        *c.available_percentage == 100.0);
}

void test_capacity_zero_total() {
  run("zero total blocks");
  const atm::FilesystemCapacity c =
      atm::computeFilesystemCapacity(0, 0, 0, 0, 0, 512, 512);
  CHECK(c.total_bytes.has_value() && *c.total_bytes == 0);
  CHECK(c.used_bytes.has_value() && *c.used_bytes == 0);
  CHECK(!c.usage_percentage.has_value());
  CHECK(!c.available_percentage.has_value());
}

void test_capacity_precision_stable_across_refreshes() {
  run("identical raw counters produce identical percentages (no drift)");
  const auto compute = [] {
    // Sanity value: ~36.7 % — a percentage whose decimal form could drift if
    // the maths were done sloppily across refreshes.
    return atm::computeFilesystemCapacity(104857600, 66355200, 66355200, 0, 0,
                                          1024, 1024);
  };
  const atm::FilesystemCapacity first = compute();
  const atm::FilesystemCapacity second = compute();
  CHECK(first.total_bytes.has_value() &&
        *first.total_bytes == static_cast<std::uint64_t>(104857600) * 1024);
  CHECK(first.usage_percentage.has_value());
  CHECK(second.usage_percentage.has_value());
  // Bit-identical across refreshes: no drift, no rounding creep.
  CHECK(*first.usage_percentage == *second.usage_percentage);
  CHECK(*first.available_percentage == *second.available_percentage);
  // Percentage value is sensible for total/free used above (~36.7 %).
  CHECK(*first.usage_percentage > 30.0 && *first.usage_percentage < 45.0);
}

// -------------------------------------------------------------------------
// Inode calculations
// -------------------------------------------------------------------------

void test_inodes_normal() {
  run("normal inode usage");
  const atm::FilesystemCapacity c =
      atm::computeFilesystemCapacity(10, 0, 0, 1000, 700, 512, 512);
  CHECK(c.total_inodes.has_value() && *c.total_inodes == 1000);
  CHECK(c.free_inodes.has_value() && *c.free_inodes == 700);
  CHECK(c.used_inodes.has_value() && *c.used_inodes == 300);
  CHECK(c.inode_usage_percentage.has_value() &&
        *c.inode_usage_percentage == 30.0);
}

void test_inodes_zero_total() {
  run("zero inode total is unavailable, not 0%");
  const atm::FilesystemCapacity c =
      atm::computeFilesystemCapacity(10, 0, 0, 0, 0, 512, 512);
  CHECK(!c.total_inodes.has_value());
  CHECK(!c.free_inodes.has_value());
  CHECK(!c.used_inodes.has_value());
  CHECK(!c.inode_usage_percentage.has_value());
}

void test_inodes_free_greater_than_total() {
  run("free inodes greater than total are clamped");
  const atm::FilesystemCapacity c =
      atm::computeFilesystemCapacity(10, 0, 0, 100, 300, 512, 512);
  CHECK(c.free_inodes.has_value() && *c.free_inodes == 100);
  CHECK(c.used_inodes.has_value() && *c.used_inodes == 0);
  CHECK(c.inode_usage_percentage.has_value() &&
        *c.inode_usage_percentage == 0.0);
}

void test_inodes_overflow_wraps_are_impossible() {
  run("extreme inode values do not wrap");
  const atm::FilesystemCapacity c = atm::computeFilesystemCapacity(
      10, 0, 0, std::numeric_limits<std::uint64_t>::max(), 1, 512, 512);
  CHECK(c.total_inodes.has_value() &&
        *c.total_inodes == std::numeric_limits<std::uint64_t>::max());
  CHECK(c.used_inodes.has_value() &&
        *c.used_inodes == std::numeric_limits<std::uint64_t>::max() - 1);
  CHECK(c.inode_usage_percentage.has_value() &&
        *c.inode_usage_percentage == 100.0);
}

// -------------------------------------------------------------------------
// Mount state and classification
// -------------------------------------------------------------------------

void test_mount_access() {
  run("read-only / read-write / unknown state");
  CHECK(atm::mountAccessFromOptions("rw,relatime,seclabel") ==
        MountAccess::ReadWrite);
  CHECK(atm::mountAccessFromOptions("ro,nosuid,noexec") ==
        MountAccess::ReadOnly);
  CHECK(atm::mountAccessFromOptions("noexec,nosuid,nodev") ==
        MountAccess::Unknown);
  CHECK(atm::mountAccessFromOptions("rw") == MountAccess::ReadWrite);
  CHECK(atm::mountAccessFromOptions("ro") == MountAccess::ReadOnly);
  CHECK(atm::mountAccessFromOptions("") == MountAccess::Unknown);
  // A trailing "rw"/"ro" option is still detected.
  CHECK(atm::mountAccessFromOptions("nosuid,ro") == MountAccess::ReadOnly);
}

void test_classification_types() {
  run("classification by filesystem type");
  CHECK(atm::classifyFilesystemType("ext4") == FilesystemClass::Physical);
  CHECK(atm::classifyFilesystemType("xfs") == FilesystemClass::Physical);
  CHECK(atm::classifyFilesystemType("btrfs") == FilesystemClass::Physical);
  CHECK(atm::classifyFilesystemType("vfat") == FilesystemClass::Physical);
  CHECK(atm::classifyFilesystemType("tmpfs") == FilesystemClass::Temporary);
  CHECK(atm::classifyFilesystemType("ramfs") == FilesystemClass::Temporary);
  CHECK(atm::classifyFilesystemType("nfs") == FilesystemClass::Network);
  CHECK(atm::classifyFilesystemType("cifs") == FilesystemClass::Network);
  CHECK(atm::classifyFilesystemType("9p") == FilesystemClass::Network);
  CHECK(atm::classifyFilesystemType("sshfs") == FilesystemClass::Network);
  CHECK(atm::classifyFilesystemType("proc") == FilesystemClass::Pseudo);
  CHECK(atm::classifyFilesystemType("sysfs") == FilesystemClass::Pseudo);
  CHECK(atm::classifyFilesystemType("devpts") == FilesystemClass::Pseudo);
  CHECK(atm::classifyFilesystemType("cgroup2") == FilesystemClass::Pseudo);
  CHECK(atm::classifyFilesystemType("overlay") == FilesystemClass::Overlay);
  CHECK(atm::classifyFilesystemType("zzzfs") == FilesystemClass::Unknown);
  CHECK(atm::classifyFilesystemType("") == FilesystemClass::Unknown);
}

void test_classification_mount_refinement() {
  run("unknown type backed by a block device is physical");
  MountInfo dev;
  dev.filesystem_type = "zzzfs";
  dev.major = 8;
  dev.minor = 1;
  dev.mount_source = "/dev/sda1";
  CHECK(atm::classifyMount(dev) == FilesystemClass::Physical);

  MountInfo none;
  none.filesystem_type = "zzzfs";
  none.major = 0;
  none.minor = 0;
  none.mount_source = "none";
  CHECK(atm::classifyMount(none) == FilesystemClass::Unknown);
}

// -------------------------------------------------------------------------
// History
// -------------------------------------------------------------------------

void test_history_stable_identity() {
  run("history keeps one series per stable identity");
  atm::FilesystemMonitor monitor;
  monitor.setHistoryMaxSamples(16);

  atm::FilesystemSnapshot s1;
  s1.filesystems.push_back(makeInfo(31, "/", "ext4", 20.0, 800'000'000, 10.0));
  monitor.recordHistory(s1);

  atm::FilesystemSnapshot s2;
  s2.filesystems.push_back(makeInfo(31, "/", "ext4", 21.0, 790'000'000, 10.5));
  monitor.recordHistory(s2);

  const atm::FilesystemHistory *history = monitor.historyFor("31:/");
  CHECK(history != nullptr);
  CHECK(history->usage_percent.size() == 2);
  CHECK(history->mount_point == "/");
  CHECK(history->available_bytes.size() == 2);
  CHECK(history->free_bytes.size() == 2);
  CHECK(history->inode_usage_percent.size() == 2);
}

void test_history_appearing_disappearing() {
  run("mount appearing and disappearing");
  atm::FilesystemMonitor monitor;
  monitor.setHistoryMaxSamples(16);

  atm::FilesystemSnapshot s1;
  s1.filesystems.push_back(makeInfo(31, "/", "ext4", 20.0, 800'000'000, 10.0));
  monitor.recordHistory(s1);
  CHECK(monitor.historyFor("31:/") != nullptr);

  atm::FilesystemSnapshot s2;
  s2.filesystems.push_back(makeInfo(31, "/", "ext4", 20.0, 800'000'000, 10.0));
  s2.filesystems.push_back(
      makeInfo(42, "/home", "ext4", 30.0, 400'000'000, 5.0));
  monitor.recordHistory(s2);
  CHECK(monitor.historyFor("42:/home") != nullptr);
  CHECK(monitor.histories().size() == 2);

  atm::FilesystemSnapshot s3;
  s3.filesystems.push_back(makeInfo(31, "/", "ext4", 20.0, 800'000'000, 10.0));
  monitor.recordHistory(s3);
  CHECK(monitor.historyFor("42:/home") == nullptr);  // pruned
  CHECK(monitor.historyFor("31:/") != nullptr);
  CHECK(monitor.histories().size() == 1);
}

void test_history_mount_point_change() {
  run("mount point change rebinds history by identity");
  atm::FilesystemMonitor monitor;
  monitor.setHistoryMaxSamples(16);

  atm::FilesystemSnapshot s1;
  s1.filesystems.push_back(
      makeInfo(50, "/oldspot", "ext4", 20.0, 800'000'000, 10.0));
  monitor.recordHistory(s1);
  CHECK(monitor.historyFor("50:/oldspot") != nullptr);

  atm::FilesystemSnapshot s2;
  s2.filesystems.push_back(
      makeInfo(50, "/newspot", "ext4", 25.0, 700'000'000, 11.0));
  monitor.recordHistory(s2);
  CHECK(monitor.historyFor("50:/oldspot") == nullptr);
  CHECK(monitor.historyFor("50:/newspot") != nullptr);
  CHECK(monitor.historyFor("50:/newspot")->usage_percent.size() == 1);
}

void test_history_bounded() {
  run("history size is bounded");
  atm::FilesystemMonitor monitor;
  monitor.setHistoryMaxSamples(3);

  atm::FilesystemSnapshot s;
  s.filesystems.push_back(makeInfo(31, "/", "ext4", 20.0, 800'000'000, 10.0));
  for (int i = 0; i < 5; ++i) {
    monitor.recordHistory(s);
  }
  const atm::FilesystemHistory *history = monitor.historyFor("31:/");
  CHECK(history != nullptr);
  CHECK(history->usage_percent.size() == 3);
}

void test_history_disabled_and_resumed() {
  run("paused history records nothing");
  atm::FilesystemMonitor monitor;
  monitor.setHistoryMaxSamples(16);

  atm::FilesystemSnapshot s;
  s.filesystems.push_back(makeInfo(31, "/", "ext4", 20.0, 800'000'000, 10.0));

  monitor.setHistoryPaused(true);
  monitor.recordHistory(s);
  monitor.recordHistory(s);
  CHECK(monitor.histories().empty());

  monitor.setHistoryPaused(false);
  monitor.recordHistory(s);
  CHECK(monitor.historyFor("31:/") != nullptr);
  CHECK(monitor.historyFor("31:/")->usage_percent.size() == 1);
}

void test_history_one_sample_per_refresh() {
  run("one sample per refresh, no duplicates");
  atm::FilesystemMonitor monitor;
  monitor.setHistoryMaxSamples(16);

  atm::FilesystemSnapshot s;
  s.filesystems.push_back(makeInfo(31, "/", "ext4", 20.0, 800'000'000, 10.0));
  monitor.recordHistory(s);
  CHECK(monitor.historyFor("31:/")->usage_percent.size() == 1);

  // A second call is a second refresh: one more sample, never two.
  monitor.recordHistory(s);
  CHECK(monitor.historyFor("31:/")->usage_percent.size() == 2);
}

void test_history_pseudo_and_failed_are_not_tracked() {
  run("pseudo filesystems and failed reads are not tracked");
  atm::FilesystemMonitor monitor;
  monitor.setHistoryMaxSamples(16);

  atm::FilesystemSnapshot s;
  s.filesystems.push_back(makeInfo(31, "/", "ext4", 20.0, 800'000'000, 10.0));
  s.filesystems.push_back(makeInfo(40, "/proc", "proc", 0.0, 0, 0.0));
  atm::FilesystemInfo broken = makeInfo(60, "/broken", "ext4", 10.0, 1, 1.0);
  broken.classification = atm::classifyFilesystemType("ext4");
  broken.error = atm::FilesystemError::StatvfsFailed;
  broken.error_detail = "No such file or directory";
  broken.capacity = atm::FilesystemCapacity{};
  s.filesystems.push_back(std::move(broken));

  monitor.recordHistory(s);
  CHECK(monitor.histories().size() == 1);
  CHECK(monitor.historyFor("40:/proc") == nullptr);
  CHECK(monitor.historyFor("60:/broken") == nullptr);
  CHECK(monitor.historyFor("31:/") != nullptr);
}

void test_history_series_limit() {
  run("excessive mount sets are bounded");
  atm::FilesystemMonitor monitor;
  monitor.setHistoryMaxSamples(8);

  atm::FilesystemSnapshot s;
  for (int i = 0; i < 20; ++i) {
    s.filesystems.push_back(
        makeInfo(100 + i, "/mnt" + std::to_string(i), "ext4", 10.0, 1, 1.0));
  }
  monitor.recordHistory(s);
  CHECK(monitor.histories().size() <=
        atm::FilesystemMonitor::kMaxTrackedFilesystemSeries);
  CHECK(monitor.histories().size() ==
        atm::FilesystemMonitor::kMaxTrackedFilesystemSeries);
}

int main() {
  // Mountinfo parsing
  test_normal_line();
  test_optional_fields_and_separator();
  test_unknown_optional_fields();
  test_escaped_spaces_etc();
  test_missing_separator();
  test_missing_fields();
  test_malformed_ids();
  test_malformed_major_minor();
  test_empty_and_blank_input();
  test_unescape();

  // Capacity
  test_capacity_normal();
  test_capacity_frsize_preference();
  test_capacity_bsize_fallback();
  test_capacity_overflow();
  test_capacity_zero_block_size();
  test_capacity_free_greater_than_total();
  test_capacity_available_greater_than_free();
  test_capacity_zero_total();
  test_capacity_precision_stable_across_refreshes();

  // Inodes
  test_inodes_normal();
  test_inodes_zero_total();
  test_inodes_free_greater_than_total();
  test_inodes_overflow_wraps_are_impossible();

  // Classification / mount state
  test_mount_access();
  test_classification_types();
  test_classification_mount_refinement();

  // History
  test_history_stable_identity();
  test_history_appearing_disappearing();
  test_history_mount_point_change();
  test_history_bounded();
  test_history_disabled_and_resumed();
  test_history_one_sample_per_refresh();
  test_history_pseudo_and_failed_are_not_tracked();
  test_history_series_limit();

  std::fprintf(stderr, "\n%zu checks, %d failures\n", g_checks + 0UL,
               g_failures);
  if (g_failures != 0) {
    std::fprintf(stderr, "RESULT: FAIL\n");
    return 1;
  }
  std::fprintf(stderr, "RESULT: PASS\n");
  return 0;
}