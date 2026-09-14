#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <unistd.h>

#include "network_wireless.hpp"
#include "network_interface_details.hpp"
#include "network_link_state.hpp"
#include "alert_manager.hpp"

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

using atm::NetworkInterfaceInfo;
using atm::NetworkInterfaceSnapshot;
using atm::NetworkInterfaceType;
using atm::NetworkWirelessInfo;
using atm::NetworkWirelessMonitor;
using atm::WirelessAssociation;
using atm::WirelessConnectionEvent;
using atm::WirelessConnectionEventType;
using atm::WirelessConnectionTracker;
using atm::WirelessObservation;
using atm::WirelessPresence;

namespace {

std::chrono::steady_clock::time_point g_now{};
std::chrono::system_clock::time_point g_wall{};

/// One tick: advances both clocks one second and feeds the observation through
/// the tracker with the default debounce (2) and reset gap (30 s).
std::optional<WirelessConnectionEvent> feed(WirelessConnectionTracker &tracker,
                                            WirelessObservation observation,
                                            const std::string &name = "wlan0",
                                            unsigned debounce = 2,
                                            std::chrono::seconds gap =
                                                std::chrono::seconds{30}) {
  g_now += std::chrono::seconds(1);
  g_wall += std::chrono::seconds(1);
  return atm::advanceWirelessConnectionTracker(tracker, std::move(observation),
                                               g_now, g_wall, name, debounce, gap);
}

WirelessObservation present(WirelessAssociation association) {
  WirelessObservation o;
  o.present = true;
  o.wireless = true;
  o.association = association;
  return o;
}

WirelessObservation gone() {
  WirelessObservation o;
  o.present = false;
  o.wireless = false;
  o.association = WirelessAssociation::Unknown;
  return o;
}

/// Presents an associated observation with a reliable AP fingerprint.
WirelessObservation associatedWithFingerprint(std::uint64_t fingerprint) {
  WirelessObservation o = present(WirelessAssociation::Associated);
  o.ap_fingerprint = fingerprint;
  o.ap_identity_reliable = true;
  return o;
}

// ---------------------------------------------------------------------
// Hermetic sysfs /proc helpers (mirror of network_wireless_tests.cpp)
// ---------------------------------------------------------------------

void writeFile(const std::filesystem::path &path, const std::string &content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << content;
}

std::filesystem::path makeIfaceDir(std::filesystem::path root,
                                   const std::string &name) {
  const std::filesystem::path iface = root / "sys" / "class" / "net" / name;
  std::filesystem::create_directories(iface);
  return iface;
}

std::filesystem::path makeWirelessTree(std::filesystem::path root,
                                       const std::string &name) {
  const std::filesystem::path iface = makeIfaceDir(root, name);
  writeFile(iface / "phy80211" / "name", "phy0\n");
  writeFile(iface / "phy80211" / "index", "0\n");
  writeFile(iface / "wireless" / "link", "54\n");
  writeFile(iface / "wireless" / "level", "-42\n");
  writeFile(iface / "wireless" / "noise", "-96\n");
  return root;
}

/// A plain interface directory: no phy80211, no /wireless.
std::filesystem::path makePlainTree(std::filesystem::path root,
                                    const std::string &name) {
  makeIfaceDir(root, name);
  return root;
}

NetworkInterfaceInfo makeInfo(const std::string &name, int ifindex,
                              NetworkInterfaceType type) {
  NetworkInterfaceInfo info;
  info.name = name;
  info.type = type;
  info.link.ifindex = ifindex;
  info.refreshed_at = std::chrono::system_clock::now();
  return info;
}

NetworkInterfaceSnapshot makeSnapshot(
    const std::vector<NetworkInterfaceInfo> &interfaces) {
  NetworkInterfaceSnapshot snapshot;
  snapshot.interfaces = interfaces;
  snapshot.sysfs_readable = true;
  snapshot.addresses_readable = true;
  snapshot.error = atm::NetworkInterfaceError::None;
  snapshot.refreshed_at = std::chrono::system_clock::now();
  return snapshot;
}

/// A wireless "wlan0": ARPHRD_IEEE80211 with a /wireless directory and a
/// carrier-controlled association (1 = associated, 0 = disconnected).
NetworkInterfaceInfo makeWlan(int carrier) {
  NetworkInterfaceInfo wlan = makeInfo("wlan0", 30, NetworkInterfaceType::Wifi);
  wlan.link.link_type = 801u;
  wlan.link.carrier = carrier;
  wlan.wireless.present = true;
  wlan.wireless.level = -42;
  return wlan;
}

// ---------------------------------------------------------------------
// fingerprint / classification / description
// ---------------------------------------------------------------------

void test_ap_fingerprint() {
  run("wireless AP fingerprint");

  CHECK(atm::wirelessApFingerprint("AA:BB:CC:DD:EE:FF") ==
        atm::wirelessApFingerprint("AA:BB:CC:DD:EE:FF"));
  CHECK(atm::wirelessApFingerprint("") == atm::wirelessApFingerprint(""));
  CHECK(atm::wirelessApFingerprint("AA:BB:CC:DD:EE:FF") !=
        atm::wirelessApFingerprint("AA:BB:CC:DD:EE:FE"));
  CHECK(atm::wirelessApFingerprint("ape") != atm::wirelessApFingerprint("epa"));
}

void test_same_state() {
  run("sameWirelessConnectionState");

  WirelessObservation a = present(WirelessAssociation::Associated);
  WirelessObservation b = present(WirelessAssociation::Associated);
  b.signal_dbm = -70;  // signal alone never changes the connection state
  CHECK(atm::sameWirelessConnectionState(a, b));

  b.association = WirelessAssociation::Disconnected;
  CHECK(!atm::sameWirelessConnectionState(a, b));

  WirelessObservation g = gone();
  WirelessObservation h = gone();
  CHECK(atm::sameWirelessConnectionState(g, h));
  g.association = WirelessAssociation::Associated;  // irrelevant while gone
  CHECK(atm::sameWirelessConnectionState(g, h));

  a = associatedWithFingerprint(111);
  b = associatedWithFingerprint(222);  // reliable handoff -> a state change
  CHECK(!atm::sameWirelessConnectionState(a, b));

  b.ap_identity_reliable = false;  // unreliable identity -> never a change
  CHECK(atm::sameWirelessConnectionState(a, b));
}

void test_classify() {
  run("classifyWirelessConnectionChange");

  const WirelessObservation g = gone();
  const WirelessObservation a = associatedWithFingerprint(111);
  const WirelessObservation a_other = associatedWithFingerprint(999);
  const WirelessObservation a_same = associatedWithFingerprint(111);
  const WirelessObservation d = present(WirelessAssociation::Disconnected);
  const WirelessObservation u = present(WirelessAssociation::Unknown);
  const WirelessObservation unavail =
      present(WirelessAssociation::Unavailable);

  CHECK(atm::classifyWirelessConnectionChange(g, g, false) == std::nullopt);
  CHECK(atm::classifyWirelessConnectionChange(g, present(WirelessAssociation::Associated), false) ==
        WirelessConnectionEventType::InterfaceAvailable);
  CHECK(atm::classifyWirelessConnectionChange(a, g, true) ==
        WirelessConnectionEventType::InterfaceUnavailable);

  CHECK(atm::classifyWirelessConnectionChange(a, a_same, true) ==
        std::nullopt);  // same reliable AP, no change
  CHECK(atm::classifyWirelessConnectionChange(a, a_other, true) ==
        WirelessConnectionEventType::Roamed);  // reliable AP handoff

  WirelessObservation a_unreliable = present(WirelessAssociation::Associated);
  WirelessObservation b_unreliable = present(WirelessAssociation::Associated);
  b_unreliable.ap_fingerprint = 7;  // fingerprints differ but unreliable
  CHECK(atm::classifyWirelessConnectionChange(a_unreliable, b_unreliable, true) ==
        std::nullopt);  // freq/signal-only differences never roam

  a_unreliable.ap_identity_reliable = true;
  a_unreliable.ap_fingerprint = 1;
  b_unreliable.ap_identity_reliable = false;
  b_unreliable.ap_fingerprint = 2;  // one side unreliable -> never a roam
  CHECK(atm::classifyWirelessConnectionChange(a_unreliable, b_unreliable, true) ==
        std::nullopt);

  CHECK(atm::classifyWirelessConnectionChange(a, d, true) ==
        WirelessConnectionEventType::Disassociated);
  CHECK(atm::classifyWirelessConnectionChange(d, a, true) ==
        WirelessConnectionEventType::Reconnected);
  CHECK(atm::classifyWirelessConnectionChange(d, a, false) ==
        WirelessConnectionEventType::Associated);  // first-ever association
  CHECK(atm::classifyWirelessConnectionChange(a, u, true) ==
        WirelessConnectionEventType::StateUnknown);
  CHECK(atm::classifyWirelessConnectionChange(a, unavail, true) ==
        WirelessConnectionEventType::InterfaceUnavailable);
}

void test_describe() {
  run("describeWirelessConnectionEvent");

  WirelessConnectionEvent event;
  event.type = WirelessConnectionEventType::Disassociated;
  event.interface_name = "wlan0";
  event.previous_association = WirelessAssociation::Associated;
  event.new_association = WirelessAssociation::Disconnected;
  event.previous_signal_dbm = -42;
  event.new_signal_dbm = -42;
  event.confident = true;
  const std::string line = atm::describeWirelessConnectionEvent(event);
  CHECK(line.find("disassociated") != std::string::npos);
  CHECK(line.find("wlan0") != std::string::npos);
  CHECK(line.find("associated -> disconnected") != std::string::npos);
  CHECK(line.find("signal -42 -> -42 dBm") != std::string::npos);
  CHECK(line.find("(low confidence)") == std::string::npos);
  CHECK(line.find('|') == std::string::npos);

  WirelessConnectionEvent unknown;
  unknown.type = WirelessConnectionEventType::StateUnknown;
  unknown.confident = false;
  CHECK(atm::describeWirelessConnectionEvent(unknown).find(
            "(low confidence)") != std::string::npos);

  WirelessConnectionEvent roam;
  roam.type = WirelessConnectionEventType::Roamed;
  roam.new_association = WirelessAssociation::Associated;
  roam.previous_association = WirelessAssociation::Associated;
  roam.ap_fingerprint_changed = true;
  roam.ap_identity_reliable = true;
  roam.confident = true;
  const std::string rolline = atm::describeWirelessConnectionEvent(roam);
  CHECK(rolline.find("roamed") != std::string::npos);
  CHECK(rolline.find("access point changed") != std::string::npos);

  CHECK(std::string(atm::wirelessConnectionEventTypeName(
            WirelessConnectionEventType::InterfaceAvailable)) ==
        "interface available");
}

// ---------------------------------------------------------------------
// tracker
// ---------------------------------------------------------------------

void test_tracker_baseline_and_debounce() {
  run("tracker baseline silent and debounced");

  WirelessConnectionTracker tracker;

  // First observation establishes the baseline silently.
  CHECK(feed(tracker, present(WirelessAssociation::Associated)) ==
        std::nullopt);

  // Repeated same-state ticks never announce.
  CHECK(feed(tracker, present(WirelessAssociation::Associated)) ==
        std::nullopt);
  WirelessObservation a = present(WirelessAssociation::Associated);
  a.signal_dbm = -50;  // signal-only: still no event
  CHECK(feed(tracker, a) == std::nullopt);

  // A single differing tick is only a candidate (still no event)...
  CHECK(feed(tracker, present(WirelessAssociation::Disconnected)) ==
        std::nullopt);
  // ...a second consecutive differing tick commits it.
  const auto event =
      feed(tracker, present(WirelessAssociation::Disconnected));
  CHECK(event.has_value());
  CHECK(event->type == WirelessConnectionEventType::Disassociated);
  // The next same-state tick does not repeat it.
  CHECK(feed(tracker, present(WirelessAssociation::Disconnected)) ==
        std::nullopt);

  // A transient blip (one differing tick that reverts) is never committed and
  // leaves the committed baseline untouched.
  WirelessConnectionTracker flappy;
  CHECK(feed(flappy, present(WirelessAssociation::Associated)) ==
        std::nullopt);
  CHECK(feed(flappy, present(WirelessAssociation::Disconnected)) ==
        std::nullopt);  // candidate opens
  CHECK(feed(flappy, present(WirelessAssociation::Associated)) ==
        std::nullopt);  // blip reverted: candidate discarded
  // A later real change still works from the untouched baseline.
  CHECK(feed(flappy, present(WirelessAssociation::Disconnected)) ==
        std::nullopt);
  const auto after_flap =
      feed(flappy, present(WirelessAssociation::Disconnected));
  CHECK(after_flap.has_value());
  CHECK(after_flap->type == WirelessConnectionEventType::Disassociated);
  CHECK(after_flap->interface_name == "wlan0");
}

void test_tracker_associate_then_reconnect() {
  run("tracker first association vs reconnection");

  WirelessConnectionTracker tracker;

  // Baseline: unknown/disconnected.
  CHECK(feed(tracker, present(WirelessAssociation::Disconnected)) ==
        std::nullopt);

  // First-ever association -> "associated", never "reconnected".
  CHECK(feed(tracker, present(WirelessAssociation::Associated)) ==
        std::nullopt);
  const auto first_assoc =
      feed(tracker, present(WirelessAssociation::Associated));
  CHECK(first_assoc.has_value());
  CHECK(first_assoc->type == WirelessConnectionEventType::Associated);
  CHECK(first_assoc->previous_association ==
        WirelessAssociation::Disconnected);
  CHECK(first_assoc->new_association == WirelessAssociation::Associated);
  CHECK(first_assoc->confident);

  // Disconnect (2 ticks) then re-associate (2 ticks) -> "reconnected".
  CHECK(feed(tracker, present(WirelessAssociation::Disconnected)) ==
        std::nullopt);
  const auto lost = feed(tracker, present(WirelessAssociation::Disconnected));
  CHECK(lost.has_value());
  CHECK(lost->type == WirelessConnectionEventType::Disassociated);

  CHECK(feed(tracker, present(WirelessAssociation::Associated)) ==
        std::nullopt);
  const auto regained =
      feed(tracker, present(WirelessAssociation::Associated));
  CHECK(regained.has_value());
  CHECK(regained->type == WirelessConnectionEventType::Reconnected);
}

void test_tracker_state_unknown() {
  run("tracker state unknown is a low-confidence event");

  WirelessConnectionTracker tracker;
  CHECK(feed(tracker, present(WirelessAssociation::Associated)) ==
        std::nullopt);
  CHECK(feed(tracker, present(WirelessAssociation::Unknown)) == std::nullopt);
  const auto unknown =
      feed(tracker, present(WirelessAssociation::Unknown));
  CHECK(unknown.has_value());
  CHECK(unknown->type == WirelessConnectionEventType::StateUnknown);
  CHECK(!unknown->confident);
}

void test_tracker_presence_gone_back() {
  run("tracker presence gone and back");

  WirelessConnectionTracker tracker;
  CHECK(feed(tracker, present(WirelessAssociation::Associated)) ==
        std::nullopt);

  // Interface vanishes: interface-unavailable (debounced).
  CHECK(feed(tracker, gone()) == std::nullopt);
  const auto unavailable = feed(tracker, gone());
  CHECK(unavailable.has_value());
  CHECK(unavailable->type ==
        WirelessConnectionEventType::InterfaceUnavailable);
  CHECK(unavailable->previous_association ==
        WirelessAssociation::Associated);
  CHECK(unavailable->new_association == WirelessAssociation::Unavailable);
  CHECK(unavailable->source == "presence");
  CHECK(unavailable->confident);

  // Same vanished state repeats: no second event.
  CHECK(feed(tracker, gone()) == std::nullopt);

  // Interface back: interface-available.
  CHECK(feed(tracker, present(WirelessAssociation::Associated)) ==
        std::nullopt);
  const auto available = feed(tracker, present(WirelessAssociation::Associated));
  CHECK(available.has_value());
  CHECK(available->type == WirelessConnectionEventType::InterfaceAvailable);
}

void test_tracker_roaming() {
  run("tracker roaming only on a reliable AP handoff");

  WirelessConnectionTracker tracker;
  CHECK(feed(tracker, associatedWithFingerprint(111)) == std::nullopt);
  CHECK(feed(tracker, associatedWithFingerprint(112)) == std::nullopt);
  const auto roam = feed(tracker, associatedWithFingerprint(112));
  CHECK(roam.has_value());
  CHECK(roam->type == WirelessConnectionEventType::Roamed);
  CHECK(roam->ap_fingerprint_changed);
  CHECK(roam->ap_identity_reliable);
  CHECK(roam->source == "ap_identity");

  // Frequency/channel differences alone are never a roam.
  WirelessObservation freq_only = associatedWithFingerprint(112);
  freq_only.frequency_mhz = 5180.0;
  freq_only.channel = 36;
  CHECK(feed(tracker, freq_only) == std::nullopt);

  // An unreliable identity is never a roam even when fingerprints differ.
  WirelessObservation unreliable =
      present(WirelessAssociation::Associated);
  unreliable.ap_fingerprint = 777;
  unreliable.ap_identity_reliable = false;
  CHECK(feed(tracker, unreliable) == std::nullopt);
  CHECK(feed(tracker, unreliable) == std::nullopt);
}

void test_tracker_rebaseline_long_gap() {
  run("tracker rebases silently after a long wall-clock gap");

  WirelessConnectionTracker tracker;
  CHECK(feed(tracker, present(WirelessAssociation::Associated)) ==
        std::nullopt);

  // A wall-clock gap longer than the reset gap (simulating suspend) must
  // silently re-baseline instead of announcing a transition.
  g_now += std::chrono::seconds(120);
  g_wall += std::chrono::seconds(120);
  WirelessObservation reconnected =
      present(WirelessAssociation::Associated);
  CHECK(atm::advanceWirelessConnectionTracker(
            tracker, reconnected, g_now, g_wall, "wlan0") == std::nullopt);

  // The new baseline is active: a subsequent real change still announces.
  CHECK(feed(tracker, present(WirelessAssociation::Disconnected)) ==
        std::nullopt);
  const auto event = feed(tracker, present(WirelessAssociation::Disconnected));
  CHECK(event.has_value());
  CHECK(event->type == WirelessConnectionEventType::Disassociated);
}

void test_tracker_debounce_zero() {
  run("tracker debounce 0 commits immediately");

  WirelessConnectionTracker tracker;
  CHECK(feed(tracker, present(WirelessAssociation::Associated)) ==
        std::nullopt);
  const auto event = feed(tracker, present(WirelessAssociation::Disconnected),
                          "wlan0", /*debounce=*/0);
  CHECK(event.has_value());
  CHECK(event->type == WirelessConnectionEventType::Disassociated);
}

// ---------------------------------------------------------------------
// monitor integration
// ---------------------------------------------------------------------

void test_monitor_events_alert_and_sink() {
  run("monitor events feed alert history and the event sink");

  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("atm_wireless_conn_monitor_" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  makeWirelessTree(root, "wlan0");

  atm::AlertManager alerts;
  NetworkWirelessMonitor monitor(alerts, root, std::chrono::seconds(60), 8);

  static int sink_calls = 0;
  static atm::AlertEvent last_sink_event;
  sink_calls = 0;
  last_sink_event = atm::AlertEvent{};
  monitor.setEventSink([](const atm::AlertEvent &event) {
    ++sink_calls;
    last_sink_event = event;
  });

  // Baseline: associated, silent.
  monitor.update(makeSnapshot({makeWlan(1)}));
  CHECK(alerts.history().empty());
  CHECK(sink_calls == 0);

  // Wait a moment so consecutive ticks carry distinct system timestamps.
  std::this_thread::sleep_for(std::chrono::milliseconds(5));

  // Disconnect: debounced, one Warning alert, one sink call.
  monitor.update(makeSnapshot({makeWlan(0)}));
  CHECK(sink_calls == 0);
  monitor.update(makeSnapshot({makeWlan(0)}));
  const NetworkWirelessInfo *w = monitor.tracked(makeWlan(0).identity());
  CHECK(w != nullptr);
  CHECK(w->connection_events.size() == 1);
  CHECK(w->connection_events.samples().front().type ==
        WirelessConnectionEventType::Disassociated);

  CHECK(sink_calls == 1);
  CHECK(last_sink_event.type == atm::AlertType::WirelessConnectionChanged);
  CHECK(last_sink_event.severity == atm::AlertSeverity::Warning);
  CHECK(!last_sink_event.is_recovery);
  CHECK(last_sink_event.source == makeWlan(0).identity());
  CHECK(!alerts.history().empty());
  CHECK(alerts.history().back().severity == atm::AlertSeverity::Warning);
  CHECK(alerts.currentSeverity(atm::AlertType::WirelessConnectionChanged,
                               makeWlan(0).identity()) ==
        atm::AlertSeverity::Warning);

  // Reconnect: a Normal recovery, exactly one more sink call.
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  monitor.update(makeSnapshot({makeWlan(1)}));
  monitor.update(makeSnapshot({makeWlan(1)}));
  CHECK(sink_calls == 2);
  CHECK(last_sink_event.severity == atm::AlertSeverity::Normal);
  CHECK(last_sink_event.is_recovery);

  std::filesystem::remove_all(root);
}

void test_monitor_ring_bounded() {
  run("monitor connection-event ring is bounded");

  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("atm_wireless_conn_ring_" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  makeWirelessTree(root, "wlan0");

  atm::AlertManager alerts;
  NetworkWirelessMonitor monitor(alerts, root, std::chrono::seconds(60), 8);

  // Baseline, then enough debounced transitions to overflow the 64-event ring.
  monitor.update(makeSnapshot({makeWlan(1)}));
  const int transitions = 70;  // > kMaxWirelessConnectionEvents
  for (int i = 0; i < transitions; ++i) {
    const int carrier = (i % 2 == 0) ? 0 : 1;  // toggle association
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    monitor.update(makeSnapshot({makeWlan(carrier)}));
    monitor.update(makeSnapshot({makeWlan(carrier)}));
  }
  const NetworkWirelessInfo *w = monitor.tracked(makeWlan(1).identity());
  CHECK(w != nullptr);
  CHECK(w->connection_events.size() == atm::kMaxWirelessConnectionEvents);
  CHECK(w->connection_events.size() ==
        w->connection_events.maxSamples());  // ring fully wrapped

  std::filesystem::remove_all(root);
}

void test_monitor_non_wireless_clears_events() {
  run("monitor non-wireless clears events and alert subject");

  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("atm_wireless_conn_clear_" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  makeWirelessTree(root, "wlan0");

  atm::AlertManager alerts;
  NetworkWirelessMonitor monitor(alerts, root, std::chrono::seconds(60), 8);

  monitor.update(makeSnapshot({makeWlan(1)}));
  const std::string identity = makeWlan(1).identity();

  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  monitor.update(makeSnapshot({makeWlan(0)}));
  monitor.update(makeSnapshot({makeWlan(0)}));
  CHECK(alerts.currentSeverity(atm::AlertType::WirelessConnectionChanged,
                               identity) == atm::AlertSeverity::Warning);

  // The same identity stops being wireless: rename to a plain interface whose
  // sysfs directory carries no phy80211 or /wireless metadata (the rename also
  // forces a fresh probe of the new path).
  makePlainTree(root, "wlan0-nw");
  NetworkInterfaceInfo wired = makeInfo("wlan0-nw", 30, NetworkInterfaceType::Ethernet);
  wired.link.link_type = 1u;  // ARPHRD_ETHER
  monitor.update(makeSnapshot({wired}));
  monitor.update(makeSnapshot({wired}));

  const NetworkWirelessInfo *w = monitor.tracked(identity);
  CHECK(w != nullptr);
  CHECK(w->presence == WirelessPresence::NotWireless);
  CHECK(w->connection_events.empty());  // nothing carried into a wired link
  CHECK(alerts.currentSeverity(atm::AlertType::WirelessConnectionChanged,
                               identity) == atm::AlertSeverity::Normal);

  std::filesystem::remove_all(root);
}

}  // namespace

int main() {
  test_ap_fingerprint();
  test_same_state();
  test_classify();
  test_describe();
  test_tracker_baseline_and_debounce();
  test_tracker_associate_then_reconnect();
  test_tracker_state_unknown();
  test_tracker_presence_gone_back();
  test_tracker_roaming();
  test_tracker_rebaseline_long_gap();
  test_tracker_debounce_zero();
  test_monitor_events_alert_and_sink();
  test_monitor_ring_bounded();
  test_monitor_non_wireless_clears_events();

  std::fprintf(stderr, "PASS: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}