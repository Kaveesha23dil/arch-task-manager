#pragma once

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace atm {

/**
 * Raw CPU time counters read from one /proc/stat "cpu..." line.
 *
 * Every value counts CPU ticks spent in that state since boot, in units of
 * USER_HZ (normally 1/100 of a second). The `cpu_id` identifies the logical
 * CPU the counters belong to; the aggregate "cpu " line uses -1. Unlike the
 * simple CpuTimes model this also captures the optional trailing
 * guest/guest_nice columns.
 */
struct CpuCounterSnapshot {
  int cpu_id = -1;  // -1 for the aggregate "cpu " line, else a logical CPU ID
  std::uint64_t user = 0;
  std::uint64_t nice = 0;
  std::uint64_t system = 0;
  std::uint64_t idle = 0;
  std::uint64_t iowait = 0;
  std::uint64_t irq = 0;
  std::uint64_t softirq = 0;
  std::uint64_t steal = 0;
  std::uint64_t guest = 0;
  std::uint64_t guest_nice = 0;

  /// Total ticks across all non-guest states. The kernel already accounts for
  /// guest/guest_nice inside user/nice, so adding them again would double-count
  /// guest execution.
  [[nodiscard]] std::uint64_t total() const;

  /// Idle-like ticks: `idle` plus `iowait` (time a task waits on I/O while the
  /// CPU is idle).
  [[nodiscard]] std::uint64_t idleTime() const;

  /// Busy ticks: everything that is not idle-like.
  [[nodiscard]] std::uint64_t busy() const;
};

/**
 * Parses one /proc/stat line into a counter snapshot.
 *
 * Accepts the aggregate line ("cpu "), per-logical-CPU lines ("cpu0", "cpu12")
 * and returns false for any unrelated line ("intr", "ctxt", ...). The standard
 * field order is user, nice, system, idle, iowait, irq, softirq, steal, guest,
 * guest_nice. Only the first four are required — older or partially populated
 * lines fill the missing trailing fields with 0; extra fields are ignored. Any
 * malformed numeric field (empty, non-numeric, negative or overflowing a
 * 64-bit unsigned value) makes the whole line invalid.
 */
[[nodiscard]] bool parseCpuCounterLine(const std::string &line,
                                       CpuCounterSnapshot &out);

/// The parsed content of /proc/stat: the aggregate line plus every per-CPU
/// line, keyed by logical CPU ID (-1 for the aggregate "cpu " line).
struct ProcStatSnapshot {
  bool has_aggregate = false;
  std::map<int, CpuCounterSnapshot> counters;
};

/**
 * Parses every "cpu..." line from a /proc/stat stream. Unrelated lines are
 * skipped. Returns true as soon as at least one meaningful CPU line was parsed
 * or when the file was readable at all; on a completely empty or broken input
 * the `counters` map stays empty.
 *
 * This helper is exposed so tests can feed synthetic /proc/stat content
 * without touching the real file.
 */
[[nodiscard]] bool parseProcStat(std::istream &in, ProcStatSnapshot &out);

/// Opens and parses the real /proc/stat. Returns false when the file cannot be
/// opened.
[[nodiscard]] bool readProcStat(ProcStatSnapshot &out);

/**
 * One CPU's time breakdown as percentages of the elapsed CPU time within the
 * sampling interval. All values are bounded to [0, 100] and finite, or
 * `valid` is false (see computeCpuDelta).
 */
struct CpuDeltaPercentages {
  bool valid = false;
  double user_percent = 0.0;
  double nice_percent = 0.0;
  double system_percent = 0.0;
  double idle_percent = 0.0;
  double iowait_percent = 0.0;
  double irq_percent = 0.0;
  double softirq_percent = 0.0;
  double steal_percent = 0.0;
  /// Total busy: 100 - idle - iowait.
  double busy_percent = 0.0;
};

/**
 * Computes the CPU time breakdown between two counter snapshots taken from
 * /proc/stat.
 *
 * Utilisation convention (consistent with the aggregate CpuMonitor):
 *
 *     idle_time   = idle + iowait
 *     busy_time   = total_time - idle_time
 *     utilization = busy_time / total_time * 100
 *
 * guest/guest_nice are already counted inside user/nice by the kernel and are
 * therefore excluded from `total()` so they are never double-counted.
 *
 * Every category is reported as a percentage of the elapsed total time, not of
 * real time, and is clamped to [0, 100]. `out.valid` is set to false (with all
 * percentages left at 0) whenever a meaningful delta cannot be computed:
 * zero elapsed ticks, a counter decrease or reset (including suspend/resume or
 * VM migration), or any missing/inconsistent counter. The result never
 * contains NaN, infinities, negatives or values above 100.
 */
void computeCpuDelta(const CpuCounterSnapshot &previous,
                     const CpuCounterSnapshot &current,
                     CpuDeltaPercentages &out);

/// Parseable topology masks read from /sys/devices/system/cpu.
struct CpuTopologyInfo {
  std::vector<int> online;    // logical CPUs currently online (sorted)
  std::vector<int> possible;  // logical CPUs the kernel could ever bring online
  std::vector<int> present;   // logical CPUs present in the system
  bool online_available = false;
  bool possible_available = false;
  bool present_available = false;
};

/**
 * Parses a sysfs CPU mask/range list such as "0-3,8-11" or "0,1,4" into a
 * sorted list of logical CPU IDs. Returns false for empty or malformed text.
 */
[[nodiscard]] bool parseCpuList(const std::string &text, std::vector<int> &out);

/// Reads the online/possible/present masks of the system. Any file that is
/// missing, unreadable or unparseable leaves its vector empty and the
/// corresponding *_available flag false; a failure never aborts the read.
[[nodiscard]] CpuTopologyInfo readCpuTopology();

/**
 * Optional kernel cpufreq information for one CPU.
 *
 * `has_policy` is set only when the CPU exposes a cpufreq directory. Values are
 * treated as kHz as reported by the kernel. A CPU that shares a frequency
 * policy with other CPUs reports the same values as its peers — the UI must not
 * imply an independent per-CPU policy. Nothing here is a guaranteed hardware
 * clock frequency; the values are simply what the kernel currently reports.
 */
struct CpuFrequencyInfo {
  bool has_policy = false;  // cpufreq directory exists for this CPU
  std::optional<std::uint64_t> current_khz;
  std::optional<std::uint64_t> min_khz;
  std::optional<std::uint64_t> max_khz;
  std::optional<std::string> governor;
};

/**
 * Reads the optional cpufreq information for a logical CPU from sysfs.
 *
 * `scaling_cur_freq` is preferred over `cpuinfo_cur_freq`; scaling_min_freq,
 * scaling_max_freq and scaling_governor complete the picture. Missing,
 * invalid or permission-restricted files simply leave the corresponding field
 * unset; a missing cpufreq directory leaves everything unset with
 * `has_policy == false`. This never fails, throws or logs spam for the expected
 * absence of these optional files.
 *
 * `sys_root` lets tests point at a synthetic sysfs tree; production uses "/".
 */
[[nodiscard]] CpuFrequencyInfo readCpuFrequency(
    int cpu_id, const std::filesystem::path &sys_root = "/");

/// Formats a cpufreq value (kHz) for the UI, e.g. "2.40 GHz" or "800 MHz".
/// Returns "N/A" when unset so an unavailable frequency is never faked.
[[nodiscard]] std::string formatCpuFrequency(const std::optional<std::uint64_t> &khz);

/// Formats a cpufreq governor for the UI. Returns "N/A" when unset.
[[nodiscard]] std::string formatCpuGovernor(const std::optional<std::string> &governor);

/**
 * Computed metrics for one logical CPU (or the aggregate, cpu_id == -1).
 *
 * `online` reflects the current online/offline state, `has_sample` whether a
 * valid delta was computed this refresh (false on the very first read of a CPU,
 * after a counter reset, or for offline CPUs) and `delta` the corresponding
 * percentages. `frequency` is only populated for online CPUs.
 */
struct CpuStatistics {
  int cpu_id = -1;
  bool online = false;
  bool has_sample = false;
  CpuDeltaPercentages delta;
  CpuFrequencyInfo frequency;
};

/// One complete advanced CPU snapshot produced by AdvancedCpuMonitor::read().
struct AdvancedCpuSnapshot {
  bool proc_stat_readable = false;
  CpuStatistics aggregate;        // cpu_id == -1
  std::vector<CpuStatistics> cpus;  // per-logical-CPU, sorted by CPU ID
  CpuTopologyInfo topology;
};

/**
 * Monitors total and per-logical-CPU utilisation, topology and frequency from
 * /proc/stat and /sys/devices/system/cpu.
 *
 * Every read re-parses /proc/stat once and the small topology masks; delta
 * based utilisation is derived by comparing the previous snapshot. CPU identity
 * is tracked by logical CPU ID — never by position in a vector — so CPUs that
 * are hotplugged, taken offline or renumbered keep a stable identity. A CPU
 * that was offline (or just appeared) is given a fresh baseline before any
 * utilisation is reported for it.
 *
 * Optional cpufreq files are read per online CPU and may simply not exist;
 * none of the failures stop the monitor. The refresh cadence is owned by the
 * application's main loop — this class never starts its own thread.
 *
 * The optional `root` parameter points the monitor at an alternate filesystem
 * root (used by tests to feed synthetic /proc/stat and sysfs trees).
 */
class AdvancedCpuMonitor {
 public:
  explicit AdvancedCpuMonitor(std::filesystem::path root = "/");
  ~AdvancedCpuMonitor() = default;

  // Monitors hold per-CPU diffing state; copying/moving would duplicate
  // baselines.
  AdvancedCpuMonitor(const AdvancedCpuMonitor &) = delete;
  AdvancedCpuMonitor &operator=(const AdvancedCpuMonitor &) = delete;

  /// Takes a fresh sample and returns the computed snapshot. The first call
  /// only records baselines, so `has_sample` is false for every CPU until a
  /// second sample proves the counters advanced.
  [[nodiscard]] AdvancedCpuSnapshot read();

  /// The most recent topology read by read(). Stable across refreshes when the
  /// kernel does not change the CPU set.
  const CpuTopologyInfo &topology() const { return topology_; }

 private:
  std::filesystem::path root_;
  std::map<int, CpuCounterSnapshot> previous_;  // -1 = aggregate
  std::set<int> known_cpus_;                    // every CPU ID ever observed
  CpuTopologyInfo topology_;
};

}  // namespace atm