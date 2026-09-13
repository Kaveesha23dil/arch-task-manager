#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "network_interface_details.hpp"
#include "network_monitor.hpp"
#include "resource_history.hpp"

namespace atm {

/// Stable identity of the aggregate "All interfaces" series. All other series
/// use the interface-details stable identity ("idx:<ifindex>" /
/// "name:<name>").
inline constexpr std::string_view kNetworkTrafficAllIdentity = "all";

/// Default bound for each traffic-history ring buffer (samples). Mirrors the
/// other per-entity history defaults so the "history retention" setting is the
/// single knob: services stay bounded and consistent with ResourcesHistory.
inline constexpr std::size_t kDefaultNetworkTrafficHistorySamples = 120;

/// One set of cumulative counters for a single interface at one point in time.
/// Values are raw kernel counters (cumulative, since boot); std::nullopt means
/// the counter was unavailable for that refresh and never means zero.
struct NetworkTrafficCounters {
  std::string identity;  // stable identity ("idx:..." / "name:...")
  std::string name;      // current kernel interface name
  std::optional<std::uint64_t> rx_bytes;
  std::optional<std::uint64_t> tx_bytes;
  std::optional<std::uint64_t> rx_packets;
  std::optional<std::uint64_t> tx_packets;
  std::optional<std::uint64_t> rx_errors;
  std::optional<std::uint64_t> tx_errors;
  std::optional<std::uint64_t> rx_dropped;
  std::optional<std::uint64_t> tx_dropped;
};

/// Derived transfer rates for one refresh window (rate = counter delta /
/// elapsed seconds; bytes per second for bytes, packets per second for the
/// packet and error/drop counters). A std::nullopt rate means "cannot be
/// calculated safely for this window" — no previous baseline yet, zero/negative
/// elapsed time, a counter reset or wraparound detected, or the counter not
/// reported this tick — never a fake zero and never a bogus spike.
struct NetworkTrafficRates {
  std::optional<double> rx_bytes_per_second;
  std::optional<double> tx_bytes_per_second;
  std::optional<double> rx_packets_per_second;
  std::optional<double> tx_packets_per_second;
  std::optional<double> rx_errors_per_second;
  std::optional<double> tx_errors_per_second;
  std::optional<double> rx_dropped_per_second;
  std::optional<double> tx_dropped_per_second;
};

/// Full per-interface sample for one refresh: stable identity, the current
/// name, ordering/wall timestamps, the raw cumulative counters and the derived
/// rates. Built transiently by NetworkTrafficHistory::record(); the counters
/// are then kept separately from the derived rates in the bounded series rings.
struct NetworkTrafficSample {
  std::string identity;
  std::string name;
  std::chrono::steady_clock::time_point timestamp;   // ordering / graph x-axis
  std::chrono::system_clock::time_point wall_clock;  // "last update" display
  NetworkTrafficCounters counters;
  NetworkTrafficRates rates;
};

/// Derives the per-metric rates for one refresh window from two cumulative
/// counter readouts and the real elapsed time.
///
/// Every rate is `(current - previous) / seconds`, computed with unsigned
/// (wraparound-free) integer subtraction before promotion to double, so no NaN,
/// infinity, division by zero or signed overflow is possible. When `seconds`
/// is not strictly positive, the two identities differ, or any counter that
/// was present in both readouts *decreased* (counter reset, interface restart
/// or 64-bit wraparound), ALL rates are unavailable for that window — the
/// caller resets its baseline so a huge fake delta is never plotted. A counter
/// present in only one readout (temporarily unavailable) just makes that
/// metric's rate unavailable while the others are still computed.
[[nodiscard]] NetworkTrafficRates deriveTrafficRates(
    const NetworkTrafficCounters &previous, const NetworkTrafficCounters &current,
    double seconds);

/// Sums the counters of the contributing interfaces into one aggregate
/// readout (optional-aware: a metric becomes unavailable when any member is
/// missing it). `name` is the display name of the aggregate. Sums that would
/// overflow uint64 are reported as unavailable rather than wrapped.
[[nodiscard]] NetworkTrafficCounters synthesizeAggregateCounters(
    const std::vector<const NetworkTrafficCounters *> &members,
    const std::string &name);

/// Bounded per-series traffic history. Every metric is a ResourceHistory ring
/// buffer timestamped consistently (one steady-clock read covers the whole
/// record batch). Raw cumulative byte counters are preserved separately from
/// the derived byte rates so window totals can be derived exactly.
struct NetworkTrafficSeries {
  std::string identity;       // "all" or a stable interface identity
  std::string display_name;   // "All interfaces" or the current interface name
  bool aggregate = false;

  // Derived rates (bytes/s and packets/s) — the graph/peak/sample data.
  ResourceHistory<TimedSample> rx_bytes_per_second{0};
  ResourceHistory<TimedSample> tx_bytes_per_second{0};
  ResourceHistory<TimedSample> rx_packets_per_second{0};
  ResourceHistory<TimedSample> tx_packets_per_second{0};
  ResourceHistory<TimedSample> rx_errors_per_second{0};
  ResourceHistory<TimedSample> tx_errors_per_second{0};
  ResourceHistory<TimedSample> rx_dropped_per_second{0};
  ResourceHistory<TimedSample> tx_dropped_per_second{0};

  // Raw cumulative counters at each sample (used for "total transferred during
  // the retained window"; never confused with the derived rates above).
  ResourceHistory<TimedSample> rx_bytes_total{0};
  ResourceHistory<TimedSample> tx_bytes_total{0};

  /// Wall clock of the most recent sample ("last update" display).
  std::chrono::system_clock::time_point last_update{};

  /// Aggregate only: true when the set of contributing interfaces changed in
  /// the most recent window (hotplug). Totals across a membership change are
  /// approximations; the UI must say so.
  bool membership_changed = false;

  [[nodiscard]] const ResourceHistory<TimedSample> &primaryRateHistory() const {
    return rx_bytes_per_second;
  }
};

/// Network-traffic history service.
///
/// Consumes, once per network refresh, the NetworkInterfaceMonitor snapshot of
/// the *same tick* (its `traffic` entries already carry the counters parsed
/// from /proc/net/dev by NetworkMonitor — no second parse and no second polling
/// loop). It derives safe per-window rates with reset/wraparound/suspend
/// handling, keeps raw cumulative counters in bounded ResourceHistory rings
/// keyed by the existing stable interface identity, maintains an "All
/// interfaces" aggregate series, and prunes series for interfaces that
/// disappear so ephemeral/hotplugged interfaces never accumulate forever.
class NetworkTrafficHistory {
 public:
  /// Upper bound on per-interface series (mirrors the interface-detail and
  /// filesystem history limits). The aggregate series is always kept.
  static constexpr std::size_t kMaxTrackedInterfaceSeries = 8;

  NetworkTrafficHistory() = default;
  ~NetworkTrafficHistory() = default;

  NetworkTrafficHistory(const NetworkTrafficHistory &) = delete;
  NetworkTrafficHistory &operator=(const NetworkTrafficHistory &) = delete;

  /// Appends exactly one sample per present interface (with traffic) and one
  /// for the aggregate, using a single timestamped batch. Interfaces without a
  /// traffic entry (counters temporarily unavailable) contribute nothing this
  /// tick; interfaces that vanish are pruned. Idempotent per tick: calling it
  /// more than once with the same snapshot adds more samples, so it must only
  /// be called once per completed network refresh.
  void record(const NetworkInterfaceSnapshot &interfaces);

  /// The series for a stable identity ("all" or "idx:..."/"name:..."), or
  /// nullptr when it is not tracked.
  [[nodiscard]] const NetworkTrafficSeries *seriesFor(
      const std::string &identity) const;

  [[nodiscard]] const std::unordered_map<std::string, NetworkTrafficSeries> &
  series() const {
    return series_;
  }

  /// Stable, ordered list of selectable identities: the aggregate first, then
  /// each tracked interface in discovery order. Renders/selection cycle over
  /// this; entries are never resequenced by sorting or filtering.
  [[nodiscard]] std::vector<std::string> selectableIdentities() const;

  /// Display name for a selection: "All interfaces" for the aggregate, the
  /// current interface name otherwise. Unknown identities resolve to the
  /// aggregate so a stale selection (removed interface) falls back gracefully.
  [[nodiscard]] std::string displayNameFor(const std::string &identity) const;

  /// True while the aggregate series exists (always true once any record has
  /// run or will run); the interface membership set is tracked separately.
  [[nodiscard]] bool aggregateAvailable() const;

  void setHistoryMaxSamples(std::size_t max_samples);
  [[nodiscard]] std::size_t historyMaxSamples() const { return max_samples_; }

  /// Temporarily stops collecting samples. Nothing is recorded while paused,
  /// and no fake samples are created to fill the gap when resumed.
  void setHistoryPaused(bool paused);
  [[nodiscard]] bool historyPaused() const { return history_paused_; }

  void clearHistory();

 private:
  std::unordered_map<std::string, NetworkTrafficSeries> series_;
  std::vector<std::string> selection_order_;

  // Last-read cumulative counters per identity (the diffing baseline); pruned
  // to present interfaces every record so ephemeral interfaces never grow it.
  std::unordered_map<std::string, NetworkTrafficCounters> baselines_;
  std::optional<std::chrono::steady_clock::time_point> previous_refresh_;
  std::vector<std::string> aggregate_members_;  // non-loopback identities last tick

  std::size_t max_samples_ = kDefaultNetworkTrafficHistorySamples;
  bool history_paused_ = false;

  NetworkTrafficSeries *seriesOrCreate(const std::string &identity,
                                       std::string display_name,
                                       bool aggregate);
  void pruneHistory(const std::vector<std::string> &present_identities);
};

}  // namespace atm