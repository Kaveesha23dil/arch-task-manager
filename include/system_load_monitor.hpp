#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace atm {

/// Heuristic system-load level derived from the load average normalized per
/// online logical CPU. This is only an indicator of system load; it is
/// explicitly not a medical or absolute system-health diagnosis.
enum class LoadSeverity {
  Normal,    // < 0.70 per CPU
  Elevated,  // 0.70 .. < 1.00 per CPU
  High,      // 1.00 .. < 2.00 per CPU
  Critical,  // >= 2.00 per CPU
};

/// Display name for a load severity ("NORMAL", "ELEVATED", "HIGH", "CRITICAL").
[[nodiscard]] const char *loadSeverityName(LoadSeverity severity);

/// Severity thresholds for `classifyLoadSeverity()`, expressed as the load
/// average normalized per online logical CPU. Centralised here so the
/// heuristic has a single definition. The bands follow the common convention:
/// below 0.70 is normal, 0.70–<1.00 elevated, 1.00–<2.00 high, and at/above
/// 2.00 critical.
inline constexpr double kLoadSeverityElevatedAt = 0.70;
inline constexpr double kLoadSeverityHighAt = 1.00;
inline constexpr double kLoadSeverityCriticalAt = 2.00;

/// Classifies a per-CPU normalized load average into a heuristic level. NaN
/// and infinities are not expected here (the parser rejects them); unknown
/// values should be presented as unavailable instead of being classified.
[[nodiscard]] LoadSeverity classifyLoadSeverity(double normalized_load);

/// Normalizes a raw load average across the number of online logical CPUs
/// (`normalized = load / online_cpus`). Returns std::nullopt when
/// `online_cpus == 0` so callers can keep the load unnormalized instead of
/// dividing by zero.
[[nodiscard]] std::optional<double> normalizeLoad(double load,
                                                  std::uint32_t online_cpus);

/// Load averages, running/total task counts and last PID from /proc/loadavg.
struct LoadAverageSnapshot {
  /// True once the 1/5/15-minute load average triple was parsed.
  bool readable = false;
  bool running_total_available = false;
  bool last_pid_available = false;
  double load1 = 0.0;
  double load5 = 0.0;
  double load15 = 0.0;
  /// Currently running / total tasks. These are integer counts in the file;
  /// stored as doubles to share the strict parser.
  double running = 0.0;
  double total = 0.0;
  std::uint64_t last_pid = 0;
};

/**
 * Parses one /proc/loadavg line, e.g.
 * "3.36 2.98 2.51 7/1023 19041".
 *
 * The first three fields (the load averages) are required: a line without
 * them returns false. The optional running/total field and the optional last
 * PID field may be absent or malformed; a failure of one leaves only its own
 * availability flag false while the rest of the line survives. Negative
 * values, NaN and infinity are rejected.
 */
[[nodiscard]] bool parseLoadAverage(const std::string &line,
                                    LoadAverageSnapshot &out);

/// Uptime and idle seconds from /proc/uptime.
struct UptimeSnapshot {
  bool readable = false;
  /// Seconds since boot (fractional). Convert to whole seconds for display.
  double uptime_seconds = 0.0;
  /// Sum of CPUs' idle ticks divided by USER_HZ; on SMP this may exceed the
  /// uptime and is therefore not a wall-clock duration.
  double idle_seconds = 0.0;
};

/**
 * Parses one /proc/uptime line, e.g. "1625962.72 4750871.82".
 *
 * The uptime field is required; the idle field is optional for forward
 * compatibility, so a missing/malformed idle only leaves `idle_seconds` at 0.
 * Returns false when the uptime itself cannot be parsed. Negative values, NaN
 * and infinity are rejected.
 */
[[nodiscard]] bool parseUptime(const std::string &line, UptimeSnapshot &out);

/**
 * Point-in-time system load and uptime read from /proc/loadavg and
 * /proc/uptime. Every field mirrors the respective file; a missing or broken
 * file leaves that half unavailable. Derived helpers never invent values the
 * kernel did not report.
 */
struct SystemLoadSnapshot {
  LoadAverageSnapshot load;
  UptimeSnapshot uptime;
  /// Wall-clock boot estimate derived as `current time - uptime` when the
  /// uptime is readable. It is an estimate and may be off when the wall clock
  /// was adjusted; it is never presented as a guaranteed boot timestamp.
  std::optional<std::chrono::system_clock::time_point> boot_time;
};

/**
 * Monitors system load and uptime by reading /proc/loadavg and /proc/uptime
 * exactly once per read().
 *
 * The monitor is read-only and never starts its own thread; the refresh
 * cadence is owned by the application's main loop. `root` points at an
 * alternate filesystem root (used by tests).
 */
class SystemLoadMonitor {
 public:
  explicit SystemLoadMonitor(std::filesystem::path root = "/");
  ~SystemLoadMonitor() = default;

  SystemLoadMonitor(const SystemLoadMonitor &) = delete;
  SystemLoadMonitor &operator=(const SystemLoadMonitor &) = delete;

  /// Reads both files once and returns the snapshot. A missing/broken file
  /// never fails the call — the corresponding half just stays unavailable.
  [[nodiscard]] SystemLoadSnapshot read();

 private:
  std::filesystem::path root_;
};

}  // namespace atm