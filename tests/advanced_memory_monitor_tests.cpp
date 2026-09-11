#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "advanced_memory_monitor.hpp"
#include "history_manager.hpp"
#include "resource_history.hpp"

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

void expectNear(double a, double b, double eps, const char *expr,
                const char *file, int line) {
  ++g_checks;
  if (!std::isfinite(a) || !(a > b - eps && a < b + eps)) {
    ++g_failures;
    std::fprintf(stderr, "FAIL %s:%d: %s (%f vs %f)\n", file, line, expr, a, b);
  }
}

void run(const char *name) {
  std::fprintf(stderr, "TEST %s\n", name);
}
}  // namespace

#define CHECK(expr) ::expect((expr), #expr, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, eps) \
  ::expectNear(double(a), double(b), (eps), #a " ~ " #b, __FILE__, __LINE__)

namespace fs = std::filesystem;

/// Per-test temp filesystem root mimicking / with a proc tree.
struct TestRoot {
  static std::uint64_t nextId() {
    static std::uint64_t id = 0;
    return id++;
  }

  fs::path root = fs::temp_directory_path() /
                  ("arch-task-manager-mem-" + std::to_string(::getpid()) +
                   "-" + std::to_string(nextId()));

  TestRoot() { fs::create_directories(root); }
  ~TestRoot() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }

  fs::path meminfo() const { return root / "proc" / "meminfo"; }
  fs::path swaps() const { return root / "proc" / "swaps"; }

  void write(const fs::path &path, const std::string &content) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path);
    file << content;
  }

  void writeMeminfo(const std::string &content) { write(meminfo(), content); }
  void writeSwaps(const std::string &content) { write(swaps(), content); }
};

/// Parses `content` as a meminfo stream and returns the table.
atm::MeminfoTable tableFrom(const std::string &content) {
  std::istringstream in(content);
  atm::MeminfoTable table;
  (void)atm::parseMeminfo(in, table);
  return table;
}

/// Builds a snapshot, optionally ignoring /proc/swaps.
atm::AdvancedMemorySnapshot snapshotFrom(const std::string &meminfo) {
  atm::AdvancedMemorySnapshot snap;
  atm::buildMemorySnapshot(tableFrom(meminfo), snap);
  snap.meminfo_readable = true;
  return snap;
}

template <typename H>
bool has(const H &history) {
  return !history.empty();
}

int main() {
  // --- parseMeminfo --------------------------------------------------------
  run("parseMeminfo normal fields with kB conversion");
  {
    const std::string content =
        "MemTotal:       16162988 kB\n"
        "MemFree:         1000000 kB\n"
        "MemAvailable:    8000000 kB\n";
    const atm::MeminfoTable table = tableFrom(content);
    CHECK(table.find("MemTotal") != table.end());
    const atm::MeminfoValue &total = table.at("MemTotal");
    CHECK(total.status == atm::MeminfoStatus::Present);
    CHECK(total.raw == 16162988);
    CHECK(total.unit == "kB");
    CHECK(total.converted);
    CHECK(total.bytes == 16162988ULL * 1024);
  }

  run("parseMeminfo byte conversion");
  {
    const std::string content = "MemFree: 2048 bytes\n";
    const atm::MeminfoTable table = tableFrom(content);
    const atm::MeminfoValue &v = table.at("MemFree");
    CHECK(v.status == atm::MeminfoStatus::Present);
    CHECK(v.unit == "bytes");
    CHECK(v.converted);
    CHECK(v.bytes == 2048);
  }

  run("parseMeminfo MiB / GiB units");
  {
    const std::string content =
        "Buffers: 2 MiB\n"
        "Cached: 3 GiB\n"
        "Slab: 1 MB\n";
    const atm::MeminfoTable table = tableFrom(content);
    CHECK(table.at("Buffers").bytes == 2ULL * 1024 * 1024);
    CHECK(table.at("Cached").bytes == 3ULL * 1024 * 1024 * 1024);
    CHECK(table.at("Slab").bytes == 1ULL * 1024 * 1024);
  }

  run("parseMeminfo missing unit is preserved without conversion");
  {
    // Synthetic: a unit-less line like the HugePages_* page counts.
    const std::string content = "HugePages_Total:       4\n";
    const atm::MeminfoTable table = tableFrom(content);
    const atm::MeminfoValue &v = table.at("HugePages_Total");
    CHECK(v.status == atm::MeminfoStatus::Present);
    CHECK(v.raw == 4);
    CHECK(v.unit.empty());
    CHECK(!v.converted);
    CHECK(!v.unit_unknown);
  }

  run("parseMeminfo unknown unit is preserved");
  {
    const std::string content = "SwapCached: 42 frogs\n";
    const atm::MeminfoTable table = tableFrom(content);
    const atm::MeminfoValue &v = table.at("SwapCached");
    CHECK(v.status == atm::MeminfoStatus::Present);
    CHECK(v.raw == 42);
    CHECK(v.unit == "frogs");
    CHECK(v.unit_unknown);
    CHECK(!v.converted);
  }

  run("parseMeminfo unknown fields are kept for forward compatibility");
  {
    const std::string content = "FutureMetric: 7 kB\nEverything: 1 kB\n";
    const atm::MeminfoTable table = tableFrom(content);
    CHECK(table.find("FutureMetric") != table.end());
    CHECK(table.at("FutureMetric").bytes == 7168);
    CHECK(table.find("Everything") != table.end());
  }

  run("parseMeminfo missing field is distinct from zero");
  {
    const std::string content = "MemFree: 0 kB\n";
    const atm::MeminfoTable table = tableFrom(content);
    CHECK(table.find("MemTotal") == table.end());  // genuinely absent
    const atm::MeminfoValue &zero = table.at("MemFree");
    CHECK(zero.status == atm::MeminfoStatus::Zero);
    CHECK(zero.bytes == 0);
  }

  run("parseMeminfo zero values stay present and converted");
  {
    const std::string content = "SwapTotal: 0 kB\n";
    const atm::MeminfoTable table = tableFrom(content);
    const atm::MeminfoValue &v = table.at("SwapTotal");
    CHECK(v.status == atm::MeminfoStatus::Zero);
    CHECK(v.raw == 0);
    CHECK(v.converted);
    CHECK(v.bytes == 0);
  }

  run("parseMeminfo malformed numeric values are rejected");
  {
    const std::string content =
        "MemTotal: notanumber kB\n"
        "MemFree: -5 kB\n"
        "Buffers: 1.5 kB\n";
    const atm::MeminfoTable table = tableFrom(content);
    CHECK(table.at("MemTotal").status == atm::MeminfoStatus::Malformed);
    CHECK(table.at("MemFree").status == atm::MeminfoStatus::Malformed);
    CHECK(table.at("Buffers").status == atm::MeminfoStatus::Malformed);
  }

  run("parseMeminfo huge values that overflow uint64 are rejected");
  {
    const std::string content = "MemTotal: 99999999999999999999999 kB\n";
    const atm::MeminfoTable table = tableFrom(content);
    CHECK(table.at("MemTotal").status == atm::MeminfoStatus::Malformed);
  }

  run("parseMeminfo unit conversion overflow is rejected safely");
  {
    // raw fits in uint64 but raw * 1024 does not.
    const std::string content =
        "MemTotal: 18446744073709551615 kB\n";
    const atm::MeminfoTable table = tableFrom(content);
    const atm::MeminfoValue &v = table.at("MemTotal");
    CHECK(v.status == atm::MeminfoStatus::Malformed);
    CHECK(!v.converted);
  }

  run("parseMeminfo duplicate fields keep the last occurrence");
  {
    const std::string content =
        "MemTotal: 100 kB\n"
        "MemTotal: 200 kB\n";
    const atm::MeminfoTable table = tableFrom(content);
    CHECK(table.at("MemTotal").raw == 200);
    CHECK(table.at("MemTotal").bytes == 200 * 1024);

    const atm::AdvancedMemorySnapshot snap = snapshotFrom(content);
    CHECK(*snap.mem_total == 200ULL * 1024);
  }

  run("parseMeminfo whitespace and tab variations");
  {
    const std::string content =
        "  MemTotal:   1024  kB  \n"
        "MemFree:\t2048\tkB\n"
        "Swaps:\t0 kB\n";
    const atm::MeminfoTable table = tableFrom(content);
    CHECK(table.at("MemTotal").bytes == 1048576);
    CHECK(table.at("MemFree").bytes == 2097152);
    CHECK(table.at("Swaps").bytes == 0);
  }

  run("parseMeminfo empty input yields an empty table");
  {
    const atm::MeminfoTable table = tableFrom("");
    CHECK(table.empty());
  }

  run("parseMeminfo junk lines without a colon are skipped");
  {
    const std::string content =
        "this line has no colon and is not a field\n"
        "MemTotal: 512 kB\n"
        "---------------------------------------------\n";
    const atm::MeminfoTable table = tableFrom(content);
    CHECK(table.find("MemTotal") != table.end());
    CHECK(table.at("MemTotal").bytes == 512 * 1024);
    CHECK(table.size() == 1);
  }

  run("parseMeminfo a key with an empty value is malformed");
  {
    const std::string content = "MemTotal:\n";
    const atm::MeminfoTable table = tableFrom(content);
    CHECK(table.at("MemTotal").status == atm::MeminfoStatus::Malformed);
  }

  // --- Derived metrics -----------------------------------------------------
  run("derived used/available/free percent from total and available");
  {
    const std::string content =
        "MemTotal: 100000 kB\n"
        "MemAvailable: 60000 kB\n"
        "MemFree: 20000 kB\n";
    const atm::AdvancedMemorySnapshot snap = snapshotFrom(content);
    CHECK(snap.usedBytes().has_value());
    CHECK(*snap.usedBytes() == 40000ULL * 1024);
    CHECK(snap.availableBytes().has_value());
    CHECK(*snap.availableBytes() == 60000ULL * 1024);
    CHECK(!snap.available_is_estimate);
    CHECK_NEAR(*snap.usedPercent(), 40.0, 1e-9);
    CHECK_NEAR(*snap.availablePercent(), 60.0, 1e-9);
    CHECK_NEAR(*snap.freePercent(), 20.0, 1e-9);
  }

  run("derived available falls back to a documented estimate");
  {
    const std::string content =
        "MemTotal: 100000 kB\n"
        "MemFree: 20000 kB\n"
        "Buffers: 5000 kB\n"
        "Cached: 25000 kB\n";
    const atm::AdvancedMemorySnapshot snap = snapshotFrom(content);
    CHECK(snap.available_is_estimate);
    CHECK(snap.availableBytes().has_value());
    CHECK(*snap.availableBytes() == (20000ULL + 5000 + 25000) * 1024);
    // used is still total - available estimate
    CHECK(*snap.usedBytes() == (100000ULL - 50000) * 1024);
    CHECK_NEAR(*snap.usedPercent(), 50.0, 1e-9);
  }

  run("derived usage clamps when available exceeds total");
  {
    const std::string content =
        "MemTotal: 100000 kB\n"
        "MemAvailable: 250000 kB\n";
    const atm::AdvancedMemorySnapshot snap = snapshotFrom(content);
    CHECK(snap.usedBytes().has_value());
    CHECK(*snap.usedBytes() == 0);
    CHECK_NEAR(*snap.usedPercent(), 0.0, 1e-9);
    CHECK_NEAR(*snap.availablePercent(), 100.0, 1e-9);  // clamped
  }

  run("derived percentages are nullopt on zero/unknown totals");
  {
    const atm::AdvancedMemorySnapshot snap;  // all fields missing
    CHECK(!snap.usedPercent().has_value());
    CHECK(!snap.availablePercent().has_value());
    CHECK(!snap.freePercent().has_value());

    const std::string zero = "MemTotal: 0 kB\n";
    const atm::AdvancedMemorySnapshot snap2 = snapshotFrom(zero);
    CHECK(!snap2.usedPercent().has_value());
  }

  run("derived reclaimable kernel memory");
  {
    const std::string content =
        "SReclaimable: 1000 kB\n"
        "KReclaimable: 400 kB\n"
        "SUnreclaim: 300 kB\n";
    const atm::AdvancedMemorySnapshot snap = snapshotFrom(content);
    CHECK(snap.reclaimableKernelBytes().has_value());
    CHECK(*snap.reclaimableKernelBytes() == 1400ULL * 1024);
    CHECK(*snap.sunreclaim == 300ULL * 1024);
  }

  run("derived reclaimable when only SReclaimable exists");
  {
    const std::string content = "SReclaimable: 500 kB\n";
    const atm::AdvancedMemorySnapshot snap = snapshotFrom(content);
    CHECK(*snap.reclaimableKernelBytes() == 500ULL * 1024);

    const atm::AdvancedMemorySnapshot empty;
    CHECK(!empty.reclaimableKernelBytes().has_value());
  }

  run("derived anonymous and file-backed memory");
  {
    const std::string content =
        "MemTotal: 100000 kB\n"
        "Cached: 40000 kB\n"
        "Shmem: 8000 kB\n"
        "Buffers: 2000 kB\n"
        "AnonPages: 18000 kB\n";
    const atm::AdvancedMemorySnapshot snap = snapshotFrom(content);
    CHECK(snap.anonymousBytes().has_value());
    CHECK(*snap.anonymousBytes() == 18000ULL * 1024);
    // File-backed = Cached - Shmem + Buffers.
    CHECK(snap.fileBackedBytes().has_value());
    CHECK(*snap.fileBackedBytes() == (40000ULL - 8000 + 2000) * 1024);
  }

  run("derived file-backed memory clamps at zero");
  {
    const std::string content =
        "Cached: 100 kB\n"
        "Shmem: 300 kB\n";
    const atm::AdvancedMemorySnapshot snap = snapshotFrom(content);
    CHECK(snap.fileBackedBytes().has_value());
    CHECK(*snap.fileBackedBytes() == 0);
  }

  run("derived swap used and percentage");
  {
    const std::string content =
        "SwapTotal: 20000 kB\n"
        "SwapFree: 5000 kB\n"
        "SwapCached: 100 kB\n";
    const atm::AdvancedMemorySnapshot snap = snapshotFrom(content);
    CHECK(*snap.swapUsedBytes() == 15000ULL * 1024);
    CHECK_NEAR(*snap.swapPercent(), 75.0, 1e-9);
    CHECK(*snap.swap_cached == 100ULL * 1024);
  }

  run("derived swap counters clamp when inconsistent");
  {
    const std::string content =
        "SwapTotal: 100 kB\n"
        "SwapFree: 9000 kB\n";
    const atm::AdvancedMemorySnapshot snap = snapshotFrom(content);
    CHECK(*snap.swapUsedBytes() == 0);
    CHECK_NEAR(*snap.swapPercent(), 0.0, 1e-9);
  }

  run("derived swap percent with no swap is 0, unknown is nullopt");
  {
    const std::string content = "SwapTotal: 0 kB\nSwapFree: 0 kB\n";
    const atm::AdvancedMemorySnapshot snap = snapshotFrom(content);
    CHECK(snap.swapPercent().has_value());
    CHECK_NEAR(*snap.swapPercent(), 0.0, 1e-9);

    const atm::AdvancedMemorySnapshot empty;
    CHECK(!empty.swapPercent().has_value());
    CHECK(!empty.swapUsedBytes().has_value());
  }

  run("derived kernel memory estimate");
  {
    const std::string content =
        "Slab: 7000 kB\n"
        "KernelStack: 800 kB\n"
        "PageTables: 900 kB\n";
    const atm::AdvancedMemorySnapshot snap = snapshotFrom(content);
    CHECK(snap.kernelMemoryBytes().has_value());
    CHECK(*snap.kernelMemoryBytes() == (7000ULL + 800 + 900) * 1024);
  }

  run("derived commitment usage");
  {
    const std::string content =
        "CommitLimit: 100000 kB\n"
        "Committed_AS: 30000 kB\n";
    const atm::AdvancedMemorySnapshot snap = snapshotFrom(content);
    CHECK_NEAR(*snap.commitPercent(), 30.0, 1e-9);
  }

  run("derived commitment clamps and guards zero limit");
  {
    const std::string content =
        "CommitLimit: 100 kB\n"
        "Committed_AS: 9000 kB\n";
    const atm::AdvancedMemorySnapshot snap = snapshotFrom(content);
    CHECK(snap.commitPercent().has_value());
    CHECK_NEAR(*snap.commitPercent(), 100.0, 1e-9);  // clamped

    const std::string zero = "CommitLimit: 0 kB\nCommitted_AS: 10 kB\n";
    const atm::AdvancedMemorySnapshot snap2 = snapshotFrom(zero);
    CHECK(!snap2.commitPercent().has_value());

    const atm::AdvancedMemorySnapshot empty;
    CHECK(!empty.commitPercent().has_value());
  }

  run("derived huge-page metrics");
  {
    const std::string content =
        "HugePages_Total:       4\n"
        "HugePages_Free:        1\n"
        "HugePages_Rsvd:        0\n"
        "HugePages_Surp:        0\n"
        "Hugepagesize:       2048 kB\n"
        "Hugetlb:           8192 kB\n"
        "AnonHugePages:      2048 kB\n"
        "DirectMap4k:        1024 kB\n"
        "DirectMap2M:        8192 kB\n"
        "DirectMap1G:             0 kB\n";
    const atm::AdvancedMemorySnapshot snap = snapshotFrom(content);
    CHECK(snap.hasHugePageInfo());
    CHECK(snap.huge_pages_total.has_value());
    CHECK(*snap.huge_pages_total == 4);  // page count, not bytes
    CHECK(*snap.huge_pages_free == 1);
    CHECK(*snap.huge_pages_rsvd == 0);
    CHECK(*snap.hugepagesize == 2048ULL * 1024);
    CHECK(*snap.hugetlb == 8192ULL * 1024);
    CHECK(*snap.anon_huge_pages == 2048ULL * 1024);
    CHECK(*snap.direct_map_1g == 0);

    const atm::AdvancedMemorySnapshot empty;
    CHECK(!empty.hasHugePageInfo());
  }

  run("derived values are bounded and never NaN/inf");
  {
    const std::string content =
        "MemTotal: 100000 kB\n"
        "MemAvailable: 5000 kB\n"
        "MemFree: 3000 kB\n"
        "CommitLimit: 999999 kB\n"
        "Committed_AS: 878787878 kB\n";
    const atm::AdvancedMemorySnapshot snap = snapshotFrom(content);
    for (const double v : {*snap.usedPercent(), *snap.availablePercent(),
                           *snap.freePercent(), *snap.commitPercent()}) {
      CHECK(std::isfinite(v));
      CHECK(v >= 0.0 && v <= 100.0);
    }
  }

  run("toMemoryInfo mirrors the legacy view");
  {
    const std::string content =
        "MemTotal: 100000 kB\n"
        "MemAvailable: 60000 kB\n"
        "MemFree: 20000 kB\n"
        "Cached: 30000 kB\n"
        "Buffers: 1000 kB\n"
        "SwapTotal: 8000 kB\n"
        "SwapFree: 1000 kB\n";
    const atm::AdvancedMemorySnapshot snap = snapshotFrom(content);
    const atm::MemoryInfo info = snap.toMemoryInfo();
    CHECK(info.total == 100000);
    CHECK(info.available == 60000);
    CHECK(info.free == 20000);
    CHECK(info.cached == 30000);
    CHECK(info.buffers == 1000);
    CHECK(info.swap_total == 8000);
    CHECK(info.swap_free == 1000);
    CHECK(info.used() == 40000);
    CHECK(info.swapUsed() == 7000);
    CHECK_NEAR(info.usagePercent(), 40.0, 1e-9);
    CHECK_NEAR(info.swapUsagePercent(), 87.5, 1e-9);
  }

  run("toMemoryInfo falls back to the estimate when available missing");
  {
    const std::string content =
        "MemTotal: 100000 kB\n"
        "MemFree: 20000 kB\n"
        "Buffers: 5000 kB\n"
        "Cached: 25000 kB\n";
    const atm::MemoryInfo info = snapshotFrom(content).toMemoryInfo();
    CHECK(info.available == 50000);
    CHECK(info.used() == 50000);
  }

  // --- parseSwaps ----------------------------------------------------------
  run("parseSwaps header and multiple areas");
  {
    const std::string content =
        "Filename\t\t\t\tType\t\tSize\tUsed\tPriority\n"
        "/dev/sda2                               partition\t1048572\t12345\t-2\n"
        "/swapfile                               file\t\t1048576\t0\t-2\n";
    std::istringstream in(content);
    std::vector<atm::SwapAreaInfo> areas;
    CHECK(atm::parseSwaps(in, areas));
    CHECK(areas.size() == 2);
    CHECK(areas[0].filename == "/dev/sda2");
    CHECK(areas[0].type == "partition");
    CHECK(areas[0].size_bytes.has_value());
    CHECK(*areas[0].size_bytes == 1048572ULL * 1024);
    CHECK(*areas[0].used_bytes == 12345ULL * 1024);
    CHECK(areas[0].priority.has_value());
    CHECK(*areas[0].priority == -2);
    CHECK(areas[1].filename == "/swapfile");
    CHECK(areas[1].type == "file");
    CHECK(*areas[1].size_bytes == 1048576ULL * 1024);
    CHECK(*areas[1].used_bytes == 0);
  }

  run("parseSwaps encrypted swap names");
  {
    const std::string content =
        "Filename\t\t\t\tType\t\tSize\tUsed\tPriority\n"
        "/dev/mapper/cryptswap                        partition\t2097148\t0\t-2\n";
    std::istringstream in(content);
    std::vector<atm::SwapAreaInfo> areas;
    CHECK(atm::parseSwaps(in, areas));
    CHECK(areas.size() == 1);
    CHECK(areas[0].filename == "/dev/mapper/cryptswap");
    CHECK(areas[0].type == "partition");
  }

  run("parseSwaps paths containing spaces");
  {
    const std::string content =
        "Filename\t\t\t\tType\t\tSize\tUsed\tPriority\n"
        "/my swap dir/swap file              file\t\t512\t0\t-1\n";
    std::istringstream in(content);
    std::vector<atm::SwapAreaInfo> areas;
    CHECK(atm::parseSwaps(in, areas));
    CHECK(areas.size() == 1);
    CHECK(areas[0].filename == "/my swap dir/swap file");
    CHECK(areas[0].type == "file");
    CHECK(*areas[0].size_bytes == 512ULL * 1024);
  }

  run("parseSwaps invalid rows are skipped");
  {
    std::istringstream in("only three tokens here\n");
    std::vector<atm::SwapAreaInfo> areas;
    CHECK(atm::parseSwaps(in, areas));
    CHECK(areas.empty());
  }

  run("parseSwaps empty input yields no areas");
  {
    std::istringstream in("");
    std::vector<atm::SwapAreaInfo> areas;
    CHECK(atm::parseSwaps(in, areas));
    CHECK(areas.empty());

    std::istringstream header("Filename\t\tType\t\tSize\tUsed\tPriority\n");
    std::vector<atm::SwapAreaInfo> only_header;
    CHECK(atm::parseSwaps(header, only_header));
    CHECK(only_header.empty());
  }

  run("parseSwaps invalid sizes and priorities degrade safely");
  {
    const std::string content =
        "Filename\t\t\t\tType\t\tSize\tUsed\tPriority\n"
        "/bad                                     partition\tbig\tnotausd\tzz\n"
        "/ok                                      file\t\t1024\t0\t-2\n";
    std::istringstream in(content);
    std::vector<atm::SwapAreaInfo> areas;
    CHECK(atm::parseSwaps(in, areas));
    CHECK(areas.size() == 2);
    CHECK(!areas[0].size_bytes.has_value());
    CHECK(!areas[0].used_bytes.has_value());
    CHECK(!areas[0].priority.has_value());
    CHECK(areas[0].filename == "/bad");
    CHECK(areas[1].size_bytes.has_value());
    CHECK(*areas[1].size_bytes == 1024ULL * 1024);
    CHECK(*areas[1].priority == -2);
  }

  // --- AdvancedMemoryMonitor -----------------------------------------------
  run("monitor reads meminfo and swaps through a synthetic root");
  {
    TestRoot root;
    root.writeMeminfo(
        "MemTotal:       10000 kB\n"
        "MemAvailable:    4000 kB\n"
        "MemFree:         1000 kB\n"
        "Cached:          3000 kB\n"
        "Buffers:          500 kB\n"
        "SwapTotal:        2000 kB\n"
        "SwapFree:          300 kB\n"
        "SReclaimable:      200 kB\n"
        "KReclaimable:      100 kB\n"
        "CommitLimit:      5000 kB\n"
        "Committed_AS:     2000 kB\n");
    root.writeSwaps(
        "Filename\t\t\t\tType\t\tSize\tUsed\tPriority\n"
        "/dev/sdb1                               partition\t2048\t128\t-2\n");
    atm::AdvancedMemoryMonitor monitor(root.root);
    const atm::AdvancedMemorySnapshot snap = monitor.read();
    CHECK(snap.meminfo_readable);
    CHECK(snap.swaps_readable);
    CHECK(*snap.mem_total == 10000ULL * 1024);
    CHECK(*snap.mem_available == 4000ULL * 1024);
    CHECK(*snap.swap_total == 2000ULL * 1024);
    CHECK(snap.reclaimableKernelBytes().has_value());
    CHECK(*snap.reclaimableKernelBytes() == 300ULL * 1024);
    CHECK_NEAR(*snap.commitPercent(), 40.0, 1e-9);
    CHECK(snap.swap_areas.size() == 1);
    CHECK(snap.swap_areas[0].filename == "/dev/sdb1");
    CHECK(*snap.swap_areas[0].size_bytes == 2048ULL * 1024);
  }

  run("monitor missing meminfo leaves readable flag false");
  {
    TestRoot root;  // no /proc/meminfo
    atm::AdvancedMemoryMonitor monitor(root.root);
    const atm::AdvancedMemorySnapshot snap = monitor.read();
    CHECK(!snap.meminfo_readable);
    CHECK(!snap.mem_total.has_value());
    CHECK(!snap.usedPercent().has_value());
    CHECK(!snap.swaps_readable);
    CHECK(snap.swap_areas.empty());
  }

  run("monitor missing swaps leaves areas empty without failing meminfo");
  {
    TestRoot root;
    root.writeMeminfo("MemTotal: 1000 kB\n");
    atm::AdvancedMemoryMonitor monitor(root.root);
    const atm::AdvancedMemorySnapshot snap = monitor.read();
    CHECK(snap.meminfo_readable);
    CHECK(*snap.mem_total == 1000ULL * 1024);
    CHECK(snap.swap_areas.empty());
  }

  run("monitor swap areas reflect disappearance between refreshes");
  {
    TestRoot root;
    root.writeMeminfo("SwapTotal: 2000 kB\nSwapFree: 300 kB\n");
    root.writeSwaps(
        "Filename\tType\tSize\tUsed\tPriority\n"
        "/swap1\tpartition\t1024\t0\t-2\n"
        "/swap2\tfile\t1024\t0\t-2\n");
    atm::AdvancedMemoryMonitor monitor(root.root);
    const atm::AdvancedMemorySnapshot first = monitor.read();
    CHECK(first.swap_areas.size() == 2);

    root.writeSwaps(
        "Filename\tType\tSize\tUsed\tPriority\n"
        "/swap1\tpartition\t1024\t0\t-2\n");
    const atm::AdvancedMemorySnapshot second = monitor.read();
    CHECK(second.swap_areas.size() == 1);
    CHECK(second.swap_areas[0].filename == "/swap1");
  }

  // --- History integration ---------------------------------------------------
  run("history advanced memory records one sample per available metric");
  {
    atm::HistoryManager history;
    atm::AdvancedMemoryMetrics metrics;
    metrics.available_percent = 40.0;
    metrics.cached_bytes = 1234.0;
    metrics.reclaimable_bytes = 56.0;
    metrics.commitment_percent = 30.0;
    history.updateAdvancedMemory(metrics);
    CHECK(history.memoryAvailablePercentHistory().size() == 1);
    CHECK(history.cachedBytesHistory().size() == 1);
    CHECK(history.reclaimableBytesHistory().size() == 1);
    CHECK(history.commitmentPercentHistory().size() == 1);
  }

  run("history advanced memory skips unavailable metrics");
  {
    atm::HistoryManager history;
    atm::AdvancedMemoryMetrics partial;
    partial.cached_bytes = 1.0;  // only cached available
    history.updateAdvancedMemory(partial);
    CHECK(history.memoryAvailablePercentHistory().empty());
    CHECK(history.cachedBytesHistory().size() == 1);
    CHECK(history.reclaimableBytesHistory().empty());
    CHECK(history.commitmentPercentHistory().empty());
  }

  run("history advanced memory empty update contributes no samples");
  {
    atm::HistoryManager history;
    atm::AdvancedMemoryMetrics none;
    history.updateAdvancedMemory(none);
    CHECK(has(history.cachedBytesHistory()) == false);
    CHECK(has(history.commitmentPercentHistory()) == false);
  }

  run("history advanced memory respects pause");
  {
    atm::HistoryManager history;
    history.setPaused(true);
    atm::AdvancedMemoryMetrics metrics;
    metrics.available_percent = 50.0;
    metrics.cached_bytes = 2.0;
    metrics.reclaimable_bytes = 3.0;
    metrics.commitment_percent = 4.0;
    history.updateAdvancedMemory(metrics);
    history.updateAdvancedMemory(metrics);
    CHECK(history.cachedBytesHistory().empty());
    CHECK(history.commitmentPercentHistory().empty());

    history.setPaused(false);
    history.updateAdvancedMemory(metrics);
    CHECK(history.cachedBytesHistory().size() == 1);
  }

  run("history advanced memory is bounded by max samples");
  {
    atm::HistoryManager history(4);
    atm::AdvancedMemoryMetrics metrics;
    metrics.available_percent = 10.0;
    metrics.cached_bytes = 1.0;
    metrics.reclaimable_bytes = 2.0;
    metrics.commitment_percent = 3.0;
    for (int i = 0; i < 10; ++i) {
      history.updateAdvancedMemory(metrics);
    }
    CHECK(history.cachedBytesHistory().size() == 4);
    CHECK(history.commitmentPercentHistory().size() == 4);
  }

  run("history advanced memory has no duplicate samples per refresh");
  {
    atm::HistoryManager history;
    atm::AdvancedMemoryMetrics metrics;
    metrics.available_percent = 42.0;
    metrics.cached_bytes = 7.0;
    metrics.reclaimable_bytes = 8.0;
    metrics.commitment_percent = 9.0;
    history.updateAdvancedMemory(metrics);
    CHECK(history.memoryAvailablePercentHistory().size() == 1);
    CHECK(history.cachedBytesHistory().size() == 1);
    CHECK(history.reclaimableBytesHistory().size() == 1);
    CHECK(history.commitmentPercentHistory().size() == 1);
  }

  run("history advanced memory timestamps are consistent within a refresh");
  {
    atm::HistoryManager history;
    atm::AdvancedMemoryMetrics metrics;
    metrics.available_percent = 1.0;
    metrics.cached_bytes = 2.0;
    metrics.reclaimable_bytes = 3.0;
    metrics.commitment_percent = 4.0;
    history.updateAdvancedMemory(metrics);
    const auto init = std::chrono::steady_clock::now();
    atm::AdvancedMemoryMetrics next;
    next.available_percent = 2.0;
    next.cached_bytes = 3.0;
    next.reclaimable_bytes = 4.0;
    next.commitment_percent = 5.0;
    history.updateAdvancedMemory(next);

    const auto &avail = history.memoryAvailablePercentHistory().samples();
    const auto &cached = history.cachedBytesHistory().samples();
    const auto &reclaim = history.reclaimableBytesHistory().samples();
    const auto &commit = history.commitmentPercentHistory().samples();

    // Every series captured one sample per refresh.
    CHECK(avail.size() == 2 && cached.size() == 2 && reclaim.size() == 2 &&
          commit.size() == 2);
    // Samples from the same refresh share one timestamp (the updateAdvances a
    // single clock read for the whole batch).
    CHECK(avail.back().timestamp == cached.back().timestamp);
    CHECK(cached.back().timestamp == reclaim.back().timestamp);
    CHECK(reclaim.back().timestamp == commit.back().timestamp);
    CHECK(avail.front().timestamp == commit.front().timestamp);
    // The later refresh must not be timestamped before the earlier one.
    CHECK(avail.back().timestamp >= init);
    CHECK(avail.back().timestamp >= avail.front().timestamp);
  }

  run("history advanced memory survives clearAll");
  {
    atm::HistoryManager history;
    atm::AdvancedMemoryMetrics metrics;
    metrics.available_percent = 5.0;
    metrics.cached_bytes = 6.0;
    history.updateAdvancedMemory(metrics);
    history.clearAll();
    CHECK(history.cachedBytesHistory().empty());
    CHECK(history.commitmentPercentHistory().empty());
  }

  std::fprintf(stderr, "\n%s (%d checks)\n",
               g_failures == 0 ? "PASS" : "FAIL", g_checks);
  return g_failures == 0 ? 0 : 1;
}