#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "network_interface_details.hpp"
#include "network_traffic_history.hpp"

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
void run(const char *name) { std::fprintf(stderr, "TEST %s\n", name); }
}  // namespace

#define CHECK(expr) ::expect((expr), #expr, __FILE__, __LINE__)

// -------------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------------

using atm::NetworkInterfaceInfo;
using atm::NetworkInterfaceSnapshot;
using atm::NetworkInterfaceStats;
using atm::NetworkInterfaceType;
using atm::NetworkTrafficCounters;
using atm::NetworkTrafficHistory;
using atm::NetworkTrafficRates;
using atm::NetworkTrafficSeries;

/// Builds one interface with merged traffic counters for history tests.
NetworkInterfaceInfo makeInterface(const std::string &name, int ifindex,
                                   NetworkInterfaceType type,
                                   std::uint64_t rx_bytes,
                                   std::uint64_t tx_bytes,
                                   std::uint64_t rx_packets = 0,
                                   std::uint64_t tx_packets = 0,
                                   std::uint64_t rx_errors = 0,
                                   std::uint64_t tx_errors = 0,
                                   std::uint64_t rx_dropped = 0,
                                   std::uint64_t tx_dropped = 0) {
  NetworkInterfaceInfo info;
  info.name = name;
  info.type = type;
  info.link.ifindex = ifindex;
  NetworkInterfaceStats traffic;
  traffic.name = name;
  traffic.rx_bytes = rx_bytes;
  traffic.tx_bytes = tx_bytes;
  traffic.rx_packets = rx_packets;
  traffic.tx_packets = tx_packets;
  traffic.rx_errors = rx_errors;
  traffic.tx_errors = tx_errors;
  traffic.rx_dropped = rx_dropped;
  traffic.tx_dropped = tx_dropped;
  info.traffic = traffic;
  info.refreshed_at = std::chrono::system_clock::now();
  return info;
}

/// Records after guaranteeing a positive, non-zero elapsed window so rate
/// samples are deterministically produced (steady clock only strictly advances
/// when something has elapsed).
void recordWithGap(NetworkTrafficHistory &monitor,
                   const NetworkInterfaceSnapshot &snapshot) {
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  monitor.record(snapshot);
}

double lastValue(const NetworkTrafficSeries &series, bool rx) {
  const auto &ring = rx ? series.rx_bytes_per_second
                        : series.tx_bytes_per_second;
  return ring.empty() ? double{} : ring.samples().back().value;
}

/// Last cumulative byte counter sample for a series.
double lastTotal(const NetworkTrafficSeries &series, bool rx) {
  const auto &ring = rx ? series.rx_bytes_total : series.tx_bytes_total;
  return ring.empty() ? double{} : ring.samples().back().value;
}

// -------------------------------------------------------------------------
// deriveTrafficRates
// -------------------------------------------------------------------------

NetworkTrafficCounters countersAt(const std::string &identity, const std::string &name,
                                  std::optional<std::uint64_t> rx_bytes,
                                  std::optional<std::uint64_t> tx_bytes,
                                  std::optional<std::uint64_t> rx_packets,
                                  std::optional<std::uint64_t> tx_packets,
                                  std::optional<std::uint64_t> rx_errors,
                                  std::optional<std::uint64_t> tx_errors,
                                  std::optional<std::uint64_t> rx_dropped,
                                  std::optional<std::uint64_t> tx_dropped) {
  NetworkTrafficCounters counters;
  counters.identity = identity;
  counters.name = name;
  counters.rx_bytes = rx_bytes;
  counters.tx_bytes = tx_bytes;
  counters.rx_packets = rx_packets;
  counters.tx_packets = tx_packets;
  counters.rx_errors = rx_errors;
  counters.tx_errors = tx_errors;
  counters.rx_dropped = rx_dropped;
  counters.tx_dropped = tx_dropped;
  return counters;
}

void test_rates_normal_interval() {
  run("rates divide counter deltas by elapsed seconds");
  const NetworkTrafficCounters prev =
      countersAt("idx:2", "eth0", 100, 50, 10, 5, 1, 0, 0, 0);
  const NetworkTrafficCounters curr =
      countersAt("idx:2", "eth0", 200, 120, 20, 10, 3, 2, 4, 1);
  const NetworkTrafficRates rates = atm::deriveTrafficRates(prev, curr, 2.0);
  CHECK(rates.rx_bytes_per_second.has_value() &&
        rates.rx_bytes_per_second.value() == 50.0);
  CHECK(rates.tx_bytes_per_second.has_value() &&
        rates.tx_bytes_per_second.value() == 35.0);
  CHECK(rates.rx_packets_per_second.has_value() &&
        rates.rx_packets_per_second.value() == 5.0);
  CHECK(rates.tx_packets_per_second.has_value() &&
        rates.tx_packets_per_second.value() == 2.5);
  CHECK(rates.rx_errors_per_second.has_value() &&
        rates.rx_errors_per_second.value() == 1.0);
  CHECK(rates.tx_errors_per_second.has_value() &&
        rates.tx_errors_per_second.value() == 1.0);
  CHECK(rates.rx_dropped_per_second.has_value() &&
        rates.rx_dropped_per_second.value() == 2.0);
  CHECK(rates.tx_dropped_per_second.has_value() &&
        rates.tx_dropped_per_second.value() == 0.5);
}

void test_rates_fractional_seconds() {
  run("sub-second windows produce correct fractional rates");
  const NetworkTrafficCounters prev =
      countersAt("idx:2", "eth0", 0, 0, 0, 0, 0, 0, 0, 0);
  const NetworkTrafficCounters curr =
      countersAt("idx:2", "eth0", 1, 2, 0, 0, 0, 0, 0, 0);
  const NetworkTrafficRates rates = atm::deriveTrafficRates(prev, curr, 0.25);
  CHECK(rates.rx_bytes_per_second.has_value() &&
        rates.rx_bytes_per_second.value() == 4.0);
  CHECK(rates.tx_bytes_per_second.has_value() &&
        rates.tx_bytes_per_second.value() == 8.0);
}

void test_rates_unsafe_windows() {
  run("zero, negative and non-finite windows yield no rates at all");
  const NetworkTrafficCounters prev =
      countersAt("idx:2", "eth0", 100, 100, 100, 100, 100, 100, 100, 100);
  const NetworkTrafficCounters curr =
      countersAt("idx:2", "eth0", 200, 200, 200, 200, 200, 200, 200, 200);
  for (double seconds : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN()}) {
    const NetworkTrafficRates rates =
        atm::deriveTrafficRates(prev, curr, seconds);
    CHECK(!rates.rx_bytes_per_second.has_value());
    CHECK(!rates.tx_bytes_per_second.has_value());
    CHECK(!rates.rx_packets_per_second.has_value());
    CHECK(!rates.tx_packets_per_second.has_value());
    CHECK(!rates.rx_errors_per_second.has_value());
    CHECK(!rates.tx_errors_per_second.has_value());
    CHECK(!rates.rx_dropped_per_second.has_value());
    CHECK(!rates.tx_dropped_per_second.has_value());
  }
}

void test_rates_identity_mismatch_or_missing() {
  run("a different or missing identity invalidates the window");
  const NetworkTrafficCounters prev =
      countersAt("idx:2", "eth0", 100, 100, 100, 100, 100, 100, 100, 100);
  const NetworkTrafficCounters curr =
      countersAt("idx:3", "eth0", 200, 200, 200, 200, 200, 200, 200, 200);
  const NetworkTrafficRates mismatch =
      atm::deriveTrafficRates(prev, curr, 1.0);
  CHECK(!mismatch.rx_bytes_per_second.has_value());

  const NetworkTrafficCounters unnamed =
      countersAt("", "eth0", 100, 100, 100, 100, 100, 100, 100, 100);
  const NetworkTrafficRates missing =
      atm::deriveTrafficRates(unnamed, curr, 1.0);
  CHECK(!missing.rx_bytes_per_second.has_value());
}

void test_rates_counter_reset() {
  run("any decreased counter invalidates every rate for that window");
  const NetworkTrafficCounters prev =
      countersAt("idx:2", "eth0", 100, 100, 100, 100, 100, 100, 100, 100);
  const NetworkTrafficCounters reset =
      countersAt("idx:2", "eth0", 0, 101, 101, 101, 101, 101, 101, 101);
  const NetworkTrafficRates rates = atm::deriveTrafficRates(prev, reset, 1.0);
  CHECK(!rates.rx_bytes_per_second.has_value());
  CHECK(!rates.tx_bytes_per_second.has_value());
  CHECK(!rates.rx_packets_per_second.has_value());
  CHECK(!rates.tx_packets_per_second.has_value());
  CHECK(!rates.rx_errors_per_second.has_value());
  CHECK(!rates.tx_errors_per_second.has_value());
  CHECK(!rates.rx_dropped_per_second.has_value());
  CHECK(!rates.tx_dropped_per_second.has_value());
}

void test_rates_missing_counter_is_isolated() {
  run("a counter missing on one side only disables that metric");
  NetworkTrafficCounters prev =
      countersAt("idx:2", "eth0", 100, 100, 100, 100, 100, 100, 100, 100);
  prev.rx_bytes = std::nullopt;  // unavailable last tick
  const NetworkTrafficCounters curr =
      countersAt("idx:2", "eth0", 200, 300, 200, 200, 200, 200, 200, 200);
  const NetworkTrafficRates rates = atm::deriveTrafficRates(prev, curr, 2.0);
  CHECK(!rates.rx_bytes_per_second.has_value());
  CHECK(rates.tx_bytes_per_second.has_value() &&
        rates.tx_bytes_per_second.value() == 100.0);
  CHECK(rates.rx_packets_per_second.has_value() &&
        rates.rx_packets_per_second.value() == 50.0);
}

void test_rates_uint64_full_range() {
  run("full uint64-range deltas stay exact (no signed overflow)");
  const NetworkTrafficCounters prev =
      countersAt("idx:2", "eth0", 0, 0, 0, 0, 0, 0, 0, 0);
  const NetworkTrafficCounters curr = countersAt(
      "idx:2", "eth0", std::numeric_limits<std::uint64_t>::max(), 0, 0, 0, 0,
      0, 0, 0);
  const NetworkTrafficRates rates = atm::deriveTrafficRates(prev, curr, 10.0);
  CHECK(rates.rx_bytes_per_second.has_value());
  const double expected = static_cast<double>(
      std::numeric_limits<std::uint64_t>::max()) / 10.0;
  CHECK(rates.rx_bytes_per_second.value() == expected);
}

// -------------------------------------------------------------------------
// synthesizeAggregateCounters
// -------------------------------------------------------------------------

void test_aggregate_sums() {
  run("aggregate sums parallel per-metric counters");
  const NetworkTrafficCounters a =
      countersAt("idx:2", "eth0", 100, 10, 30, 5, 1, 2, 3, 4);
  const NetworkTrafficCounters b =
      countersAt("idx:3", "eth1", 500, 90, 70, 7, 9, 8, 6, 5);
  const std::vector<const NetworkTrafficCounters *> members = {&a, &b};
  const NetworkTrafficCounters aggregate =
      atm::synthesizeAggregateCounters(members, "All interfaces");
  CHECK(aggregate.identity == std::string(atm::kNetworkTrafficAllIdentity));
  CHECK(aggregate.name == "All interfaces");
  CHECK(aggregate.rx_bytes.has_value() && *aggregate.rx_bytes == 600);
  CHECK(aggregate.tx_bytes.has_value() && *aggregate.tx_bytes == 100);
  CHECK(aggregate.rx_packets.has_value() && *aggregate.rx_packets == 100);
  CHECK(aggregate.tx_packets.has_value() && *aggregate.tx_packets == 12);
  CHECK(aggregate.rx_errors.has_value() && *aggregate.rx_errors == 10);
  CHECK(aggregate.tx_errors.has_value() && *aggregate.tx_errors == 10);
  CHECK(aggregate.rx_dropped.has_value() && *aggregate.rx_dropped == 9);
  CHECK(aggregate.tx_dropped.has_value() && *aggregate.tx_dropped == 9);
}

void test_aggregate_missing_member_disables_metric() {
  run("a member missing one counter disables only that aggregate metric");
  NetworkTrafficCounters a =
      countersAt("idx:2", "eth0", 100, 10, 30, 5, 1, 2, 3, 4);
  a.rx_bytes = std::nullopt;
  const NetworkTrafficCounters b =
      countersAt("idx:3", "eth1", 500, 90, 70, 7, 9, 8, 6, 5);
  const std::vector<const NetworkTrafficCounters *> members = {&a, &b};
  const NetworkTrafficCounters aggregate =
      atm::synthesizeAggregateCounters(members, "All interfaces");
  CHECK(!aggregate.rx_bytes.has_value());
  CHECK(aggregate.tx_bytes.has_value() && *aggregate.tx_bytes == 100);
  CHECK(aggregate.rx_packets.has_value() && *aggregate.rx_packets == 100);
}

void test_aggregate_empty_and_overflow() {
  run("empty members and overflow are reported as unavailable");
  const std::vector<const NetworkTrafficCounters *> empty_members;
  const NetworkTrafficCounters empty_agg =
      atm::synthesizeAggregateCounters(empty_members, "All interfaces");
  CHECK(!empty_agg.rx_bytes.has_value());
  CHECK(!empty_agg.tx_bytes.has_value());

  const NetworkTrafficCounters big =
      countersAt("idx:2", "eth0", std::numeric_limits<std::uint64_t>::max(),
                 std::numeric_limits<std::uint64_t>::max(), 1, 1, 0, 0, 0, 0);
  const NetworkTrafficCounters also_big =
      countersAt("idx:3", "eth1", std::numeric_limits<std::uint64_t>::max(),
                 std::numeric_limits<std::uint64_t>::max(), 2, 2, 0, 0, 0, 0);
  const std::vector<const NetworkTrafficCounters *> members = {&big, &also_big};
  const NetworkTrafficCounters overflow =
      atm::synthesizeAggregateCounters(members, "All interfaces");
  CHECK(!overflow.rx_bytes.has_value());  // never wraps
  CHECK(!overflow.tx_bytes.has_value());
  CHECK(overflow.rx_packets.has_value() && *overflow.rx_packets == 3);
}

// -------------------------------------------------------------------------
// NetworkTrafficHistory::record
// -------------------------------------------------------------------------

NetworkInterfaceSnapshot snapshotWith(
    const std::vector<NetworkInterfaceInfo> &interfaces) {
  NetworkInterfaceSnapshot snapshot;
  snapshot.interfaces = interfaces;
  snapshot.refreshed_at = std::chrono::system_clock::now();
  return snapshot;
}

void test_record_creates_series_and_aggregate() {
  run("one series per interface plus the aggregate, first sample only");
  NetworkTrafficHistory monitor;
  std::vector<NetworkInterfaceInfo> interfaces;
  interfaces.push_back(makeInterface("eth0", 2, NetworkInterfaceType::Ethernet,
                                     1000, 500));
  interfaces.push_back(makeInterface("lo", 1, NetworkInterfaceType::Loopback,
                                     900000, 900000));
  monitor.record(snapshotWith(interfaces));

  const NetworkTrafficSeries *eth0 = monitor.seriesFor("idx:2");
  const NetworkTrafficSeries *lo = monitor.seriesFor("idx:1");
  const NetworkTrafficSeries *all = monitor.seriesFor("all");
  CHECK(eth0 != nullptr);
  CHECK(lo != nullptr);
  CHECK(all != nullptr);
  CHECK(eth0->aggregate == false);
  CHECK(all->aggregate == true);
  CHECK(eth0->display_name == "eth0");

  // Exactly one cumulative sample per interface per refresh on the first call.
  CHECK(eth0->rx_bytes_total.size() == 1);
  CHECK(lo->rx_bytes_total.size() == 1);
  CHECK(all->rx_bytes_total.size() == 1);

  // No baseline yet on the first call: rate rings are empty, never fake zeros.
  CHECK(eth0->rx_bytes_per_second.empty());
  CHECK(eth0->tx_bytes_per_second.empty());

  // The aggregate excludes loopback (mirrors the live totals).
  CHECK(lastTotal(*all, /*rx=*/true) == 1000.0);
  CHECK(lastTotal(*all, /*tx=*/false) == 500.0);

  // Selection order: aggregate first, then interfaces in discovery order.
  const std::vector<std::string> selectable = monitor.selectableIdentities();
  CHECK(selectable.size() == 3);
  CHECK(selectable[0] == "all");
  CHECK(selectable[1] == "idx:2");
  CHECK(selectable[2] == "idx:1");
  CHECK(monitor.displayNameFor("all") == "All interfaces");
  CHECK(monitor.displayNameFor("idx:2") == "eth0");
}

void test_record_one_sample_per_refresh() {
  run("a second refresh appends a rate sample for a linked baseline");
  NetworkTrafficHistory monitor;
  std::vector<NetworkInterfaceInfo> interfaces;
  interfaces.push_back(makeInterface("eth0", 2, NetworkInterfaceType::Ethernet,
                                     1000, 500));
  monitor.record(snapshotWith(interfaces));
  recordWithGap(monitor, snapshotWith(interfaces));

  const NetworkTrafficSeries *eth0 = monitor.seriesFor("idx:2");
  CHECK(eth0 != nullptr);
  CHECK(eth0->rx_bytes_total.size() == 2);
  CHECK(eth0->rx_bytes_per_second.size() == 1);
  CHECK(lastValue(*eth0, /*rx=*/true) >= 0.0);
}

void test_record_rate_reset_is_not_spiked() {
  run("a counter decrease reseeds the baseline and adds no spike");
  NetworkTrafficHistory monitor;
  std::vector<NetworkInterfaceInfo> interfaces;
  interfaces.push_back(makeInterface("eth0", 2, NetworkInterfaceType::Ethernet,
                                     100, 100));
  monitor.record(snapshotWith(interfaces));
  recordWithGap(monitor, snapshotWith(interfaces));  // 100 -> 100
  std::vector<NetworkInterfaceInfo> advancing;
  advancing.push_back(makeInterface("eth0", 2, NetworkInterfaceType::Ethernet,
                                    300, 300));
  recordWithGap(monitor, snapshotWith(advancing));  // 100 -> 300, window valid

  const NetworkTrafficSeries *eth0 = monitor.seriesFor("idx:2");
  CHECK(eth0 != nullptr);
  CHECK(eth0->rx_bytes_per_second.size() == 2);

  interfaces.clear();
  interfaces.push_back(makeInterface("eth0", 2, NetworkInterfaceType::Ethernet,
                                     0, 0));  // kernel counters reset
  recordWithGap(monitor, snapshotWith(interfaces));
  const std::size_t before_reset = eth0->rx_bytes_per_second.size();
  recordWithGap(monitor, snapshotWith(interfaces));  // reseeded, valid again
  CHECK(eth0->rx_bytes_per_second.size() == before_reset + 1);
}

void test_record_ignores_interfaces_without_traffic() {
  run("interfaces without merged traffic are not tracked");
  NetworkTrafficHistory monitor;
  NetworkInterfaceInfo eth0 = makeInterface("eth0", 2,
                                            NetworkInterfaceType::Ethernet,
                                            1000, 1000);
  eth0.traffic = std::nullopt;  // /proc/net/dev absent for this interface
  std::vector<NetworkInterfaceInfo> interfaces;
  interfaces.push_back(eth0);
  monitor.record(snapshotWith(interfaces));

  CHECK(monitor.seriesFor("idx:2") == nullptr);
  CHECK(monitor.seriesFor("all") != nullptr);  // aggregate still exists
  CHECK(monitor.selectableIdentities().size() == 1);
  CHECK(monitor.selectableIdentities()[0] == "all");
}

void test_record_empty_snapshot_still_has_aggregate() {
  run("an empty snapshot keeps the aggregate, prunes nothing");
  NetworkTrafficHistory monitor;
  monitor.record(snapshotWith({}));
  CHECK(monitor.aggregateAvailable());
  CHECK(monitor.selectableIdentities().size() == 1 &&
        monitor.selectableIdentities()[0] == "all");
  CHECK(monitor.seriesFor("all")->rx_bytes_total.empty());
}

void test_pause_ignores_record() {
  run("paused history records nothing and resumes cleanly");
  NetworkTrafficHistory monitor;
  monitor.setHistoryPaused(true);

  std::vector<NetworkInterfaceInfo> interfaces;
  interfaces.push_back(makeInterface("eth0", 2, NetworkInterfaceType::Ethernet,
                                     1000, 1000));
  NetworkInterfaceSnapshot snapshot = snapshotWith(interfaces);
  monitor.record(snapshot);
  monitor.record(snapshot);
  CHECK(monitor.series().size() == 0);
  CHECK(monitor.seriesFor("all") == nullptr);
  CHECK(!monitor.aggregateAvailable());
  CHECK(monitor.selectableIdentities().size() == 1 &&
        monitor.selectableIdentities()[0] == "all");

  monitor.setHistoryPaused(false);
  monitor.record(snapshot);
  CHECK(monitor.seriesFor("idx:2") != nullptr);
  CHECK(monitor.seriesFor("all") != nullptr);
  CHECK(monitor.seriesFor("idx:2")->rx_bytes_total.size() == 1);
  CHECK(monitor.seriesFor("idx:2")->rx_bytes_per_second.empty());
}

void test_prune_vanished_interface() {
  run("a disappeared interface is pruned from series and selection");
  NetworkTrafficHistory monitor;
  std::vector<NetworkInterfaceInfo> with_lo_eth;
  with_lo_eth.push_back(makeInterface("eth0", 2, NetworkInterfaceType::Ethernet,
                                      1000, 1000));
  with_lo_eth.push_back(makeInterface("lo", 1, NetworkInterfaceType::Loopback,
                                      999, 999));
  monitor.record(snapshotWith(with_lo_eth));

  std::vector<NetworkInterfaceInfo> only_eth;
  only_eth.push_back(makeInterface("eth0", 2, NetworkInterfaceType::Ethernet,
                                   1000, 1000));
  monitor.record(snapshotWith(only_eth));

  CHECK(monitor.seriesFor("idx:1") == nullptr);  // lo gone from /sys
  CHECK(monitor.seriesFor("idx:2") != nullptr);
  const std::vector<std::string> selectable = monitor.selectableIdentities();
  CHECK(selectable.size() == 2);
  CHECK(selectable[1] == "idx:2");
  CHECK(monitor.displayNameFor("idx:1") == "All interfaces");  // stale fallback
}

void test_aggregate_membership_change() {
  run("aggregate membership changes are flagged, stable sets are not");
  NetworkTrafficHistory monitor;

  std::vector<NetworkInterfaceInfo> one;
  one.push_back(makeInterface("eth0", 2, NetworkInterfaceType::Ethernet, 100,
                              100));
  monitor.record(snapshotWith(one));
  CHECK(monitor.seriesFor("all")->membership_changed);

  std::vector<NetworkInterfaceInfo> two;
  two.push_back(makeInterface("eth0", 2, NetworkInterfaceType::Ethernet, 100,
                              100));
  two.push_back(makeInterface("eth1", 3, NetworkInterfaceType::Ethernet, 200,
                              200));
  monitor.record(snapshotWith(two));
  CHECK(monitor.seriesFor("all")->membership_changed);

  monitor.record(snapshotWith(two));
  CHECK(!monitor.seriesFor("all")->membership_changed);

  monitor.record(snapshotWith(one));
  CHECK(monitor.seriesFor("all")->membership_changed);
}

void test_aggregate_sums_and_excludes_loopback() {
  run("aggregate totals sum non-loopback counters only");
  NetworkTrafficHistory monitor;
  std::vector<NetworkInterfaceInfo> interfaces;
  interfaces.push_back(makeInterface("eth0", 2, NetworkInterfaceType::Ethernet,
                                     1000, 500));
  interfaces.push_back(makeInterface("eth1", 3, NetworkInterfaceType::Ethernet,
                                     500, 250));
  interfaces.push_back(makeInterface("lo", 1, NetworkInterfaceType::Loopback,
                                     999999, 999999));
  monitor.record(snapshotWith(interfaces));

  const NetworkTrafficSeries *all = monitor.seriesFor("all");
  CHECK(all != nullptr);
  CHECK(lastTotal(*all, true) == 1500.0);
  CHECK(lastTotal(*all, false) == 750.0);

  // The loopback interface itself is individually tracked without inflating it.
  CHECK(monitor.seriesFor("idx:1") != nullptr);
  CHECK(monitor.seriesFor("idx:1")->rx_bytes_total.samples().back().value ==
        999999.0);
}

void test_history_bounded() {
  run("history is bounded by the configured sample count");
  NetworkTrafficHistory monitor;
  monitor.setHistoryMaxSamples(3);
  std::vector<NetworkInterfaceInfo> interfaces;
  interfaces.push_back(makeInterface("eth0", 2, NetworkInterfaceType::Ethernet,
                                     100, 100));
  NetworkInterfaceSnapshot snapshot = snapshotWith(interfaces);
  for (int i = 0; i < 6; ++i) {
    monitor.record(snapshot);
  }
  const NetworkTrafficSeries *eth0 = monitor.seriesFor("idx:2");
  CHECK(eth0 != nullptr);
  CHECK(eth0->rx_bytes_total.size() == 3);
  CHECK(monitor.seriesFor("all")->rx_bytes_total.size() == 3);
}

void test_series_cap_limits_tracked_interfaces() {
  run("excessive interface counts are bounded but aggregate stays complete");
  NetworkTrafficHistory monitor;
  monitor.setHistoryMaxSamples(8);

  std::vector<NetworkInterfaceInfo> interfaces;
  std::uint64_t total_rx = 0;
  for (int i = 0; i < 12; ++i) {
    const std::uint64_t rx = static_cast<std::uint64_t>(i + 1) * 10;
    total_rx += rx;
    interfaces.push_back(makeInterface("net" + std::to_string(i), 100 + i,
                                       NetworkInterfaceType::Ethernet, rx, rx));
  }
  NetworkInterfaceSnapshot snapshot = snapshotWith(interfaces);
  monitor.record(snapshot);
  monitor.record(snapshot);

  std::size_t tracked = 0;
  for (int i = 0; i < 12; ++i) {
    const NetworkTrafficSeries *series = monitor.seriesFor(
        "idx:" + std::to_string(100 + i));
    if (series != nullptr) {
      ++tracked;
    }
  }
  CHECK(tracked == NetworkTrafficHistory::kMaxTrackedInterfaceSeries);
  CHECK(monitor.seriesFor("idx:100") != nullptr);
  CHECK(monitor.seriesFor("idx:111") == nullptr);  // beyond the cap, not created

  // The aggregate covers every member even when individual series are capped.
  const NetworkTrafficSeries *all = monitor.seriesFor("all");
  CHECK(all != nullptr);
  CHECK(lastTotal(*all, true) == static_cast<double>(total_rx));
}

void test_rename_keeps_series_by_identity() {
  run("a rename keeps the series via the stable index identity");
  NetworkTrafficHistory monitor;
  std::vector<NetworkInterfaceInfo> before;
  before.push_back(makeInterface("eth0", 2, NetworkInterfaceType::Ethernet, 100,
                                 100));
  monitor.record(snapshotWith(before));

  std::vector<NetworkInterfaceInfo> after;
  after.push_back(makeInterface("eth-fast", 2, NetworkInterfaceType::Ethernet,
                                200, 200));
  monitor.record(snapshotWith(after));

  const NetworkTrafficSeries *series = monitor.seriesFor("idx:2");
  CHECK(series != nullptr);
  CHECK(series->display_name == "eth-fast");
  CHECK(monitor.selectableIdentities().size() == 2);
  CHECK(monitor.displayNameFor("idx:2") == "eth-fast");
}

void test_reconfigure_and_clear() {
  run("reconfiguring sample count rebuilds rings; clear empties everything");
  NetworkTrafficHistory monitor;
  std::vector<NetworkInterfaceInfo> interfaces;
  interfaces.push_back(makeInterface("eth0", 2, NetworkInterfaceType::Ethernet,
                                     100, 100));
  NetworkInterfaceSnapshot snapshot = snapshotWith(interfaces);
  monitor.record(snapshot);
  CHECK(monitor.seriesFor("idx:2")->rx_bytes_total.size() == 1);

  monitor.setHistoryMaxSamples(16);
  const NetworkTrafficSeries *rebuild = monitor.seriesFor("idx:2");
  CHECK(rebuild != nullptr);
  CHECK(rebuild->rx_bytes_total.empty());  // rings rebuilt empty
  monitor.record(snapshot);
  CHECK(monitor.seriesFor("idx:2")->rx_bytes_total.size() == 1);

  monitor.clearHistory();
  CHECK(monitor.series().empty());
  CHECK(monitor.seriesFor("all") == nullptr);
  CHECK(monitor.selectableIdentities().size() == 1);  // aggregate fallback only

  monitor.record(snapshot);
  CHECK(monitor.seriesFor("all") != nullptr);
  const NetworkTrafficSeries *fresh = monitor.seriesFor("idx:2");
  CHECK(fresh != nullptr);
  CHECK(fresh->rx_bytes_per_second.empty());  // baseline reset after clear
}

int main() {
  // deriveTrafficRates
  test_rates_normal_interval();
  test_rates_fractional_seconds();
  test_rates_unsafe_windows();
  test_rates_identity_mismatch_or_missing();
  test_rates_counter_reset();
  test_rates_missing_counter_is_isolated();
  test_rates_uint64_full_range();

  // synthesizeAggregateCounters
  test_aggregate_sums();
  test_aggregate_missing_member_disables_metric();
  test_aggregate_empty_and_overflow();

  // record()
  test_record_creates_series_and_aggregate();
  test_record_one_sample_per_refresh();
  test_record_rate_reset_is_not_spiked();
  test_record_ignores_interfaces_without_traffic();
  test_record_empty_snapshot_still_has_aggregate();
  test_pause_ignores_record();
  test_prune_vanished_interface();
  test_aggregate_membership_change();
  test_aggregate_sums_and_excludes_loopback();
  test_history_bounded();
  test_series_cap_limits_tracked_interfaces();
  test_rename_keeps_series_by_identity();
  test_reconfigure_and_clear();

  std::fprintf(stderr, "\n%zu checks, %d failures\n", g_checks + 0UL,
               g_failures);
  if (g_failures != 0) {
    std::fprintf(stderr, "RESULT: FAIL\n");
    return 1;
  }
  std::fprintf(stderr, "RESULT: PASS\n");
  return 0;
}