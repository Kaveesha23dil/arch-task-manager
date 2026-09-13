#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "alert_manager.hpp"
#include "network_interface_details.hpp"
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

using atm::NetworkCarrierState;
using atm::NetworkAdminState;
using atm::NetworkInterfaceInfo;
using atm::NetworkInterfaceSnapshot;
using atm::NetworkInterfaceType;
using atm::NetworkLinkAvailability;
using atm::NetworkLinkEventType;
using atm::NetworkOperState;

constexpr unsigned kIffUp = 0x1u;

/// Builds one interface-discovery record. `flags` drives both the raw flags
/// and the derived `admin_up` (IFF_UP).
NetworkInterfaceInfo makeInfo(
    const std::string &name, int ifindex,
    const std::optional<std::string> &operstate,
    const std::optional<int> &carrier,
    const std::optional<unsigned> &flags,
    NetworkInterfaceType type) {
  NetworkInterfaceInfo info;
  info.name = name;
  info.type = type;
  info.link.ifindex = ifindex;
  info.link.operstate = operstate;
  info.link.carrier = carrier;
  info.link.flags = flags;
  info.link.admin_up = flags.has_value() ? ((*flags & kIffUp) != 0) : false;
  info.refreshed_at = std::chrono::system_clock::now();
  return info;
}

/// A physical Ethernet interface that is operationally up with carrier.
NetworkInterfaceInfo ethUp() {
  return makeInfo("eth0", 1, std::string("up"), 1, kIffUp,
                  NetworkInterfaceType::Ethernet);
}

/// The same physical Ethernet interface, administratively up but link down.
NetworkInterfaceInfo ethDown() {
  return makeInfo("eth0", 1, std::string("down"), 0, kIffUp,
                  NetworkInterfaceType::Ethernet);
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

std::size_t linkHistoryCount(const atm::NetworkLinkStateMonitor &monitor,
                             NetworkLinkEventType type) {
  std::size_t count = 0;
  for (const atm::NetworkLinkStateEvent &event : monitor.history().samples()) {
    if (event.type == type) {
      ++count;
    }
  }
  return count;
}

std::size_t alertCount(const atm::AlertManager &alerts) {
  std::size_t count = 0;
  for (const atm::AlertEvent &event : alerts.history()) {
    if (event.type == atm::AlertType::LinkStateChanged) {
      ++count;
    }
  }
  return count;
}

// --- Notification sink capture -------------------------------------------
namespace {
int g_sink_calls = 0;
atm::AlertEvent g_last_sink_event;

void sink(const atm::AlertEvent &event) {
  ++g_sink_calls;
  g_last_sink_event = event;
}

void resetSink() {
  g_sink_calls = 0;
  g_last_sink_event = atm::AlertEvent{};
}
}  // namespace

// -------------------------------------------------------------------------
// Parsing / naming / derivation
// -------------------------------------------------------------------------

void testNameMappings() {
  run("names: enum -> string");
  CHECK(std::string(atm::networkOperStateName(NetworkOperState::Up)) == "up");
  CHECK(std::string(atm::networkOperStateName(NetworkOperState::Unavailable)) ==
        "unavailable");
  CHECK(std::string(atm::networkCarrierStateName(NetworkCarrierState::Carrier)) ==
        "yes");
  CHECK(std::string(atm::networkCarrierStateName(NetworkCarrierState::NoCarrier)) ==
        "no carrier");
  CHECK(std::string(atm::networkAdminStateName(NetworkAdminState::AdminUp)) ==
        "enabled");
  CHECK(std::string(atm::networkAdminStateName(NetworkAdminState::AdminDown)) ==
        "disabled");
  CHECK(std::string(atm::networkLinkAvailabilityName(
            NetworkLinkAvailability::Connected)) == "Connected");
  CHECK(std::string(atm::networkLinkAvailabilityName(
            NetworkLinkAvailability::Unavailable)) == "Unavailable");
}

void testParseOperState() {
  run("parse: operstate strings");
  CHECK(atm::parseOperState(std::optional<std::string>("up")) ==
        NetworkOperState::Up);
  CHECK(atm::parseOperState(std::optional<std::string>("down")) ==
        NetworkOperState::Down);
  CHECK(atm::parseOperState(std::optional<std::string>("dormant")) ==
        NetworkOperState::Dormant);
  CHECK(atm::parseOperState(std::optional<std::string>("lowerlayerdown")) ==
        NetworkOperState::LowerLayerDown);
  CHECK(atm::parseOperState(std::optional<std::string>("testing")) ==
        NetworkOperState::Testing);
  CHECK(atm::parseOperState(std::optional<std::string>("notpresent")) ==
        NetworkOperState::NotPresent);
  // Unknown and unrecognised values are never treated as "down".
  CHECK(atm::parseOperState(std::optional<std::string>("unknown")) ==
        NetworkOperState::Unknown);
  CHECK(atm::parseOperState(std::optional<std::string>("bogus")) ==
        NetworkOperState::Unknown);
  CHECK(atm::parseOperState(std::nullopt) == NetworkOperState::Unknown);
}

void testParseCarrierAdmin() {
  run("parse: carrier and admin");
  CHECK(atm::parseCarrierState(std::optional<int>(1)) ==
        NetworkCarrierState::Carrier);
  CHECK(atm::parseCarrierState(std::optional<int>(0)) ==
        NetworkCarrierState::NoCarrier);
  CHECK(atm::parseCarrierState(std::optional<int>(-1)) ==
        NetworkCarrierState::NoCarrier);
  CHECK(atm::parseCarrierState(std::nullopt) == NetworkCarrierState::Unknown);

  CHECK(atm::parseAdminState(std::optional<unsigned>(kIffUp), true) ==
        NetworkAdminState::AdminUp);
  CHECK(atm::parseAdminState(std::optional<unsigned>(0u), false) ==
        NetworkAdminState::AdminDown);
  CHECK(atm::parseAdminState(std::nullopt, false) ==
        NetworkAdminState::Unknown);
}

void testLinkAvailability() {
  run("derive: link availability");
  // Unreadable is never "down".
  CHECK(atm::linkAvailability(false, NetworkOperState::Up,
                              NetworkCarrierState::Carrier,
                              NetworkAdminState::AdminUp) ==
        NetworkLinkAvailability::Unavailable);
  // Kernel "up" is connected.
  CHECK(atm::linkAvailability(true, NetworkOperState::Up,
                              NetworkCarrierState::Carrier,
                              NetworkAdminState::AdminUp) ==
        NetworkLinkAvailability::Connected);
  CHECK(atm::linkAvailability(true, NetworkOperState::Up,
                              NetworkCarrierState::NoCarrier,
                              NetworkAdminState::AdminUp) ==
        NetworkLinkAvailability::Connected);
  // Device not present on the bus -> unavailable, not down.
  CHECK(atm::linkAvailability(true, NetworkOperState::NotPresent,
                              NetworkCarrierState::NoCarrier,
                              NetworkAdminState::AdminUp) ==
        NetworkLinkAvailability::Unavailable);
  // A driver that only reports carrier signals a live link (unless disabled).
  CHECK(atm::linkAvailability(true, NetworkOperState::Unknown,
                              NetworkCarrierState::Carrier,
                              NetworkAdminState::AdminUp) ==
        NetworkLinkAvailability::Connected);
  CHECK(atm::linkAvailability(true, NetworkOperState::Unknown,
                              NetworkCarrierState::Carrier,
                              NetworkAdminState::AdminDown) ==
        NetworkLinkAvailability::Down);
  // No reported state yet at all.
  CHECK(atm::linkAvailability(true, NetworkOperState::Unknown,
                              NetworkCarrierState::Unknown,
                              NetworkAdminState::Unknown) ==
        NetworkLinkAvailability::Unknown);
  // Anything else is down.
  CHECK(atm::linkAvailability(true, NetworkOperState::Down,
                              NetworkCarrierState::NoCarrier,
                              NetworkAdminState::AdminUp) ==
        NetworkLinkAvailability::Down);
  CHECK(atm::linkAvailability(true, NetworkOperState::Dormant,
                              NetworkCarrierState::NoCarrier,
                              NetworkAdminState::AdminUp) ==
        NetworkLinkAvailability::Down);
}

void testPhysicalClassification() {
  run("classify: physical link types only");
  CHECK(atm::isPhysicalNetworkLink(NetworkInterfaceType::Ethernet));
  CHECK(atm::isPhysicalNetworkLink(NetworkInterfaceType::Wifi));
  CHECK(atm::isPhysicalNetworkLink(NetworkInterfaceType::InfiniBand));
  CHECK(!atm::isPhysicalNetworkLink(NetworkInterfaceType::Loopback));
  CHECK(!atm::isPhysicalNetworkLink(NetworkInterfaceType::Bridge));
  CHECK(!atm::isPhysicalNetworkLink(NetworkInterfaceType::Bond));
  CHECK(!atm::isPhysicalNetworkLink(NetworkInterfaceType::Tunnel));
  CHECK(!atm::isPhysicalNetworkLink(NetworkInterfaceType::Virtual));
  CHECK(!atm::isPhysicalNetworkLink(NetworkInterfaceType::P2P));
  CHECK(!atm::isPhysicalNetworkLink(NetworkInterfaceType::Unknown));
}

// -------------------------------------------------------------------------
// Monitor behavior
// -------------------------------------------------------------------------

void testInitialBaselineNoAlerts() {
  run("monitor: first tick records a silent baseline");
  atm::AlertManager alerts;
  atm::NetworkLinkStateMonitor monitor(alerts);
  resetSink();
  monitor.setEventSink(&sink);

  monitor.update(makeSnapshot({ethUp(), makeInfo("lo", 0, std::string("unknown"), 1, kIffUp,
                                                 NetworkInterfaceType::Loopback)}));

  CHECK(linkHistoryCount(monitor, NetworkLinkEventType::Appeared) == 2);
  CHECK(monitor.history().samples().size() == 2);
  CHECK(alertCount(alerts) == 0);
  CHECK(alerts.currentSeverity(atm::AlertType::LinkStateChanged, "idx:1") ==
        atm::AlertSeverity::Normal);
  CHECK(g_sink_calls == 0);

  const atm::TrackedInterface *t = monitor.tracked("idx:1");
  CHECK(t != nullptr);
  CHECK(t->has_valid_state);
  CHECK(t->availability() == NetworkLinkAvailability::Connected);
  CHECK(t->transition_count == 0);

  // An identical second tick produces no events.
  monitor.update(makeSnapshot({ethUp(), makeInfo("lo", 0, std::string("unknown"), 1, kIffUp,
                                                 NetworkInterfaceType::Loopback)}));
  CHECK(monitor.history().samples().size() == 2);
}

void testCountsExcludeVirtualAndLoopback() {
  run("counts: physical only, virtual/loopback excluded");
  atm::AlertManager alerts;
  atm::NetworkLinkStateMonitor monitor(alerts);
  monitor.update(makeSnapshot({
      ethUp(),  // idx:1 connected
      makeInfo("wlan0", 2, std::string("up"), 1, kIffUp,
               NetworkInterfaceType::Wifi),  // idx:2 connected
      makeInfo("enp3s0", 3, std::string("down"), 0, kIffUp,
               NetworkInterfaceType::Ethernet),  // idx:3 down
      makeInfo("physX", 4, std::nullopt, std::nullopt, std::nullopt,
               NetworkInterfaceType::Ethernet),  // idx:4 unavailable
      makeInfo("lo", 0, std::string("unknown"), 1, kIffUp,
               NetworkInterfaceType::Loopback),   // excluded
      makeInfo("docker0", 50, std::string("up"), 1, kIffUp,
               NetworkInterfaceType::Virtual),  // excluded
  }));

  const atm::NetworkLinkStateCounts &counts = monitor.counts();
  CHECK(counts.physical_total == 4);
  CHECK(counts.connected == 2);
  CHECK(counts.with_carrier == 2);
  CHECK(counts.down == 1);
  CHECK(counts.unavailable == 1);
}

void testDownTransitionAlerts() {
  run("monitor: up -> down fires a Warning alert");
  atm::AlertManager alerts;
  atm::NetworkLinkStateMonitor monitor(alerts);
  resetSink();
  monitor.setEventSink(&sink);

  monitor.update(makeSnapshot({ethUp()}));
  CHECK(alertCount(alerts) == 0);

  monitor.update(makeSnapshot({ethDown()}));

  CHECK(linkHistoryCount(monitor, NetworkLinkEventType::StateChanged) == 1);
  CHECK(linkHistoryCount(monitor, NetworkLinkEventType::Appeared) == 1);
  const atm::NetworkLinkStateEvent &event = monitor.history().samples().back();
  CHECK(event.identity == "idx:1");
  CHECK(event.previous_oper == NetworkOperState::Up);
  CHECK(event.new_oper == NetworkOperState::Down);
  CHECK(event.previous_carrier == NetworkCarrierState::Carrier);
  CHECK(event.new_carrier == NetworkCarrierState::NoCarrier);
  CHECK(!event.is_recovery);
  CHECK(event.reason.find("up -> down") != std::string::npos);
  CHECK(event.reason.find("carrier lost") != std::string::npos);
  CHECK(event.transition_count == 1);

  const atm::TrackedInterface *t = monitor.tracked("idx:1");
  CHECK(t != nullptr);
  CHECK(t->availability() == NetworkLinkAvailability::Down);

  // Central subject is active and history has one non-recovery Warning.
  CHECK(alertCount(alerts) == 1);
  CHECK(alerts.currentSeverity(atm::AlertType::LinkStateChanged, "idx:1") ==
        atm::AlertSeverity::Warning);
  CHECK(!alerts.history().back().is_recovery);
  CHECK(alerts.history().back().severity == atm::AlertSeverity::Warning);

  // The desktop sink received the same warning.
  CHECK(g_sink_calls == 1);
  CHECK(g_last_sink_event.severity == atm::AlertSeverity::Warning);
  CHECK(!g_last_sink_event.is_recovery);
  CHECK(g_last_sink_event.source == "idx:1");
  CHECK(g_last_sink_event.message.find("eth0 link lost") !=
        std::string::npos);
}

void testRecoveryTransitionAlerts() {
  run("monitor: down -> up fires a recovery");
  atm::AlertManager alerts;
  atm::NetworkLinkStateMonitor monitor(alerts);
  resetSink();
  monitor.setEventSink(&sink);

  monitor.update(makeSnapshot({ethUp()}));
  monitor.update(makeSnapshot({ethDown()}));
  CHECK(alertCount(alerts) == 1);

  monitor.update(makeSnapshot({ethUp()}));

  CHECK(linkHistoryCount(monitor, NetworkLinkEventType::StateChanged) == 2);
  const atm::NetworkLinkStateEvent &event = monitor.history().samples().back();
  CHECK(event.is_recovery);
  CHECK(event.reason.find("down -> up") != std::string::npos);

  CHECK(alertCount(alerts) == 2);
  CHECK(alerts.history().back().is_recovery);
  CHECK(alerts.history().back().severity == atm::AlertSeverity::Normal);
  CHECK(alerts.currentSeverity(atm::AlertType::LinkStateChanged, "idx:1") ==
        atm::AlertSeverity::Normal);

  CHECK(g_sink_calls == 2);
  CHECK(g_last_sink_event.is_recovery);
  CHECK(g_last_sink_event.severity == atm::AlertSeverity::Normal);
}

void testCarrierLossWhileUpAlerts() {
  run("monitor: carrier lost while operstate up alerts");
  atm::AlertManager alerts;
  atm::NetworkLinkStateMonitor monitor(alerts);
  resetSink();
  monitor.setEventSink(&sink);

  monitor.update(makeSnapshot({ethUp()}));
  monitor.update(makeSnapshot({
      makeInfo("eth0", 1, std::string("up"), 0, kIffUp,
               NetworkInterfaceType::Ethernet)}));

  CHECK(linkHistoryCount(monitor, NetworkLinkEventType::StateChanged) == 1);
  CHECK(monitor.history().samples().back().reason.find("carrier lost") !=
        std::string::npos);
  CHECK(alertCount(alerts) == 1);
  CHECK(alerts.history().back().severity == atm::AlertSeverity::Warning);
  CHECK(g_sink_calls == 1);
}

void testReadFailureKeepsStateNoEvent() {
  run("monitor: temporary read failure preserves state, no alert");
  atm::AlertManager alerts;
  atm::NetworkLinkStateMonitor monitor(alerts);
  resetSink();
  monitor.setEventSink(&sink);

  monitor.update(makeSnapshot({ethUp()}));

  // Metadata becomes unreadable for a tick (no operstate/carrier/flags).
  monitor.update(makeSnapshot({
      makeInfo("eth0", 1, std::nullopt, std::nullopt, std::nullopt,
               NetworkInterfaceType::Ethernet)}));

  CHECK(monitor.history().samples().size() == 1);  // no new event
  CHECK(alertCount(alerts) == 0);
  CHECK(g_sink_calls == 0);
  const atm::TrackedInterface *t = monitor.tracked("idx:1");
  CHECK(t != nullptr);
  CHECK(t->availability() == NetworkLinkAvailability::Unavailable);
  CHECK(t->oper == NetworkOperState::Up);   // last valid state preserved
  CHECK(t->carrier == NetworkCarrierState::Carrier);
  CHECK(monitor.counts().unavailable == 1);

  // Read restores the identical state: still no event.
  monitor.update(makeSnapshot({ethUp()}));
  CHECK(monitor.history().samples().size() == 1);
  CHECK(alertCount(alerts) == 0);
  CHECK(monitor.counts().connected == 1);
}

void testRemovalAndReappearance() {
  run("monitor: removal is silent, re-plug is a recovery");
  atm::AlertManager alerts;
  atm::NetworkLinkStateMonitor monitor(alerts);
  resetSink();
  monitor.setEventSink(&sink);

  monitor.update(makeSnapshot({ethUp()}));

  // Removal: history event only, no alert, subject cleared.
  monitor.update(makeSnapshot({}));
  CHECK(linkHistoryCount(monitor, NetworkLinkEventType::Disappeared) == 1);
  CHECK(alertCount(alerts) == 0);
  CHECK(g_sink_calls == 0);
  CHECK(alerts.currentSeverity(atm::AlertType::LinkStateChanged, "idx:1") ==
        atm::AlertSeverity::Normal);
  const atm::TrackedInterface *gone = monitor.tracked("idx:1");
  CHECK(gone != nullptr);
  CHECK(!gone->present);
  CHECK(monitor.counts().physical_total == 0);

  // Re-plug: recovery event + recovery notification, state reset in place.
  resetSink();
  monitor.update(makeSnapshot({ethUp()}));
  CHECK(linkHistoryCount(monitor, NetworkLinkEventType::Reappeared) == 1);
  CHECK(alerts.currentSeverity(atm::AlertType::LinkStateChanged, "idx:1") ==
        atm::AlertSeverity::Normal);
  CHECK(g_sink_calls == 1);
  CHECK(g_last_sink_event.is_recovery);
  CHECK(g_last_sink_event.severity == atm::AlertSeverity::Normal);
  const atm::TrackedInterface *back = monitor.tracked("idx:1");
  CHECK(back != nullptr);
  CHECK(back->present);
  CHECK(back->transition_count == 2);
  CHECK(back->has_valid_state);
}

void testRenameKeepsIdentity() {
  run("monitor: rename keeps identity and history");
  atm::AlertManager alerts;
  atm::NetworkLinkStateMonitor monitor(alerts);

  monitor.update(makeSnapshot({ethUp()}));
  // Same ifindex, new kernel name, identical link state: no event.
  monitor.update(makeSnapshot({
      makeInfo("enp0s3", 1, std::string("up"), 1, kIffUp,
               NetworkInterfaceType::Ethernet)}));

  CHECK(monitor.history().samples().size() == 1);
  CHECK(linkHistoryCount(monitor, NetworkLinkEventType::StateChanged) == 0);
  const atm::TrackedInterface *t = monitor.tracked("idx:1");
  CHECK(t != nullptr);
  CHECK(t->name == "enp0s3");
  CHECK(monitor.counts().physical_total == 1);
  CHECK(monitor.counts().connected == 1);
}

void testBoundedHistory() {
  run("monitor: event history is bounded");
  atm::AlertManager alerts;
  atm::NetworkLinkStateMonitor monitor(alerts, 3);

  monitor.update(makeSnapshot({ethUp()}));   // Appeared
  monitor.update(makeSnapshot({ethDown()}));  // StateChanged 1
  monitor.update(makeSnapshot({ethUp()}));    // StateChanged 2
  monitor.update(makeSnapshot({ethDown()}));  // StateChanged 3
  monitor.update(makeSnapshot({ethUp()}));    // StateChanged 4
  monitor.update(makeSnapshot({ethDown()}));  // StateChanged 5

  CHECK(monitor.history().samples().size() == 3);
  CHECK(linkHistoryCount(monitor, NetworkLinkEventType::Appeared) == 0);  // evicted
  CHECK(linkHistoryCount(monitor, NetworkLinkEventType::StateChanged) == 3);
}

int main() {
  testNameMappings();
  testParseOperState();
  testParseCarrierAdmin();
  testLinkAvailability();
  testPhysicalClassification();
  testInitialBaselineNoAlerts();
  testCountsExcludeVirtualAndLoopback();
  testDownTransitionAlerts();
  testRecoveryTransitionAlerts();
  testCarrierLossWhileUpAlerts();
  testReadFailureKeepsStateNoEvent();
  testRemovalAndReappearance();
  testRenameKeepsIdentity();
  testBoundedHistory();

  std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}