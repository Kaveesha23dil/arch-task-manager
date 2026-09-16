#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "network_interface_details.hpp"
#include "resource_history.hpp"

namespace atm {

/// A configured MTU is a link property, not a capability claim. The kernel
/// exposes only the configured value (`/sys/class/net/<iface>/mtu`), never a
/// verified jumbo-frame-capability bit, so a large value is described as a
/// configured MTU above the standard Ethernet size and never as "supports
/// jumbo frames". This is a sizing label only.
enum class NetworkPacketMtuSizeClass {
  Unknown,        // no retained MTU value to classify
  Standard,       // == the standard Ethernet MTU
  BelowStandard,  // < the standard Ethernet MTU
  AboveStandard,  // > the standard Ethernet MTU (jumbo-sized *configuration*)
};

/// Short, stable size-class token (export-friendly): "unknown", "standard",
/// "below_standard", "above_standard".
[[nodiscard]] const char *networkPacketMtuSizeClassName(
    std::optional<int> mtu);

/// Standard Ethernet MTU used as the neutral size-class reference. Purely a
/// label: an interface with a larger configured MTU is not asserted to support
/// jumbo frames end-to-end.
inline constexpr int kStandardEthernetMtu = 1500;

/// Cap on the retained MTU-change timeline (most-recent events; the running
/// change counter itself is unbounded).
inline constexpr std::size_t kNetworkPacketMtuMaxEvents = 32;

/// Freshness of the per-tick MTU/counter read for one interface, mirroring the
/// other network freshness models. A temporary read failure preserves the last
/// valid values and reads `Stale`; a clean absence (the mtu file is not
/// exposed) reads `Unavailable` — never a fabricated zero MTU.
enum class NetworkPacketMtuState {
  Unknown,     // no read has been absorbed for this identity yet
  Valid,       // mtus/counters freshly read this tick
  Stale,       // temporary failure — last valid values preserved
  Unavailable, // clean absence: no mtu sysfs file exposed by this interface
};

[[nodiscard]] const char *networkPacketMtuStateName(NetworkPacketMtuState state);

/// Derived interval rates (packets/bytes per second) for one refresh window:
/// `counter delta / elapsed steady-clock seconds`. Every rate is std::nullopt
/// when it cannot be computed safely — no previous valid baseline yet, the
/// counter was missing at either endpoint, the counter decreased
/// (reset/wraparound), or the elapsed time is not strictly positive — never a
/// fabricated zero and never a reset spike. Combined rates need both of their
/// component directions.
struct NetworkPacketMtuRates {
  std::optional<double> rx_packets_per_second;
  std::optional<double> tx_packets_per_second;
  std::optional<double> combined_packets_per_second;  // rx + tx
  std::optional<double> rx_bytes_per_second;
  std::optional<double> tx_bytes_per_second;
  std::optional<double> combined_bytes_per_second;    // rx + tx
};

/// Estimated average frame size (bytes per frame, headers included) derived
/// from the raw `/proc/net/dev` counters: `bytes delta / packets delta`. This
/// is an *estimate*, not a packet-size distribution: the kernel counters expose
/// only aggregate byte and packet totals, so a skewed mix of tiny and large
/// packets shows as a single midpoint and no jumbo/oversized-relative-to-MTU
/// classification is attempted. Every direction is independently std::nullopt
/// when it cannot be computed (no baseline, missing counter at either endpoint,
/// decreased counter, non-positive packet delta).
struct NetworkPacketSizeEstimate {
  std::optional<double> rx_bytes_per_frame;
  std::optional<double> tx_bytes_per_frame;
  std::optional<double> combined_bytes_per_frame;  // rx + tx frames
};

/// One retained per-interface MTU/packet-size sample (Step 54). Self-contained
/// (timestamps + cumulative counters + the derived window rates and size
/// estimates) so summaries, charts and exports are pure functions of the
/// retained ring. `valid == false` marks a tick that produced no fresh
/// measurement (both the mtu read and the counters were unavailable); such a
/// tick carries the last valid MTU and counter values unchanged (for honest,
/// gap-free display) but contributes no fresh measurement and no rate. The MTU
/// and the counters have independent freshness: `mtu_available` /
/// `counters_available` record which part was freshly read this tick.
struct NetworkPacketMtuSample {
  std::chrono::steady_clock::time_point timestamp{};  // ordering / rate math
  std::chrono::system_clock::time_point wall_clock{};  // "last update" display
  bool valid = false;             // any fresh measurement this tick
  bool mtu_available = false;     // the mtu file was freshly read and parsed
  bool counters_available = false;  // a /proc/net/dev counter was freshly seen
  std::optional<int> mtu;          // fresh or preserved last valid value
  std::optional<int> previous_mtu; // the previous retained value, on change ticks
  bool mtu_changed = false;        // true only on the tick the value actually changed

  std::optional<std::uint64_t> rx_packets;
  std::optional<std::uint64_t> tx_packets;
  std::optional<std::uint64_t> rx_bytes;
  std::optional<std::uint64_t> tx_bytes;

  NetworkPacketMtuRates rates;
  NetworkPacketSizeEstimate estimate;
  std::size_t counter_discontinuity_count = 0;  // counters that decreased
};

/// One MTU change observed while tracking an interface. Only actual value
/// changes produce an event (old != new); a temporary read failure or a
/// preserved value never does, so the list stays free of duplicates.
struct NetworkPacketMtuChangeEvent {
  std::chrono::system_clock::time_point wall_clock{};
  std::optional<int> previous_mtu;
  std::optional<int> new_mtu;
};

/// One raw probe of one interface's MTU sysfs file plus the same-tick traffic
/// counters. MTU comes from `/sys/class/net/<iface>/mtu`; the counters come
/// from the existing NetworkInterfaceMonitor snapshot (the same `/proc/net/dev`
/// totals that drive the throughput graphs) so the packet-size estimates always
/// agree with the byte-rate graphs. Each counter is independently optional: an
/// interface absent from `/proc/net/dev` is unavailable, never zero.
struct NetworkPacketMtuRead {
  bool mtu_read_ok = true;          // the mtu file was opened
  bool mtu_access_denied = false;   // the file existed but was not readable
  bool counters_available = false;  // at least one counter value was present
  std::optional<int> mtu;
  std::optional<std::uint64_t> rx_packets;
  std::optional<std::uint64_t> tx_packets;
  std::optional<std::uint64_t> rx_bytes;
  std::optional<std::uint64_t> tx_bytes;
};

/// Reads `/sys/class/net/<iface>/mtu` under `<root>` using native filesystem
/// APIs (no subprocesses, no shell). `root` defaults to "/" (the real sysfs
/// tree) but may be an alternate root for hermetic tests. Output fields
/// counters_available/... are left untouched; only the MTU part is populated so
/// the monitor can merge in the snapshot's traffic counters afterwards. A
/// missing file, a permission failure, or a syntactically invalid value are all
/// handled without throwing and without inventing a value.
[[nodiscard]] NetworkPacketMtuRead readNetworkInterfaceMtu(
    const std::filesystem::path &root, const std::string &iface);

/// Derives the per-direction interval rates for one window from two consecutive
/// fresh-counter samples and the real elapsed seconds. Each direction is
/// independent: a counter present at both endpoints and never decreased yields
/// its rate; anything else leaves that metric unavailable. Combined rates are
/// unavailable unless both directions are. The size estimate requires the
/// matching packet delta to be strictly positive (bytes with no frames are not
/// meaningful). Wraparound-safe (unsigned subtraction before promotion) and
/// NaN/infinity/division-by-zero free.
[[nodiscard]] NetworkPacketMtuRates deriveNetworkPacketMtuRates(
    const NetworkPacketMtuSample &previous, const NetworkPacketMtuSample &current,
    double seconds);

/// Derives the estimated average frame sizes for one window from the same two
/// samples (see NetworkPacketSizeEstimate for the estimate caveat).
[[nodiscard]] NetworkPacketSizeEstimate deriveNetworkPacketSizeEstimate(
    const NetworkPacketMtuSample &previous, const NetworkPacketMtuSample &current);

/// One interface's tracked MTU and packet/counter statistics. Mirrors the other
/// per-interface monitor records: keyed by the stable identity so a renamed
/// interface keeps its series while a recreated one (new ifindex) starts fresh.
struct NetworkPacketMtuStats {
  std::string identity;            // stable key ("idx:<ifindex>" / "name:<name>")
  std::string name;                // current kernel name
  bool present = true;             // false once the identity has disappeared
  NetworkPacketMtuState state = NetworkPacketMtuState::Unknown;

  /// MTU changes observed so far (only actual value changes are counted).
  std::size_t mtu_change_count = 0;
  /// Counter resets/discontinuities observed so far (counters that decreased
  /// between consecutive fresh samples — driver/interface resets, wraparound).
  std::size_t discontinuity_count = 0;

  /// Bounded per-tick sample ring (one sample per refresh; never on the render
  /// path). Invalid/stale ticks are kept as honest gaps.
  ResourceHistory<NetworkPacketMtuSample> history{0};

  /// Bounded list of MTU change events (newest last), for the change timeline.
  std::vector<NetworkPacketMtuChangeEvent> mtu_events;

  std::chrono::system_clock::time_point last_update{};  // newest sample wall time
  std::chrono::steady_clock::time_point first_seen{};
};

/// Pure per-sample recorder: merges one fresh probe into the previously
/// retained record — appends the (preserved-on-stale) sample, derives this
/// window's rates and size estimate against the last fresh baseline, appends an
/// MTU-change event when the value actually changed, and counts discontinuities.
/// Never performs I/O; the caller owns the sysfs reads. `now` is the steady
/// tick timestamp, `wall_clock` the display timestamp, `max_samples` the ring
/// bound.
[[nodiscard]] NetworkPacketMtuStats updateNetworkPacketMtu(
    const NetworkPacketMtuStats &previous, const NetworkPacketMtuRead &read,
    const std::chrono::steady_clock::time_point &now,
    const std::chrono::system_clock::time_point &wall_clock,
    std::size_t max_samples);

/// Derived display summary of one interface's tracked MTU/packet statistics.
/// Pure and deterministic: a bounded pass over the retained ring; the current
/// MTU/counters come from the newest sample, window deltas from the first/last
/// fresh-counter samples. Unavailable values stay nullopt — never a fabricated
/// zero.
struct NetworkPacketMtuSummary {
  bool has_data = false;          // at least one retained sample
  bool has_valid_data = false;    // at least one sample with a fresh reading
  bool has_mtu_data = false;      // at least one freshly-read MTU value
  bool has_counter_data = false;  // at least one freshly-read counter value
  std::size_t sample_count = 0;
  std::size_t valid_sample_count = 0;
  std::size_t stale_sample_count = 0;      // retained ticks read as stale
  std::size_t mtu_valid_count = 0;         // ticks with a freshly-read MTU
  std::size_t counter_valid_count = 0;     // ticks with fresh counters
  std::size_t mtu_change_count = 0;        // changes across the whole record
  std::size_t discontinuity_count = 0;     // counter resets across the record
  double coverage = 0.0;                   // sample_count / retention bound
  double span_seconds = 0.0;               // newest - oldest retained span

  std::optional<int> current_mtu;
  std::optional<int> previous_mtu;
  std::optional<int> min_mtu;              // over retained window MTU values
  std::optional<int> max_mtu;

  std::optional<std::uint64_t> current_rx_packets;
  std::optional<std::uint64_t> current_tx_packets;
  std::optional<std::uint64_t> current_rx_bytes;
  std::optional<std::uint64_t> current_tx_bytes;
  std::optional<std::uint64_t> window_rx_packets;  // first vs last fresh sample
  std::optional<std::uint64_t> window_tx_packets;
  std::optional<std::uint64_t> window_rx_bytes;
  std::optional<std::uint64_t> window_tx_bytes;

  NetworkPacketMtuRates rates;
  NetworkPacketSizeEstimate estimate;          // newest window
  NetworkPacketSizeEstimate window_estimate;   // whole retained window

  std::chrono::system_clock::time_point last_update{};
};

/// Computes the Step 54 summary for `stats`. Pure and deterministic: reads only
/// the retained ring, never the live kernel. `max_samples` is the same
/// retention bound the monitor uses, so coverage is consistent with the rest of
/// the interface details.
[[nodiscard]] NetworkPacketMtuSummary summarizeNetworkPacketMtu(
    const NetworkPacketMtuStats &stats, std::size_t max_samples);

/// Formats a packets-per-second rate ("12.3/s", "0.05/s") or "unavailable".
/// Locale-independent, never negative.
[[nodiscard]] std::string formatNetworkPacketMtuRate(
    const std::optional<double> &rate);

/// Formats a byte throughput in human-friendly scaled units ("1.2 MB/s",
/// "850 KB/s") or "unavailable". Locale-independent, never negative.
[[nodiscard]] std::string formatNetworkPacketMtuThroughput(
    const std::optional<double> &bytes_per_second);

/// Formats an estimated average frame size ("1514 bytes/frame") or
/// "unavailable" (see NetworkPacketSizeEstimate for the estimate caveat).
[[nodiscard]] std::string formatNetworkPacketSizeEstimate(
    const std::optional<double> &bytes_per_frame);

/// Renders the "Packet and MTU statistics" block used by the interface-details
/// view: freshness state, last update, coverage, configured MTU with its size
/// class and a change indicator, cumulative counters, interval rates, estimated
/// average frame sizes, and retained-window deltas — with explicit
/// "unavailable" for missing values. Pure ASCII, two-space indented to match
/// the surrounding detail page. Pure and unit-testable.
[[nodiscard]] std::string renderNetworkPacketMtu(
    const NetworkPacketMtuStats &stats, std::size_t max_samples);

/// Rendering an MTU change is a discrete log, not a sampled chart: a summary
/// line and up to kMaxRenderedPacketMtuEvents most-recent changes (newest
/// last), each with the previous value, the new value and a timestamp. Pure and
/// deterministic.
[[nodiscard]] std::string renderNetworkPacketMtuChanges(
    const NetworkPacketMtuStats &stats);

/// Chart geometry for the compact packet-rate / frame-size trend graph.
struct NetworkPacketMtuChartConfig {
  std::size_t data_width = 40;   // plot columns
  std::size_t data_height = 5;   // plot rows (y-axis scale labels included)
};

/// Selects which derived series a trend chart plots.
enum class NetworkPacketMtuTrendMetric {
  RxPacketRate,          // rx_packets_per_second
  TxPacketRate,          // tx_packets_per_second
  CombinedAvgFrameSize,  // combined sizes, else rx, else tx (bytes/frame)
};

/// Renders a compact monochrome-safe ASCII trend of the selected derived metric
/// over the retained ring, using the same glyph conventions as the other
/// history charts ('~' peak / '.' flow; blank columns for unavailable ticks).
/// Missing/gap ticks render as honest blanks — never a fabricated reading. When
/// the metric has no value at all a short "(no data)" marker is returned.
/// Deterministic and bounded (O(cols * samples) over the retained ring only).
[[nodiscard]] std::string renderNetworkPacketMtuTrend(
    const NetworkPacketMtuStats &stats, NetworkPacketMtuTrendMetric metric,
    const NetworkPacketMtuChartConfig &config);

/// Monitors per-interface MTU and packet-size statistics (Step 54).
///
/// Fed from the existing NetworkInterfaceMonitor snapshot exactly once per tick
/// by the application's monitoring loop (no second polling loop, nothing read
/// on the UI-render path). For each present interface it reads the kernel's
/// `/sys/class/net/<iface>/mtu` file with native filesystem APIs (no
/// subprocesses) and merges the same-tick `/proc/net/dev` traffic counters from
/// the snapshot, keeps a bounded per-tick sample ring under the existing
/// history-retention setting, pushes a bounded set of gone identities so a
/// temporary removal is not misreported, and keeps a bounded MTU-change
/// timeline. A recreated interface (new ifindex) naturally starts a fresh
/// record; a counter that decreases is handled per direction (rate unavailable
/// for that window, one discontinuity counted) instead of poisoning the rest.
/// An MTU-change event is recorded only when the value actually changed; a
/// temporary read failure preserves the current MTU and emits nothing. History
/// pause skips the reads and samples entirely.
class NetworkPacketMtuMonitor {
 public:
  static constexpr std::size_t kMaxTrackedPacketMtuInterfaces = 64;

  explicit NetworkPacketMtuMonitor(
      std::filesystem::path root = "/",
      std::size_t history_max_samples = kDefaultInterfaceHistorySamples);

  NetworkPacketMtuMonitor(const NetworkPacketMtuMonitor &) = delete;
  NetworkPacketMtuMonitor &operator=(const NetworkPacketMtuMonitor &) = delete;
  ~NetworkPacketMtuMonitor() = default;

  /// Processes one discovery snapshot (produced by NetworkInterfaceMonitor).
  void update(const NetworkInterfaceSnapshot &snapshot);

  /// Latest statistics for an identity; nullptr when never seen.
  [[nodiscard]] const NetworkPacketMtuStats *tracked(
      const std::string &identity) const;

  /// All retained statistics records (present and gone identities).
  [[nodiscard]] const std::unordered_map<std::string, NetworkPacketMtuStats> &
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
  std::unordered_map<std::string, NetworkPacketMtuStats> tracked_;
  void evictOverflow();
};

}  // namespace atm