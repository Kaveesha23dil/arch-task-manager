#pragma once

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "memory_monitor.hpp"

namespace atm {

/// What happened to one named field while parsing /proc/meminfo.
enum class MeminfoStatus {
  /// The field's key never appeared in the input.
  Missing,
  /// The key appeared but its value could not be trusted (malformed number,
  /// or a recognized unit whose byte conversion overflowed).
  Malformed,
  /// The key appeared and parsed as the value zero.
  Zero,
  /// The key appeared and parsed as a value greater than zero.
  Present
};

/**
 * One raw /proc/meminfo value.
 *
 * `raw` is the numeric token exactly as printed; `unit` is the reported unit
 * token ("kB", "bytes", ...) which may be empty when the kernel printed no
 * unit (e.g. the HugePages_* page counts). `bytes` is `raw` converted to
 * bytes for a recognized decimal unit and only meaningful when the value is
 * Present and the unit was recognized; missing units and unknown units leave
 * `bytes` at 0 while the raw value is preserved. The distinction between a
 * present zero, a missing field, a malformed field and an unknown/absent unit
 * is always preserved so callers never conflate them.
 */
struct MeminfoValue {
  MeminfoStatus status = MeminfoStatus::Missing;
  std::uint64_t raw = 0;
  std::string unit;
  std::uint64_t bytes = 0;
  bool unit_unknown = false;  // a unit token was present but not recognized
  bool converted = false;     // `bytes` holds a valid byte conversion
};

/// A parsed /proc/meminfo table keyed by field name ("MemTotal", "SwapFree",
/// "AnonHugePages", unknown fields kept for forward compatibility, ...).
using MeminfoTable = std::map<std::string, MeminfoValue>;

/**
 * Parses a /proc/meminfo stream ("Key:       value kB", one field per line)
 * into the field table.
 *
 * Lines without a ':' separator are ignored. A field whose value token is not
 * a plain base-10 unsigned integer is recorded as Malformed; any unit that is
 * not one of the recognized decimal units leaves the raw value intact with
 * `unit_unknown` set. Unknown field names are kept verbatim in the table — an
 * unrecognized key never fails the parse and never drops other fields.
 * Duplicate keys keep the last occurrence. Empty input yields an empty table.
 */
[[nodiscard]] bool parseMeminfo(std::istream &in, MeminfoTable &out);

/// Opens and parses the real /proc/meminfo. Returns false only when the file
/// cannot be opened.
[[nodiscard]] bool readMeminfoTable(MeminfoTable &out);

/**
 * One configured swap area, parsed from /proc/swaps.
 *
 * The area path (Filename) is the only field that may legally contain
 * whitespace, so rows are split from the right: Type Size Used Priority always
 * occupy the trailing four tokens and everything before them is the path. A
 * row with fewer than five tokens, or the header row, is skipped. Unparseable
 * Size/Used notes leave the corresponding fields unset rather than failing the
 * whole line. Sizes are converted from kB to bytes. Priorities are signed
 * integers (negative priorities are normal) and may be unset when malformed.
 */
struct SwapAreaInfo {
  std::string filename;
  std::string type;
  std::optional<std::uint64_t> size_bytes;
  std::optional<std::uint64_t> used_bytes;
  std::optional<long> priority;
};

/// Parses a /proc/swaps stream into the configured swap areas. Returns true
/// when the stream was readable; areas holds whatever valid rows were found.
[[nodiscard]] bool parseSwaps(std::istream &in, std::vector<SwapAreaInfo> &out);

/**
 * Complete advanced memory snapshot produced by AdvancedMemoryMonitor::read().
 *
 * Every field mirrors the /proc/meminfo counter of the same name; sizes are in
 * bytes and are std::nullopt when the field is missing, malformed or was not
 * reported by the kernel. HugePages_* counts are raw page counts. The derived
 * helpers document their formulas and never produce negative values, NaNs,
 * infinities or percentages outside [0, 100]; they return only the data that
 * supports them.
 */
struct AdvancedMemorySnapshot {
  bool meminfo_readable = false;
  bool swaps_readable = false;
  /// True when MemAvailable is not reported and `available` falls back to a
  /// documented estimate (MemFree + Buffers + Cached).
  bool available_is_estimate = false;

  // --- Physical memory (bytes) ---------------------------------------------
  std::optional<std::uint64_t> mem_total;
  std::optional<std::uint64_t> mem_free;
  std::optional<std::uint64_t> mem_available;
  std::optional<std::uint64_t> buffers;
  std::optional<std::uint64_t> cached;
  std::optional<std::uint64_t> shmem;
  std::optional<std::uint64_t> anon_pages;
  std::optional<std::uint64_t> mapped;
  std::optional<std::uint64_t> active;
  std::optional<std::uint64_t> inactive;
  std::optional<std::uint64_t> active_anon;
  std::optional<std::uint64_t> inactive_anon;
  std::optional<std::uint64_t> active_file;
  std::optional<std::uint64_t> inactive_file;
  std::optional<std::uint64_t> unevictable;
  std::optional<std::uint64_t> mlocked;

  // --- Slab / kernel accounting (bytes) -------------------------------------
  std::optional<std::uint64_t> slab;
  std::optional<std::uint64_t> sreclaimable;
  std::optional<std::uint64_t> sunreclaim;
  std::optional<std::uint64_t> kreclaimable;
  std::optional<std::uint64_t> kernel_stack;
  std::optional<std::uint64_t> page_tables;
  std::optional<std::uint64_t> vmalloc_total;
  std::optional<std::uint64_t> vmalloc_used;
  std::optional<std::uint64_t> vmalloc_chunk;
  std::optional<std::uint64_t> percpu;
  std::optional<std::uint64_t> nfs_unstable;
  std::optional<std::uint64_t> bounce;
  std::optional<std::uint64_t> writeback;
  std::optional<std::uint64_t> writeback_tmp;

  // --- Swap (bytes) and areas ----------------------------------------------
  std::optional<std::uint64_t> swap_total;
  std::optional<std::uint64_t> swap_free;
  std::optional<std::uint64_t> swap_cached;
  std::vector<SwapAreaInfo> swap_areas;

  // --- Memory commitment (virtual, not resident) -----------------------------
  std::optional<std::uint64_t> commit_limit;
  std::optional<std::uint64_t> committed_as;

  // --- Huge pages: *_count are page counts, everything else is bytes ---------
  std::optional<std::uint64_t> huge_pages_total;
  std::optional<std::uint64_t> huge_pages_free;
  std::optional<std::uint64_t> huge_pages_rsvd;
  std::optional<std::uint64_t> huge_pages_surp;
  std::optional<std::uint64_t> hugepagesize;
  std::optional<std::uint64_t> hugetlb;
  std::optional<std::uint64_t> anon_huge_pages;
  std::optional<std::uint64_t> shmem_huge_pages;
  std::optional<std::uint64_t> shmem_pmd_mapped;
  std::optional<std::uint64_t> file_huge_pages;
  std::optional<std::uint64_t> file_pmd_mapped;
  std::optional<std::uint64_t> direct_map_4k;
  std::optional<std::uint64_t> direct_map_2m;
  std::optional<std::uint64_t> direct_map_1g;
  std::optional<std::uint64_t> hardware_corrupted;

  // --- Derived metrics (see formulas in the header; never NaN/inf) ----------

  /// Used RAM = MemTotal - MemAvailable (preferred); falls back to the
  /// documented estimate when MemAvailable is absent. Clamped at zero.
  [[nodiscard]] std::optional<std::uint64_t> usedBytes() const;

  /// Available RAM: MemAvailable when present, else the estimate
  /// MemFree + Buffers + Cached (see `available_is_estimate`).
  [[nodiscard]] std::optional<std::uint64_t> availableBytes() const;

  /// Usage percentage of total RAM, clamped to [0, 100]. Nullopt when total is
  /// unknown or zero.
  [[nodiscard]] std::optional<double> usedPercent() const;
  [[nodiscard]] std::optional<double> availablePercent() const;
  [[nodiscard]] std::optional<double> freePercent() const;

  /// Used swap = SwapTotal - SwapFree, clamped at zero. Nullopt when either
  /// swap counter is unknown.
  [[nodiscard]] std::optional<std::uint64_t> swapUsedBytes() const;

  /// Swap usage percent of SwapTotal, clamped to [0, 100]. Nullopt when
  /// SwapTotal is unknown or zero (no swap / unknown swap). A present zero
  /// SwapTotal yields 0%.
  [[nodiscard]] std::optional<double> swapPercent() const;

  /// Reclaimable kernel memory = SReclaimable + KReclaimable. On kernels that
  /// report only one of the two the other simply contributes nothing; both
  /// unknown yields nullopt. Nothing here negates or double-counts slab.
  [[nodiscard]] std::optional<std::uint64_t> reclaimableKernelBytes() const;

  /// File-backed memory = Cached - Shmem + Buffers. Cached counts tmpfs/shmem
  /// pages, which are anonymous not file-backed, so Shmem is subtracted.
  /// Clamped at zero; nullopt when Cached is unknown.
  [[nodiscard]] std::optional<std::uint64_t> fileBackedBytes() const;

  /// Anonymous process memory = AnonPages.
  [[nodiscard]] std::optional<std::uint64_t> anonymousBytes() const;

  /// Kernel memory estimate = Slab + KernelStack + PageTables. Deliberately
  /// does not add VmallocUsed (vmalloc pages are already accounted inside the
  /// page allocator counters) to avoid double counting. Nullopt when none of
  /// the parts is known.
  [[nodiscard]] std::optional<std::uint64_t> kernelMemoryBytes() const;

  /// Commitment usage = Committed_AS / CommitLimit * 100, clamped to [0, 100].
  /// Nullopt when CommitLimit is missing or not greater than zero. This is a
  /// virtual-memory commitment figure, not resident RAM.
  [[nodiscard]] std::optional<double> commitPercent() const;

  /// True when any huge-page field was reported.
  [[nodiscard]] bool hasHugePageInfo() const;

  /// The classical MemoryInfo view built from this snapshot, for consumers
  /// (header, alerts, process table) that predate the breakdown. Available
  /// falls back to the estimate when MemAvailable is missing, keeping the
  /// existing monitor behavior unchanged.
  [[nodiscard]] MemoryInfo toMemoryInfo() const;
};

/// Builds every derived field of a snapshot from its raw parsed table.
void buildMemorySnapshot(const MeminfoTable &table, AdvancedMemorySnapshot &out);

/**
 * Monitors physical memory, composition, swap and commitment by reading
 * /proc/meminfo and /proc/swaps on every read(). This is a point-in-time
 * snapshot (no running counters to diff), so the monitor holds no state. The
 * refresh cadence is owned by the application's main loop — this class never
 * starts its own thread and never reads either file more than once per refresh.
 *
 * `root` points at an alternate filesystem root (used by tests).
 *
 * Read-only: swap is never enabled/disabled/resized and huge-page
 * configuration is never changed.
 */
class AdvancedMemoryMonitor {
 public:
  explicit AdvancedMemoryMonitor(std::filesystem::path root = "/");
  ~AdvancedMemoryMonitor() = default;

  AdvancedMemoryMonitor(const AdvancedMemoryMonitor &) = delete;
  AdvancedMemoryMonitor &operator=(const AdvancedMemoryMonitor &) = delete;

  /// Reads /proc/meminfo and /proc/swaps once and returns the snapshot.
  [[nodiscard]] AdvancedMemorySnapshot read();

 private:
  std::filesystem::path root_;
};

}  // namespace atm