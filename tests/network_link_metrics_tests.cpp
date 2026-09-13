#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "network_interface_details.hpp"
#include "network_link_metrics.hpp"
#include "network_link_state.hpp"

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

using atm::NetworkDuplexMode;
using atm::NetworkDuplexState;
using atm::NetworkInterfaceInfo;
using atm::NetworkInterfaceSnapshot;
using atm::NetworkInterfaceType;
using atm::NetworkLinkMetrics;
using atm::NetworkSpeedState;

constexpr unsigned kIffUp = 0x1u;

/// Builds one interface-discovery record. `flags` drives both the raw flags
/// and the derived `admin_up` (IFF_UP). Speed/duplex represent the raw sysfs
/// files exactly as NetworkInterfaceMonitor reads them.
NetworkInterfaceInfo makeInfo(
    const std::string &name, int ifindex,
    const std::optional<std::string> &operstate,
    const std::optional<int> &carrier,
    const std::optional<unsigned> &flags,
    const std::optional<int> &speed_mbps,
    const std::optional<std::string> &duplex,
    NetworkInterfaceType type) {
  NetworkInterfaceInfo info;
  info.name = name;
  info.type = type;
  info.link.ifindex = ifindex;
  info.link.operstate = operstate;
  info.link.carrier = carrier;
  info.link.flags = flags;
  info.link.admin_up = flags.has_value() ? ((*flags & kIffUp) != 0) : false;
  info.link.speed_mbps = speed_mbps;
  info.link.duplex = duplex;
  info.refreshed_at = std::chrono::system_clock::now();
  return info;
}

/// A physical Ethernet interface, operationally up with carrier, negotiating
/// 1 Gbps full duplex.
NetworkInterfaceInfo ethUpActive() {
  return makeInfo("eth0", 1, std::string("up"), 1, kIffUp, 1000,
                  std::string("full"), NetworkInterfaceType::Ethernet);
}

/// The same interface administratively up but with the link down; the driver
/// still reports the last negotiated speed.
NetworkInterfaceInfo ethUpDown() {
  return makeInfo("eth0", 1, std::string("down"), 0, kIffUp, 1000,
                  std::string("full"), NetworkInterfaceType::Ethernet);
}

/// The same interface, now failing to report any speed/duplex (~= "-1" /
/// unreadable): the honest "read failure" case.
NetworkInterfaceInfo ethNoSpeed() {
  return makeInfo("eth0", 1, std::string("down"), 0, kIffUp, std::nullopt,
                  std::nullopt, NetworkInterfaceType::Ethernet);
}

NetworkInterfaceSnapshot makeSnapshot(
    const std::vector<NetworkInterfaceInfo> &interfaces) {
  NetworkInterfaceSnapshot snapshot;
  snapshot.interfaces = interfaces;
  snapshot.sysfs_readable = true;
  snapshot.addresses_readable = true;
  snapshot.refreshed_at = std::chrono::system_clock::now();
  return snapshot;
}

// -------------------------------------------------------------------------
// Duplex parsing / naming
// -------------------------------------------------------------------------

void test_parse_duplex_full_half() {
  run("parseDuplexFullHalf");
  CHECK(atm::parseNetworkDuplex(std::string("full")) == NetworkDuplexMode::Full);
  CHECK(atm::parseNetworkDuplex(std::string("half")) == NetworkDuplexMode::Half);
}

void test_parse_duplex_unknown() {
  run("parseDuplexUnknownNeverZero");
  CHECK(atm::parseNetworkDuplex(std::nullopt) == NetworkDuplexMode::Unknown);
  CHECK(atm::parseNetworkDuplex(std::string("unknown")) ==
        NetworkDuplexMode::Unknown);
  CHECK(atm::parseNetworkDuplex(std::string("")) == NetworkDuplexMode::Unknown);
  CHECK(atm::parseNetworkDuplex(std::string("FULL")) ==
        NetworkDuplexMode::Unknown);  // exact lowercase only
  CHECK(atm::parseNetworkDuplex(std::string("half-duplex")) ==
        NetworkDuplexMode::Unknown);
}

void test_state_names() {
  run("stateNamesStable");
  CHECK(std::string(atm::networkDuplexModeName(NetworkDuplexMode::Full)) ==
        "full");
  CHECK(std::string(atm::networkDuplexModeName(NetworkDuplexMode::Half)) ==
        "half");
  CHECK(std::string(atm::networkSpeedStateName(NetworkSpeedState::Valid)) ==
        "valid");
  CHECK(std::string(atm::networkSpeedStateName(NetworkSpeedState::Stale)) ==
        "stale");
  CHECK(std::string(atm::networkSpeedStateName(NetworkSpeedState::Unavailable)) ==
        "unavailable");
  CHECK(std::string(atm::networkDuplexStateName(NetworkDuplexState::Stale)) ==
        "stale");
}

// -------------------------------------------------------------------------
// Pure derive() profiles
// -------------------------------------------------------------------------

void test_fresh_active_link_valid() {
  run("FreshActiveLinkValid");
  NetworkLinkMetrics base;
  base.identity = "idx:1";
  const auto refreshed = std::chrono::system_clock::now();
  const NetworkLinkMetrics m =
      atm::updateNetworkLinkMetrics(base, ethUpActive(), true, refreshed);
  CHECK(m.physical);
  CHECK(m.link_active);
  CHECK(m.speed_state == NetworkSpeedState::Valid);
  CHECK(m.speed_mbps.has_value() && *m.speed_mbps == 1000);
  CHECK(m.duplex_state == NetworkDuplexState::Valid);
  CHECK(m.duplex == NetworkDuplexMode::Full);
  CHECK(m.last_update == refreshed);
}

void test_fresh_inactive_link_stale() {
  run("FreshInactiveLinkStale");
  NetworkLinkMetrics base;
  base.identity = "idx:1";
  const auto refreshed = std::chrono::system_clock::now();
  const NetworkLinkMetrics m =
      atm::updateNetworkLinkMetrics(base, ethUpDown(), false, refreshed);
  CHECK(!m.link_active);
  CHECK(m.speed_state == NetworkSpeedState::Stale);
  CHECK(m.speed_mbps.has_value() && *m.speed_mbps == 1000);
  CHECK(m.duplex_state == NetworkDuplexState::Stale);
  CHECK(m.duplex == NetworkDuplexMode::Full);
}

void test_read_failure_preserves_last_value() {
  run("ReadFailurePreservesLastValue");
  NetworkLinkMetrics previous;
  previous.identity = "idx:1";
  previous.physical = true;
  previous.speed_state = NetworkSpeedState::Valid;
  previous.speed_mbps = 1000;
  previous.duplex_state = NetworkDuplexState::Valid;
  previous.duplex = NetworkDuplexMode::Full;
  const auto last_read = std::chrono::system_clock::now();
  previous.last_update = last_read;

  const auto refreshed = last_read + std::chrono::seconds(1);
  const NetworkLinkMetrics m =
      atm::updateNetworkLinkMetrics(previous, ethNoSpeed(), false, refreshed);
  // The last known value is preserved and marked stale — never dropped, never
  // zero, and the update timestamp stays at the last successful read.
  CHECK(m.speed_state == NetworkSpeedState::Stale);
  CHECK(m.speed_mbps.has_value() && *m.speed_mbps == 1000);
  CHECK(m.duplex_state == NetworkDuplexState::Stale);
  CHECK(m.duplex == NetworkDuplexMode::Full);
  CHECK(m.last_update == last_read);
}

void test_partial_read_duplex_only() {
  run("PartialReadDuplexOnly");
  // A wifi link reports duplex "unknown" (no negotiated duplex yet) while the
  // speed is still fresh: speed valid, duplex unavailable.
  NetworkLinkMetrics base;
  base.identity = "idx:1";
  const auto refreshed = std::chrono::system_clock::now();
  const NetworkLinkMetrics m = atm::updateNetworkLinkMetrics(
      base,
      makeInfo("wlp2s0", 2, std::string("up"), 1, kIffUp, 866,
               std::string("unknown"), NetworkInterfaceType::Wifi),
      true, refreshed);
  CHECK(m.speed_state == NetworkSpeedState::Valid);
  CHECK(m.speed_mbps.has_value() && *m.speed_mbps == 866);
  CHECK(m.duplex_state == NetworkDuplexState::Unavailable);
  CHECK(m.duplex == NetworkDuplexMode::Unknown);
}

void test_never_reported_unavailable() {
  run("NeverReportedUnavailable");
  NetworkLinkMetrics base;
  base.identity = "idx:1";
  const auto refreshed = std::chrono::system_clock::now();
  const NetworkLinkMetrics m = atm::updateNetworkLinkMetrics(
      base,
      makeInfo("eth0", 1, std::string("up"), 1, kIffUp, std::nullopt,
               std::nullopt, NetworkInterfaceType::Ethernet),
      true, refreshed);
  CHECK(m.speed_state == NetworkSpeedState::Unavailable);
  CHECK(!m.speed_mbps.has_value());
  CHECK(m.duplex_state == NetworkDuplexState::Unavailable);
  CHECK(m.duplex == NetworkDuplexMode::Unknown);
  CHECK(m.last_update == std::chrono::system_clock::time_point{});
}

void test_virtual_never_meaningful() {
  run("VirtualNeverMeaningful");
  // A synthetic interface reports a bogus speed; the metrics layer must not
  // echo it (it is not a negotiated physical link).
  NetworkLinkMetrics base;
  base.identity = "idx:9";
  base.speed_mbps = 1000;  // stale value from an earlier physical incarnation
  const auto refreshed = std::chrono::system_clock::now();
  const NetworkLinkMetrics m = atm::updateNetworkLinkMetrics(
      base,
      makeInfo("docker0", 9, std::string("down"), 0, kIffUp, 10000,
               std::string("full"), NetworkInterfaceType::Virtual),
      false, refreshed);
  CHECK(!m.physical);
  CHECK(m.speed_state == NetworkSpeedState::Unavailable);
  CHECK(!m.speed_mbps.has_value());
  CHECK(m.duplex_state == NetworkDuplexState::Unavailable);
  CHECK(m.duplex == NetworkDuplexMode::Unknown);
}

void test_loopback_unavailable() {
  run("LoopbackUnavailable");
  NetworkLinkMetrics base;
  base.identity = "idx:1";
  const auto refreshed = std::chrono::system_clock::now();
  const NetworkLinkMetrics m = atm::updateNetworkLinkMetrics(
      base,
      makeInfo("lo", 1, std::string("unknown"), 1, kIffUp, std::nullopt,
               std::nullopt, NetworkInterfaceType::Loopback),
      true, refreshed);
  CHECK(m.speed_state == NetworkSpeedState::Unavailable);
  CHECK(m.duplex_state == NetworkDuplexState::Unavailable);
}

// -------------------------------------------------------------------------
// Monitor behaviour
// -------------------------------------------------------------------------

void test_monitor_tracks_rename() {
  run("MonitorTracksRename");
  atm::NetworkLinkMetricsMonitor monitor;
  monitor.update(makeSnapshot({ethUpActive()}));
  const NetworkLinkMetrics *m = monitor.tracked("idx:1");
  CHECK(m != nullptr);
  CHECK(m->present);
  CHECK(m->name == "eth0");
  CHECK(m->speed_state == NetworkSpeedState::Valid);

  // Rename to eth5 keeping the same ifindex: same identity, new name.
  NetworkInterfaceInfo renamed = ethUpActive();
  renamed.name = "eth5";
  renamed.link.speed_mbps = 2500;
  monitor.update(makeSnapshot({renamed}));
  m = monitor.tracked("idx:1");
  CHECK(m != nullptr);
  CHECK(m->name == "eth5");
  CHECK(m->speed_state == NetworkSpeedState::Valid);
  CHECK(m->speed_mbps.has_value() && *m->speed_mbps == 2500);
}

void test_monitor_down_then_disappear() {
  run("MonitorDownThenDisappear");
  atm::NetworkLinkMetricsMonitor monitor;
  monitor.update(makeSnapshot({ethUpActive()}));

  // Link drops but still reports a speed: stale, value preserved.
  monitor.update(makeSnapshot({ethUpDown()}));
  const NetworkLinkMetrics *m = monitor.tracked("idx:1");
  CHECK(m != nullptr);
  CHECK(m->speed_state == NetworkSpeedState::Stale);
  CHECK(m->speed_mbps.has_value() && *m->speed_mbps == 1000);

  // Interface removed: retained record is marked gone (not deleted).
  monitor.update(makeSnapshot({}));
  m = monitor.tracked("idx:1");
  CHECK(m != nullptr);
  CHECK(!m->present);
}

void test_monitor_recreation_resets() {
  run("MonitorRecreationResets");
  atm::NetworkLinkMetricsMonitor monitor;
  monitor.update(makeSnapshot({ethUpActive()}));
  CHECK(monitor.tracked("idx:1") != nullptr);

  // The device is unplugged and a different device reuses the name eth0 with a
  // new ifindex. A new identity starts fresh; the old record is marked gone.
  NetworkInterfaceInfo recreated = ethUpActive();
  recreated.link.ifindex = 7;
  monitor.update(makeSnapshot({recreated}));
  const NetworkLinkMetrics *old = monitor.tracked("idx:1");
  const NetworkLinkMetrics *fresh = monitor.tracked("idx:7");
  CHECK(old != nullptr && !old->present);
  CHECK(fresh != nullptr);
  CHECK(fresh->present);
  CHECK(fresh->name == "eth0");
  CHECK(fresh->speed_state == NetworkSpeedState::Valid);
}

void test_monitor_retains_bound() {
  run("MonitorRetainsBound");
  atm::NetworkLinkMetricsMonitor monitor;
  std::vector<NetworkInterfaceInfo> many;
  for (int i = 1; i <= 70; ++i) {
    many.push_back(makeInfo("veth" + std::to_string(i), i, std::string("up"), 1,
                            kIffUp, 1000, std::string("full"),
                            NetworkInterfaceType::Ethernet));
  }
  monitor.update(makeSnapshot(many));
  // All 70 are live: a live entry is never evicted, so the map keeps them all
  // (mirrors the link-state monitor's eviction semantics).
  CHECK(monitor.entries().size() == 70);

  // All vanish at once: the oldest gone entries are evicted down to the bound.
  monitor.update(makeSnapshot({}));
  CHECK(monitor.entries().size() <=
        atm::NetworkLinkMetricsMonitor::kMaxTrackedLinkMetricsInterfaces);
  for (const auto &kv : monitor.entries()) {
    CHECK(!kv.second.present);
  }
}

void test_monitor_reset() {
  run("MonitorReset");
  atm::NetworkLinkMetricsMonitor monitor;
  monitor.update(makeSnapshot({ethUpActive()}));
  CHECK(monitor.entries().size() == 1);
  monitor.reset();
  CHECK(monitor.entries().empty());
  CHECK(monitor.tracked("idx:1") == nullptr);
}

int main() {
  test_parse_duplex_full_half();
  test_parse_duplex_unknown();
  test_state_names();

  test_fresh_active_link_valid();
  test_fresh_inactive_link_stale();
  test_read_failure_preserves_last_value();
  test_partial_read_duplex_only();
  test_never_reported_unavailable();
  test_virtual_never_meaningful();
  test_loopback_unavailable();

  test_monitor_tracks_rename();
  test_monitor_down_then_disappear();
  test_monitor_recreation_resets();
  test_monitor_retains_bound();
  test_monitor_reset();

  std::fprintf(stderr, "\n%zu checks, %d failures\n", g_checks + 0UL,
               g_failures);
  if (g_failures != 0) {
    std::fprintf(stderr, "RESULT: FAIL\n");
    return 1;
  }
  std::fprintf(stderr, "RESULT: PASS\n");
  return 0;
}