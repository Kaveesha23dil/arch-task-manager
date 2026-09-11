#include "advanced_memory_monitor.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <fstream>
#include <istream>
#include <limits>
#include <sstream>
#include <utility>

namespace atm {

namespace {

namespace fs = std::filesystem;

constexpr std::uint64_t kByte = 1;
constexpr std::uint64_t kKibibyte = 1'024;
constexpr std::uint64_t kMebibyte = 1'024 * 1'024;
constexpr std::uint64_t kGibibyte = 1'024 * 1'024 * 1'024;

/// Trims ASCII whitespace off both ends of a string.
std::string trim(std::string text) {
  const auto not_space = [](unsigned char c) { return c > ' '; };
  const auto first = std::find_if(text.begin(), text.end(), not_space);
  if (first == text.end()) {
    return {};
  }
  auto last = std::find_if(text.rbegin(), text.rend(), not_space).base();
  return std::string(first, last);
}

/// Strictly parses a base-10 unsigned 64-bit integer. Accepts only plain
/// digits (no sign) and rejects overflow and trailing garbage.
bool parseU64(const std::string &token, std::uint64_t &out) {
  if (token.empty() || token.front() == '-') {
    return false;
  }
  errno = 0;
  char *end = nullptr;
  const unsigned long long value = std::strtoull(token.c_str(), &end, 10);
  if (errno == ERANGE || end == token.c_str() || *end != '\0') {
    return false;
  }
  out = static_cast<std::uint64_t>(value);
  return true;
}

/// Result of converting a raw value plus a reported unit into bytes.
enum class UnitParse {
  /// The unit is recognized and `bytes` holds the converted value.
  Converted,
  /// No unit reported (or an unrecognized one); `bytes` untouched.
  UnknownUnit,
  /// A recognized unit whose multiplication would overflow; `bytes` untouched.
  Overflow
};

UnitParse convertToBytes(std::uint64_t raw, const std::string &unit,
                         std::uint64_t &bytes) {
  std::uint64_t factor = 0;
  if (unit == "B" || unit == "bytes" || unit == "byte") {
    factor = kByte;
  } else if (unit == "kB" || unit == "KB" || unit == "KiB" || unit == "kb") {
    factor = kKibibyte;
  } else if (unit == "MB" || unit == "MiB") {
    factor = kMebibyte;
  } else if (unit == "GB" || unit == "GiB") {
    factor = kGibibyte;
  } else {
    return UnitParse::UnknownUnit;  // missing (empty) or unknown unit
  }

  if (raw > std::numeric_limits<std::uint64_t>::max() / factor) {
    return UnitParse::Overflow;
  }
  bytes = raw * factor;
  return UnitParse::Converted;
}

/// Saturating add of two optional byte counts (missing parts contribute 0).
/// Returns nullopt only when both operands are unknown.
std::optional<std::uint64_t> satAddBytes(
    const std::optional<std::uint64_t> &a,
    const std::optional<std::uint64_t> &b) {
  if (!a.has_value() && !b.has_value()) {
    return std::nullopt;
  }
  const std::uint64_t x = a.value_or(0);
  const std::uint64_t y = b.value_or(0);
  if (x > std::numeric_limits<std::uint64_t>::max() - y) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return x + y;
}

/// share of a delta over a non-zero total, clamped to [0, 100].
double clampPercent(const std::uint64_t part, const std::uint64_t total) {
  if (total == 0) {
    return 0.0;
  }
  const double ratio = static_cast<double>(part) / static_cast<double>(total);
  return std::clamp(ratio * 100.0, 0.0, 100.0);
}

}  // namespace

bool parseMeminfo(std::istream &in, MeminfoTable &out) {
  out.clear();

  std::string line;
  while (std::getline(in, line)) {
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) {
      continue;  // not a "Key: value unit" field
    }

    const std::string key = trim(line.substr(0, colon));
    if (key.empty()) {
      continue;
    }

    std::string rest = trim(line.substr(colon + 1));
    MeminfoValue value;
    if (rest.empty()) {
      // The key appeared but carried no value token.
      value.status = MeminfoStatus::Malformed;
      out[key] = value;
      continue;
    }

    // The value token then an optional unit token; extra tokens are ignored
    // for forward compatibility.
    std::istringstream rest_parser(rest);
    std::string token;
    if (!(rest_parser >> token)) {
      value.status = MeminfoStatus::Malformed;
      out[key] = value;
      continue;
    }
    if (!parseU64(token, value.raw)) {
      value.status = MeminfoStatus::Malformed;
      out[key] = value;
      continue;
    }

    if (rest_parser >> token) {
      value.unit = token;
    }

    value.status =
        value.raw == 0 ? MeminfoStatus::Zero : MeminfoStatus::Present;

    if (!value.unit.empty()) {
      switch (convertToBytes(value.raw, value.unit, value.bytes)) {
        case UnitParse::Converted:
          value.converted = true;
          break;
        case UnitParse::UnknownUnit:
          // A recognized-unit overflow must not be conflated with an unknown
          // unit; the value is unusable either way, so mark it malformed.
          value.unit_unknown = true;
          break;
        case UnitParse::Overflow:
          value.status = MeminfoStatus::Malformed;
          break;
      }
    }

    out[key] = value;  // duplicate keys: the last occurrence wins
  }

  return true;
}

bool readMeminfoTable(MeminfoTable &out) {
  std::ifstream file("/proc/meminfo");
  if (!file.is_open()) {
    return false;
  }
  return parseMeminfo(file, out);
}

bool parseSwaps(std::istream &in, std::vector<SwapAreaInfo> &out) {
  out.clear();

  std::string line;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty()) {
      continue;
    }

    std::vector<std::string> tokens;
    std::istringstream parser(line);
    std::string token;
    while (parser >> token) {
      tokens.push_back(token);
    }
    if (tokens.size() < 5) {
      continue;  // short/malformed row; cannot be a header either
    }
    if (tokens[0] == "Filename") {
      continue;  // the column header row
    }

    // The trailing four tokens are always Type Size Used Priority; the path is
    // everything before them (a path may legally contain spaces).
    SwapAreaInfo area;
    const std::size_t n = tokens.size();
    area.type = tokens[n - 4];

    std::uint64_t size_kib = 0;
    if (parseU64(tokens[n - 3], size_kib)) {
      std::uint64_t bytes = 0;
      if (convertToBytes(size_kib, "kB", bytes) == UnitParse::Converted) {
        area.size_bytes = bytes;
      }
    }
    std::uint64_t used_kib = 0;
    if (parseU64(tokens[n - 2], used_kib)) {
      std::uint64_t bytes = 0;
      if (convertToBytes(used_kib, "kB", bytes) == UnitParse::Converted) {
        area.used_bytes = bytes;
      }
    }

    errno = 0;
    char *end = nullptr;
    const long priority = std::strtol(tokens[n - 1].c_str(), &end, 10);
    if (errno != ERANGE && end != tokens[n - 1].c_str() && *end == '\0' &&
        priority >= INT_MIN && priority <= INT_MAX) {
      area.priority = priority;
    }

    std::string filename;
    for (std::size_t i = 0; i + 4 < n; ++i) {
      if (i != 0) {
        filename += ' ';
      }
      filename += tokens[i];
    }
    area.filename = filename;

    out.push_back(std::move(area));
  }

  return true;
}

std::optional<std::uint64_t> AdvancedMemorySnapshot::usedBytes() const {
  const std::optional<std::uint64_t> available = availableBytes();
  if (!mem_total.has_value() || !available.has_value()) {
    return std::nullopt;
  }
  // MemAvailable is always <= MemTotal, but clamp rather than wrap.
  return *mem_total > *available ? *mem_total - *available : 0;
}

std::optional<std::uint64_t> AdvancedMemorySnapshot::availableBytes() const {
  if (mem_available.has_value()) {
    return *mem_available;
  }
  // Documented fallback estimate (MemFree + Buffers + Cached). Missing parts
  // contribute nothing; all unknown yields nullopt.
  const std::optional<std::uint64_t> with_buffers =
      satAddBytes(mem_free, buffers);
  return satAddBytes(with_buffers, cached);
}

std::optional<double> AdvancedMemorySnapshot::usedPercent() const {
  if (!mem_total.has_value() || *mem_total == 0) {
    return std::nullopt;
  }
  const std::optional<std::uint64_t> used = usedBytes();
  if (!used.has_value()) {
    return std::nullopt;
  }
  return clampPercent(*used, *mem_total);
}

std::optional<double> AdvancedMemorySnapshot::availablePercent() const {
  if (!mem_total.has_value() || *mem_total == 0) {
    return std::nullopt;
  }
  const std::optional<std::uint64_t> available = availableBytes();
  if (!available.has_value()) {
    return std::nullopt;
  }
  return clampPercent(*available, *mem_total);
}

std::optional<double> AdvancedMemorySnapshot::freePercent() const {
  if (!mem_total.has_value() || *mem_total == 0 || !mem_free.has_value()) {
    return std::nullopt;
  }
  return clampPercent(*mem_free, *mem_total);
}

std::optional<std::uint64_t> AdvancedMemorySnapshot::swapUsedBytes() const {
  if (!swap_total.has_value() || !swap_free.has_value()) {
    return std::nullopt;
  }
  return *swap_total > *swap_free ? *swap_total - *swap_free : 0;
}

std::optional<double> AdvancedMemorySnapshot::swapPercent() const {
  if (!swap_total.has_value() || !swap_free.has_value()) {
    return std::nullopt;
  }
  if (*swap_total == 0) {
    return 0.0;  // no swap configured
  }
  const std::uint64_t used = swapUsedBytes().value_or(0);
  return clampPercent(used, *swap_total);
}

std::optional<std::uint64_t>
AdvancedMemorySnapshot::reclaimableKernelBytes() const {
  return satAddBytes(sreclaimable, kreclaimable);
}

std::optional<std::uint64_t> AdvancedMemorySnapshot::fileBackedBytes() const {
  if (!cached.has_value()) {
    return std::nullopt;
  }
  const std::uint64_t shmem_part = shmem.value_or(0);
  const std::uint64_t non_file = *cached > shmem_part ? *cached - shmem_part : 0;
  return satAddBytes(non_file, buffers);
}

std::optional<std::uint64_t> AdvancedMemorySnapshot::anonymousBytes() const {
  return anon_pages;
}

std::optional<std::uint64_t>
AdvancedMemorySnapshot::kernelMemoryBytes() const {
  const std::optional<std::uint64_t> with_stack =
      satAddBytes(slab, kernel_stack);
  return satAddBytes(with_stack, page_tables);
}

std::optional<double> AdvancedMemorySnapshot::commitPercent() const {
  if (!commit_limit.has_value() || *commit_limit == 0 ||
      !committed_as.has_value()) {
    return std::nullopt;
  }
  // A count can exceed its limit on overcommit-heavy workloads; clamp.
  return clampPercent(*committed_as, *commit_limit);
}

bool AdvancedMemorySnapshot::hasHugePageInfo() const {
  return huge_pages_total.has_value() || huge_pages_free.has_value() ||
         huge_pages_rsvd.has_value() || huge_pages_surp.has_value() ||
         hugepagesize.has_value() || hugetlb.has_value() ||
         anon_huge_pages.has_value() || shmem_huge_pages.has_value() ||
         shmem_pmd_mapped.has_value() || file_huge_pages.has_value() ||
         file_pmd_mapped.has_value() || direct_map_4k.has_value() ||
         direct_map_2m.has_value() || direct_map_1g.has_value();
}

MemoryInfo AdvancedMemorySnapshot::toMemoryInfo() const {
  MemoryInfo info;
  // MemoryInfo mirrors the raw /proc/meminfo values, which are in KiB.
  constexpr std::uint64_t kKib = 1024;
  info.total = mem_total.value_or(0) / kKib;
  info.available = availableBytes().value_or(0) / kKib;
  info.free = mem_free.value_or(0) / kKib;
  info.cached = cached.value_or(0) / kKib;
  info.buffers = buffers.value_or(0) / kKib;
  info.swap_total = swap_total.value_or(0) / kKib;
  info.swap_free = swap_free.value_or(0) / kKib;

  // Preserve the legacy fallback (available==0 means "missing") for the
  // overview/header that predate this breakdown.
  if (info.available == 0 && info.total != 0) {
    info.available = info.free + info.buffers + info.cached;
  }
  return info;
}

namespace {

/// Copies an optional raw value converted to bytes from the table.
std::optional<std::uint64_t> tableBytes(const MeminfoTable &table,
                                        const char *key) {
  const auto it = table.find(key);
  if (it == table.end() || it->second.status == MeminfoStatus::Missing ||
      it->second.status == MeminfoStatus::Malformed) {
    return std::nullopt;
  }
  if (!it->second.converted) {
    return std::nullopt;  // missing/unknown unit — value not usable as bytes
  }
  return it->second.bytes;
}

/// Copies an optional raw paginated count (HugePages_* lines have no unit).
std::optional<std::uint64_t> tableCount(const MeminfoTable &table,
                                        const char *key) {
  const auto it = table.find(key);
  if (it == table.end() || it->second.status == MeminfoStatus::Missing ||
      it->second.status == MeminfoStatus::Malformed) {
    return std::nullopt;
  }
  return it->second.raw;
}

}  // namespace

void buildMemorySnapshot(const MeminfoTable &table,
                         AdvancedMemorySnapshot &out) {
  const bool meminfo_readable = out.meminfo_readable;
  const bool swaps_readable = out.swaps_readable;
  AdvancedMemorySnapshot snap;

  snap.mem_total = tableBytes(table, "MemTotal");
  snap.mem_free = tableBytes(table, "MemFree");
  snap.mem_available = tableBytes(table, "MemAvailable");
  snap.buffers = tableBytes(table, "Buffers");
  snap.cached = tableBytes(table, "Cached");
  snap.shmem = tableBytes(table, "Shmem");
  snap.anon_pages = tableBytes(table, "AnonPages");
  snap.mapped = tableBytes(table, "Mapped");
  snap.active = tableBytes(table, "Active");
  snap.inactive = tableBytes(table, "Inactive");
  snap.active_anon = tableBytes(table, "Active(anon)");
  snap.inactive_anon = tableBytes(table, "Inactive(anon)");
  snap.active_file = tableBytes(table, "Active(file)");
  snap.inactive_file = tableBytes(table, "Inactive(file)");
  snap.unevictable = tableBytes(table, "Unevictable");
  snap.mlocked = tableBytes(table, "Mlocked");

  snap.slab = tableBytes(table, "Slab");
  snap.sreclaimable = tableBytes(table, "SReclaimable");
  snap.sunreclaim = tableBytes(table, "SUnreclaim");
  snap.kreclaimable = tableBytes(table, "KReclaimable");
  snap.kernel_stack = tableBytes(table, "KernelStack");
  snap.page_tables = tableBytes(table, "PageTables");
  snap.vmalloc_total = tableBytes(table, "VmallocTotal");
  snap.vmalloc_used = tableBytes(table, "VmallocUsed");
  snap.vmalloc_chunk = tableBytes(table, "VmallocChunk");
  snap.percpu = tableBytes(table, "Percpu");
  snap.nfs_unstable = tableBytes(table, "NFS_Unstable");
  snap.bounce = tableBytes(table, "Bounce");
  snap.writeback = tableBytes(table, "Writeback");
  snap.writeback_tmp = tableBytes(table, "WritebackTmp");

  snap.swap_total = tableBytes(table, "SwapTotal");
  snap.swap_free = tableBytes(table, "SwapFree");
  snap.swap_cached = tableBytes(table, "SwapCached");

  snap.commit_limit = tableBytes(table, "CommitLimit");
  snap.committed_as = tableBytes(table, "Committed_AS");

  snap.huge_pages_total = tableCount(table, "HugePages_Total");
  snap.huge_pages_free = tableCount(table, "HugePages_Free");
  snap.huge_pages_rsvd = tableCount(table, "HugePages_Rsvd");
  snap.huge_pages_surp = tableCount(table, "HugePages_Surp");
  snap.hugepagesize = tableBytes(table, "Hugepagesize");
  snap.hugetlb = tableBytes(table, "Hugetlb");
  snap.anon_huge_pages = tableBytes(table, "AnonHugePages");
  snap.shmem_huge_pages = tableBytes(table, "ShmemHugePages");
  snap.shmem_pmd_mapped = tableBytes(table, "ShmemPmdMapped");
  snap.file_huge_pages = tableBytes(table, "FileHugePages");
  snap.file_pmd_mapped = tableBytes(table, "FilePmdMapped");
  snap.direct_map_4k = tableBytes(table, "DirectMap4k");
  snap.direct_map_2m = tableBytes(table, "DirectMap2M");
  snap.direct_map_1g = tableBytes(table, "DirectMap1G");
  snap.hardware_corrupted = tableBytes(table, "HardwareCorrupted");

  snap.available_is_estimate = !snap.mem_available.has_value();
  // Preserve the readability flags carried by the caller (they are not derived
  // from the field table and would otherwise be reset by the assignment).
  snap.meminfo_readable = meminfo_readable;
  snap.swaps_readable = swaps_readable;
  out = snap;
}

AdvancedMemoryMonitor::AdvancedMemoryMonitor(std::filesystem::path root)
    : root_(std::move(root)) {}

AdvancedMemorySnapshot AdvancedMemoryMonitor::read() {
  AdvancedMemorySnapshot snapshot;

  {
    std::ifstream file(root_ / "proc" / "meminfo");
    if (file.is_open()) {
      MeminfoTable table;
      snapshot.meminfo_readable = parseMeminfo(file, table);
      if (snapshot.meminfo_readable) {
        buildMemorySnapshot(table, snapshot);
      }
    }
  }

  {
    std::ifstream file(root_ / "proc" / "swaps");
    if (file.is_open()) {
      snapshot.swaps_readable = parseSwaps(file, snapshot.swap_areas);
    }
  }

  return snapshot;
}

}  // namespace atm