#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace atm {

/// A configured pressure category backed by one /proc/pressure file.
enum class PressureCategory {
  Cpu,
  Memory,
  Io,
};

/// Display name for a pressure category ("CPU", "Memory", "I/O").
[[nodiscard]] const char *pressureCategoryName(PressureCategory category);

/// Heuristic resource-contention severity. This is only an indicator of
/// resource pressure derived from the 10-second averages — it is explicitly
/// not a medical or absolute system-health diagnosis.
enum class PressureSeverity {
  Normal,
  Elevated,
  High,
  Critical,
};

/// Display name for a severity ("NORMAL", "ELEVATED", "HIGH", "CRITICAL").
[[nodiscard]] const char *pressureSeverityName(PressureSeverity severity);

/// Severity thresholds for `classifyPressureSeverity()`, expressed as
/// percentages of pressure within a rolling window (typically the 10-second
/// `some` average). Centralised here so the heuristic has a single definition.
/// The bands follow widely used PSI guidance: below 10% is normal, 10–30% is
/// elevated, 30–50% is high, and at/above 50% is critical.
inline constexpr double kPressureSeverityElevatedAt = 10.0;
inline constexpr double kPressureSeverityHighAt = 30.0;
inline constexpr double kPressureSeverityCriticalAt = 50.0;

/// Classifies a percentage pressure value into a heuristic contention level.
/// NaN and infinities are not expected here (the parser rejects them); unknown
/// values should be presented as unavailable instead of being classified.
[[nodiscard]] PressureSeverity classifyPressureSeverity(double pressure_percent);

/// How reading one /proc/pressure/<resource> file went. Used to tell "PSI is
/// simply not exposed by this kernel" apart from "the file exists but could
/// not be opened" and "the file yielded no recognizable pressure lines".
enum class PressureReadStatus {
  /// File missing — the kernel exposes no PSI here (CONFIG_PSI off, or the
  /// pressure files are not mounted/permitted by the environment).
  Unsupported,
  /// The file exists but could not be opened (permission denied, I/O error).
  Unreadable,
  /// The file opened but contained no recognized some/full pressure lines.
  Empty,
  /// The file opened and at least one some/full line was parsed.
  Read,
};

/// Display name for a read status ("unsupported", "unreadable", "empty",
/// "read"). "unsupported"/"unreadable" mark the category as unavailable.
[[nodiscard]] const char *pressureReadStatusName(PressureReadStatus status);

/**
 * One PSI metric: the `some` or `full` pressure set of a category file.
 *
 * Averages are percentages over rolling 10/60/300-second windows and are
 * stored exactly as reported (2.50 means 2.50%, never 0.025). `total` is the
 * cumulative stalled time in MICROSECONDS, and is preserved raw. Missing
 * fields (a key that never appeared, or one whose value was malformed) are
 * represented as empty optionals — never silently zeroed. A present zero is
 * distinguishable from a missing value because the optional then holds 0.
 */
struct PressureMetric {
  /// True when the some/full line itself was present in the file (even if all
  /// of its fields were missing or malformed).
  bool line_present = false;
  std::optional<double> avg10;
  std::optional<double> avg60;
  std::optional<double> avg300;
  /// Cumulative stalled microseconds (64-bit; NOT wall-clock seconds).
  std::optional<std::uint64_t> total;
  /// Recent stall rate in microseconds stalled per wall-clock second, derived
  /// from the delta of `total` between two reads. Nullopt on the first read,
  /// when the counter decreased/reset (baseline re-anchored), or when the
  /// elapsed wall time could not be measured.
  std::optional<double> rate_usec_per_second;
};

/// Everything known about one pressure category in a snapshot.
struct PressureCategoryData {
  PressureReadStatus status = PressureReadStatus::Unsupported;
  PressureMetric some;
  PressureMetric full;
};

/**
 * Point-in-time system pressure read from /proc/pressure/{cpu,memory,io}.
 *
 * Every field mirrors the respective file; a category whose file is missing
 * stays entirely unavailable. Derived helpers never invent a full pressure
 * value when the kernel did not report one.
 */
struct SystemPressureSnapshot {
  PressureCategoryData cpu;
  PressureCategoryData memory;
  PressureCategoryData io;

  /// Returns the data for a category.
  [[nodiscard]] const PressureCategoryData &category(PressureCategory c) const;

  /// True when at least one category provided a recognizable pressure line.
  [[nodiscard]] bool anyAvailable() const;

  /// True when the category's some/full line was present in the file.
  [[nodiscard]] bool hasLine(PressureCategory c, bool full) const;

  /// The most representative 10-second average: the some pressure when the
  /// kernel reported it, otherwise the full pressure. Nullopt when the
  /// category has neither.
  [[nodiscard]] std::optional<double> headlineAvg10(PressureCategory c) const;

  /// Heuristic severity of a category from its headline 10-second average.
  /// Nullopt when no avg10 is available. Never a diagnosis.
  [[nodiscard]] std::optional<PressureSeverity> severity(PressureCategory c) const;
};

/**
 * Parses one PSI line ("some avg10=0.00 avg60=0.00 avg300=0.00 total=123").
 *
 * Returns true and sets `out` when the line began with a recognized `some` or
 * `full` token (`is_full` is set accordingly). Empty lines and unknown line
 * types (future types, headers) return false without touching `out`. Field
 * keys may appear in any order; unknown keys are ignored for forward
 * compatibility; duplicate keys keep the last occurrence; a malformed value
 * leaves that single field unavailable while the rest of the line survives.
 * Negative values, NaN and infinity are rejected.
 */
[[nodiscard]] bool parsePressureLine(const std::string &line, bool &is_full,
                                     PressureMetric &out);

/**
 * Parses a whole /proc/pressure/<resource> stream into its some/full metrics.
 *
 * `some`/`full` may appear in either order or be absent. Returns the number of
 * recognized lines (0 for an empty or entirely-unrecognized file). The two
 * metrics are reset first so repeated parses never leak fields across files.
 */
[[nodiscard]] std::size_t parsePressureFile(std::istream &in,
                                            PressureMetric &some,
                                            PressureMetric &full);

/**
 * Monitors system pressure by reading /proc/pressure/{cpu,memory,io} exactly
 * once per read().
 *
 * The monitor is read-only and never starts its own thread; the refresh cadence
 * is owned by the application's main loop. It holds only two items of state:
 * the previous read's cumulative `total` per metric (to compute a stalled-time
 * rate) and the previous read time. On a counter decrease/reset the baseline is
 * re-anchored and no negative delta is ever produced. `root` points at an
 * alternate filesystem root (used by tests).
 */
class SystemPressureMonitor {
 public:
  explicit SystemPressureMonitor(std::filesystem::path root = "/");
  ~SystemPressureMonitor() = default;

  SystemPressureMonitor(const SystemPressureMonitor &) = delete;
  SystemPressureMonitor &operator=(const SystemPressureMonitor &) = delete;

  /// Reads all three pressure files once and returns the snapshot. PSI being
  /// unavailable never fails the call — the snapshot just stays empty.
  [[nodiscard]] SystemPressureSnapshot read();

 private:
  std::filesystem::path root_;
  /// Previous cumulative totals keyed by (category, is_full).
  std::map<std::pair<PressureCategory, bool>, std::uint64_t> previous_total_;
  std::optional<std::chrono::steady_clock::time_point> previous_read_;
};

}  // namespace atm