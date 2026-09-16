#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

#include "network_interface_details.hpp"
#include "resource_history.hpp"

namespace atm {

/// One native cumulative counter exposed by the kernel in the sysfs directory
/// `/sys/class/net/<iface>/statistics/`. The kernel reads these from the
/// driver's `struct net_device_stats` (using `dev_get_stats()`); a driver that
/// does not implement a counter leaves its file absent or zero, so availability
/// is per-metric and never assumed. Names are stable, lowercase, and identical
/// to the sysfs file names — reused verbatim as the CSV/JSON field names.
enum class NetworkLinkStatMetric {
  RxErrors,
  TxErrors,
  RxDropped,
  TxDropped,
  RxMissedErrors,
  TxFifoErrors,
  RxFifoErrors,
  Collisions,
  RxOverErrors,
  RxFrameErrors,
  RxLengthErrors,
  RxCrcErrors,
  RxCompressed,
  TxCompressed,
  TxCarrierErrors,
  TxHeartbeatErrors,
  TxWindowErrors,
  Multicast,
  RxNohandler,
};

/// Number of counters (== the sysfs `statistics/` files this step tracks).
inline constexpr std::size_t kNetworkLinkStatMetricCount = 19;

/// Stable, lowercase metric name, identical to the sysfs statistics file and to
/// the exported CSV/JSON field name (e.g. "rx_errors", "collisions"). The
/// lookup never fails: out-of-range values return "unknown".
[[nodiscard]] const char *networkLinkStatMetricName(NetworkLinkStatMetric metric);

/// Freshness of the per-tick counter read for one interface. Mirrors the other
/// network freshness models: a temporary read failure preserves the last valid
/// values and reads `Stale`; a clean absence (the driver exposes no statistics
/// at all) reads `Unavailable` — never a fabricated zero.
enum class NetworkLinkStatState {
  Unknown,     // no read has been absorbed for this identity yet
  Valid,       // counters freshly read this tick
  Stale,       // temporary failure — last valid counters preserved
  Unavailable, // clean absence: no statistics exposed by the driver/link
};

[[nodiscard]] const char *networkLinkStatStateName(NetworkLinkStatState state);

/// Derived interval rates for one refresh window: `counter delta / elapsed
/// steady-clock seconds` (events per second). Every rate is std::nullopt when it
/// cannot be computed safely — no previous valid baseline yet, the counter was
/// missing at either endpoint, the counter decreased (reset/wraparound), or the
/// elapsed time is not strictly positive — never a fabricated zero and never a
/// reset spike. Combined rates need both of their component metrics.
struct NetworkLinkStatRates {
  std::optional<double> rx_errors_per_second;
  std::optional<double> tx_errors_per_second;
  std::optional<double> rx_dropped_per_second;
  std::optional<double> tx_dropped_per_second;
  std::optional<double> combined_errors_per_second;  // rx + tx errors
  std::optional<double> combined_drops_per_second;   // rx + tx drops
  std::optional<double> collisions_per_second;
  std::optional<double> rx_missed_errors_per_second;
  std::optional<double> rx_fifo_errors_per_second;
  std::optional<double> tx_fifo_errors_per_second;
  std::optional<double> rx_crc_errors_per_second;
  std::optional<double> rx_frame_errors_per_second;
  std::optional<double> tx_carrier_errors_per_second;
};

/// One retained per-interface error-statistics sample (Step 53). Self-contained
/// (timestamps + cumulative counters + the derived window rates) so summaries,
/// charts and exports are pure functions of the retained ring. `valid == false`
/// marks a tick whose counters could not be freshly read; such a tick carries
/// the last valid counters unchanged (for honest, gap-free display) but
/// contributes no fresh measurement and no rate. `discontinuity_count` is the
/// number of metrics that decreased between the previous and this valid sample
/// (driver reset / counter wraparound) — it is 0 for a normal tick.
struct NetworkLinkStatSample {
  std::chrono::steady_clock::time_point timestamp{};  // ordering / rate math
  std::chrono::system_clock::time_point wall_clock{};  // "last update" display
  bool valid = false;            // counters freshly read this tick
  bool any_available = false;    // at least one counter file was present
  std::optional<std::uint64_t> counters[kNetworkLinkStatMetricCount]{};
  NetworkLinkStatRates rates;
  std::size_t discontinuity_count = 0;
};

/// One raw probe of one interface's `statistics/` sysfs directory. Every
/// counter is independently optional: an absent/unreadable/malformed file is
/// std::nullopt for that metric only. Pure data — never throws, a failed probe
/// of one interface never stops the others.
struct NetworkLinkStatRead {
  bool read_ok = true;         // the statistics directory was opened and listed
  bool access_denied = false;  // the directory existed but was not readable
  bool any_available = false;  // at least one counter file carried a value
  std::optional<std::uint64_t> counters[kNetworkLinkStatMetricCount]{};
};

/// Reads `/sys/class/net/<iface>/statistics/` under `<root>` using native
/// filesystem APIs (no subprocesses, no shell). `root` defaults to "/" (the
/// real sysfs tree) but may be an alternate root for hermetic tests. Missing
/// files are expected (drivers differ) and never logged; malformed values are
/// treated as unavailable. Returns a `NetworkLinkStatRead`; never throws.
[[nodiscard]] NetworkLinkStatRead readNetworkLinkStats(
    const std::filesystem::path &root, const std::string &iface);

/// Derives the per-metric interval rates for one window from two consecutive
/// valid samples and the real elapsed seconds. Each metric is independent: a
/// counter present at both endpoints and never decreased yields its rate; a
/// missing counter at either endpoint, a decrease (reset) or an invalid window
/// leaves that metric unavailable. Combined rates are unavailable unless both
/// components are. Wraparound-safe (unsigned subtraction before promotion) and
/// NaN/infinity/division-by-zero free.
[[nodiscard]] NetworkLinkStatRates deriveNetworkLinkStatRates(
    const NetworkLinkStatSample &previous, const NetworkLinkStatSample &current,
    double seconds);

/// One interface's tracked error/drop/collision statistics. Mirrors the other
/// per-interface monitor records: keyed by the stable identity so a renamed
/// interface keeps its series while a recreated one (new ifindex) starts fresh.
struct NetworkLinkStats {
  std::string identity;          // stable key ("idx:<ifindex>" / "name:<name>")
  std::string name;              // current kernel name
  bool present = true;           // false once the identity has disappeared
  NetworkLinkStatState state = NetworkLinkStatState::Unknown;

  /// Total resets/discontinuities observed so far (metrics that decreased
  /// between consecutive valid samples — driver resets, counter wraparound).
  std::size_t discontinuity_count = 0;

  /// Bounded per-tick sample ring (one sample per refresh; never on the render
  /// path). Invalid/stale ticks are kept as honest gaps.
  ResourceHistory<NetworkLinkStatSample> history{0};

  std::chrono::system_clock::time_point last_update{};  // newest sample wall time
  std::chrono::steady_clock::time_point first_seen{};
};

/// Pure per-sample recorder: merges one fresh read into the previously retained
/// record — appends the (preserved-on-stale) sample, derives this window's
/// rates against the last valid sample and counts discontinuities. Never
/// performs I/O; the caller owns the sysfs reads. `now` is the steady tick
/// timestamp, `wall_clock` the display timestamp, `max_samples` the ring bound.
[[nodiscard]] NetworkLinkStats updateNetworkLinkStats(
    const NetworkLinkStats &previous, const NetworkLinkStatRead &read,
    const std::chrono::steady_clock::time_point &now,
    const std::chrono::system_clock::time_point &wall_clock,
    std::size_t max_samples);

/// Derived display summary of one interface's tracked statistics. Pure and
/// deterministic: a bounded pass over the retained ring; the current counters
/// and rates come from the newest sample, window deltas from first/last valid
/// samples. Unavailable metrics stay nullopt — never a fabricated zero.
struct NetworkLinkStatSummary {
  bool has_data = false;         // at least one retained sample
  bool has_valid_data = false;   // at least one freshly-read retained sample
  std::size_t sample_count = 0;
  std::size_t valid_sample_count = 0;
  std::size_t stale_sample_count = 0;      // retained ticks read as stale
  std::size_t discontinuity_count = 0;     // resets across the whole record
  double coverage = 0.0;                   // sample_count / retention bound
  double span_seconds = 0.0;               // newest - oldest retained span

  std::optional<std::uint64_t> current[kNetworkLinkStatMetricCount]{};
  NetworkLinkStatRates rates;
  std::optional<std::uint64_t> window_delta[kNetworkLinkStatMetricCount]{};

  std::chrono::system_clock::time_point last_update{};
};

/// Computes the Step 53 summary for `stats`. Pure and deterministic: reads only
/// the retained ring, never the live kernel. `max_samples` is the same
/// retention bound the monitor uses, so coverage is consistent with the rest of
/// the interface details.
[[nodiscard]] NetworkLinkStatSummary summarizeNetworkLinkStats(
    const NetworkLinkStats &stats, std::size_t max_samples);

/// Formats an events-per-second rate ("12.3/s", "0.05/s") or "unavailable"
/// when the rate is nullopt. Locale-independent, never negative.
[[nodiscard]] std::string formatNetworkLinkStatRate(
    const std::optional<double> &rate);

/// Renders the "Errors & drops" block used by the interface-details view:
/// cumulative counters, current interval rates and retained-window deltas with
/// explicit "unavailable" for missing metrics, plus the freshness state, the
/// last update time and the retained coverage. Pure ASCII, two-space indented
/// to match the surrounding detail page. Pure and unit-testable.
[[nodiscard]] std::string renderNetworkLinkStats(
    const NetworkLinkStats &stats, std::size_t max_samples);

/// Chart geometry for the compact error-rate trend graph.
struct NetworkLinkStatChartConfig {
  std::size_t data_width = 40;   // plot columns
  std::size_t data_height = 5;   // plot rows (y-axis scale labels included)
};

/// Renders a compact monochrome-safe ASCII trend of one metric's derived rate
/// over the retained ring, using the same glyph conventions as the other
/// history charts ('~' peak / '.' flow; blank columns for unavailable ticks).
/// Missing/gap ticks render as honest blanks — never a fabricated reading. When
/// the metric has no rate at all a short "(no data)" marker is returned.
/// Deterministic and bounded (O(cols * samples) over the retained ring only).
[[nodiscard]] std::string renderNetworkLinkStatTrend(
    const NetworkLinkStats &stats, NetworkLinkStatMetric metric,
    const NetworkLinkStatChartConfig &config);

/// Monitors per-interface network error and drop statistics (Step 53).
///
/// Fed from the existing NetworkInterfaceMonitor snapshot exactly once per tick
/// by the application's monitoring loop (no second polling loop, nothing read
/// on the UI-render path). For each present interface it reads the kernel's
/// `/sys/class/net/<iface>/statistics/` counters with native filesystem APIs
/// (no subprocesses), keeps a bounded per-tick sample ring under the existing
/// history-retention setting, pushes a bounded set of gone identities so a
/// temporary removal is not misreported, and never logs a repeatedly missing
/// optional counter (driver coverage varies). A recreated interface (new
/// ifindex) naturally starts a fresh record; a counter that decreases is
/// handled per metric (rate unavailable for that window, one discontinuity
/// counted) instead of poisoning the rest. History pause skips the reads and
/// samples entirely.
class NetworkLinkStatsMonitor {
 public:
  static constexpr std::size_t kMaxTrackedLinkStatsInterfaces = 64;

  explicit NetworkLinkStatsMonitor(
      std::filesystem::path root = "/",
      std::size_t history_max_samples = kDefaultInterfaceHistorySamples);

  NetworkLinkStatsMonitor(const NetworkLinkStatsMonitor &) = delete;
  NetworkLinkStatsMonitor &operator=(const NetworkLinkStatsMonitor &) = delete;
  ~NetworkLinkStatsMonitor() = default;

  /// Processes one discovery snapshot (produced by NetworkInterfaceMonitor).
  void update(const NetworkInterfaceSnapshot &snapshot);

  /// Latest statistics for an identity; nullptr when never seen.
  [[nodiscard]] const NetworkLinkStats *tracked(
      const std::string &identity) const;

  /// All retained statistics records (present and gone identities).
  [[nodiscard]] const std::unordered_map<std::string, NetworkLinkStats> &
  entries() const {
    return tracked_;
  }

  void setHistoryMaxSamples(std::size_t max_samples);
  [[nodiscard]] std::size_t historyMaxSamples() const { return history_max_samples_; }

  void setHistoryPaused(bool paused) { history_paused_ = paused; }
  [[nodiscard]] bool historyPaused() const { return history_paused_; }

  void reset();

 private:
  std::filesystem::path root_;
  std::size_t history_max_samples_;
  bool history_paused_ = false;
  std::unordered_map<std::string, NetworkLinkStats> tracked_;
  void evictOverflow();
};

}  // namespace atm