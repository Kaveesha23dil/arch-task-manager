#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "network_wireless.hpp"
#include "network_interface_details.hpp"
#include "network_link_state.hpp"

// One shared AlertManager for every monitor constructed in these tests
// (NetworkWirelessMonitor requires the alert history it records into).
atm::AlertManager g_wireless_alerts;

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
using atm::ProcWirelessStats;
using atm::WirelessAssociation;
using atm::WirelessFieldState;
using atm::WirelessPresence;
using atm::WirelessProbe;

// -------------------------------------------------------------------------
// Helpers: hermetic sysfs /proc trees
// -------------------------------------------------------------------------

namespace {

/// Writes a small UTF-8 text file; helper for building fake sysfs attributes.
void writeFile(const std::filesystem::path &path, const std::string &content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << content;
}

/// Builds an interface directory under <root>/sys/class/net/<name>.
std::filesystem::path makeIfaceDir(std::filesystem::path root,
                                   const std::string &name) {
  const std::filesystem::path iface =
      root / "sys" / "class" / "net" / name;
  std::filesystem::create_directories(iface);
  return iface;
}

/// A mac80211-style wireless interface: a phy80211 directory (name + index)
/// and a legacy /wireless directory with WE quality/signal/noise values.
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

/// A plain (wired) interface: no phy80211, no /wireless.
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
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
  NetworkInterfaceSnapshot snapshot;
  snapshot.interfaces = interfaces;
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
  snapshot.sysfs_readable = true;
  snapshot.addresses_readable = true;
  snapshot.error = atm::NetworkInterfaceError::None;
  snapshot.refreshed_at = std::chrono::system_clock::now();
  return snapshot;
}

}  // namespace

// -------------------------------------------------------------------------
// Classification: reliable native signals only
// -------------------------------------------------------------------------

void test_classification_strict() {
  run("classification strict");

  // A wireless-sounding NAME alone is never enough: wlan0 with an Ethernet
  // ARPHRD type and no wireless sysfs metadata is NotWireless.
  NetworkInterfaceInfo eth_wlan = makeInfo("wlan0", 1, NetworkInterfaceType::Wifi);
  eth_wlan.link.link_type = 1u;  // ARPHRD_ETHER
  WirelessProbe empty_probe;
  NetworkWirelessInfo judged = atm::judgeWireless(eth_wlan, empty_probe, {}, {});
  CHECK(judged.presence == WirelessPresence::NotWireless);
  CHECK(judged.signal_dbm.has_value() == false);   // no invented signal
  CHECK(judged.link_quality.has_value() == false);

  // An IEEE80211 ARPHRD type is conclusive even with no directories.
  NetworkInterfaceInfo arphrd = makeInfo("enp3s0", 2, NetworkInterfaceType::Ethernet);
  arphrd.link.link_type = 801u;
  NetworkWirelessInfo by_arphrd = atm::judgeWireless(arphrd, {}, {}, {});
  CHECK(by_arphrd.presence == WirelessPresence::Wireless);

  // The legacy /wireless directory presence from the snapshot is conclusive.
  NetworkInterfaceInfo we = makeInfo("foo0", 3, NetworkInterfaceType::Ethernet);
  we.wireless.present = true;
  NetworkWirelessInfo by_we = atm::judgeWireless(we, {}, {}, {});
  CHECK(by_we.presence == WirelessPresence::Wireless);

  // phy80211 presence (from the probe) is conclusive.
  WirelessProbe phy_probe;
  phy_probe.phy80211_present = true;
  NetworkInterfaceInfo plain = makeInfo("eth1", 4, NetworkInterfaceType::Ethernet);
  NetworkWirelessInfo by_phy = atm::judgeWireless(plain, phy_probe, {}, {});
  CHECK(by_phy.presence == WirelessPresence::Wireless);
  CHECK(by_phy.is_mac80211);

  // Loopback is never wireless, even with a carrier/up operstate.
  NetworkInterfaceInfo lo = makeInfo("lo", 5, NetworkInterfaceType::Loopback);
  lo.link.link_type = 772u;  // ARPHRD_LOOPBACK
  lo.link.carrier = 1;
  NetworkWirelessInfo by_lo = atm::judgeWireless(lo, {}, {}, {});
  CHECK(by_lo.presence == WirelessPresence::NotWireless);
  CHECK(by_lo.association == WirelessAssociation::Unknown);

  // A virtual (veth/dummy) interface is never classified wireless either.
  NetworkInterfaceInfo veth =
      makeInfo("veth9", 6, NetworkInterfaceType::Virtual);
  veth.link.link_type = 1u;  // ARPHRD_ETHER
  NetworkWirelessInfo by_veth = atm::judgeWireless(veth, {}, {}, {});
  CHECK(by_veth.presence == WirelessPresence::NotWireless);
  CHECK(!by_veth.signal_dbm.has_value());
}

void test_read_wireless_probe() {
  run("readWirelessProbe");

  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("atm_wireless_probe_" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);

  makeWirelessTree(root, "wlan0");

  const WirelessProbe probe = atm::readWirelessProbe(root, "wlan0");
  CHECK(probe.probe_ok);
  CHECK(probe.phy80211_present);
  CHECK(probe.wireless_dir_present);
  CHECK(probe.metadata_available);
  CHECK(probe.phy_name == "phy0");
  CHECK(probe.phy_index.has_value());
  CHECK(*probe.phy_index == 0);

  // An unreadable phy index does not stop the rest of the probe.
  writeFile(root / "sys" / "class" / "net" / "wlan0" / "phy80211" / "index",
            "not-a-number\n");
  const WirelessProbe bad = atm::readWirelessProbe(root, "wlan0");
  CHECK(bad.phy80211_present);
  CHECK(!bad.phy_index.has_value());  // invalid -> no value, never 0
  writeFile(root / "sys" / "class" / "net" / "wlan0" / "phy80211" / "index",
            "-1\n");
  const WirelessProbe sentinel = atm::readWirelessProbe(root, "wlan0");
  CHECK(sentinel.phy80211_present);
  CHECK(!sentinel.phy_index.has_value());  // "-1" is the kernel's "no value"

  // A plain interface probes cleanly with no wireless metadata.
  makePlainTree(root, "enp3s0");
  const WirelessProbe plain = atm::readWirelessProbe(root, "enp3s0");
  CHECK(plain.probe_ok);
  CHECK(!plain.phy80211_present);
  CHECK(!plain.wireless_dir_present);
  CHECK(!plain.metadata_available);
  CHECK(plain.phy_name.empty());

  std::filesystem::remove_all(root);
}

void test_proc_net_wireless_parsing() {
  run("parseProcNetWirelessStats");

  const std::string sample =
      "Inter-| sta-|   Quality  |   Discarded packets               | Missed | "
      "WE\n"
      " eth0     0000  70.  -40.  -256.  0      0      0      0      0      0  "
      "      0   54\n"
      " wlan0    0000  54.  -42.  -96.   0      0      0      0      0      0  "
      "      0   54\n";

  const ProcWirelessStats wlan = atm::parseProcNetWirelessStats(sample, "wlan0");
  CHECK(wlan.present);
  CHECK(wlan.level_dbm.has_value());
  CHECK(*wlan.level_dbm == -42);
  CHECK(wlan.noise_dbm.has_value());
  CHECK(*wlan.noise_dbm == -96);

  const ProcWirelessStats eth = atm::parseProcNetWirelessStats(sample, "eth0");
  CHECK(eth.present);
  CHECK(eth.level_dbm.has_value());
  CHECK(*eth.level_dbm == -40);

  // Unrelated interface: absent, not a fabricated zero.
  const ProcWirelessStats other =
      atm::parseProcNetWirelessStats(sample, "wlan9");
  CHECK(!other.present);
  CHECK(!other.level_dbm.has_value());

  // Whitespace-padded and short/invalid lines never crash and never leak. The
  // "0000" status column is the real kernel column reported between the face
  // name and the link (qual) value.
  const std::string messy =
      " Inter-| sta-|   Quality  |   Discarded\n"
      " wlan0     0000   54.  -42.  -96.  0 0 0\n"
      "garbage\n"
      "  eth7     0000   12.  junk.  -10.\n";
  const ProcWirelessStats wlan2 = atm::parseProcNetWirelessStats(messy, "wlan0");
  CHECK(wlan2.present);
  CHECK(wlan2.level_dbm.has_value());
  CHECK(*wlan2.level_dbm == -42);
  CHECK(wlan2.noise_dbm.has_value());
  CHECK(*wlan2.noise_dbm == -96);
  const ProcWirelessStats junk = atm::parseProcNetWirelessStats(messy, "eth7");
  CHECK(junk.present);
  CHECK(!junk.level_dbm.has_value());  // invalid -> no value, never 0
  CHECK(junk.noise_dbm.has_value());
  CHECK(*junk.noise_dbm == -10);

  // A kernel-reported zero measurement is preserved as a number (it was read,
  // not fabricated); a missing column stays absent.
  const std::string zero_line =
      " usb0 0000 0. 0. -256. 0 0 0 0 0 0 0 30\n";
  const ProcWirelessStats zero_value =
      atm::parseProcNetWirelessStats(zero_line, "usb0");
  CHECK(zero_value.present);
  CHECK(zero_value.level_dbm.has_value());
  CHECK(*zero_value.level_dbm == 0);

  const std::string empty = "   \n\n  \n";
  CHECK(!atm::parseProcNetWirelessStats(empty, "wlan0").present);

  // The kernel's "invalid measurement" sentinel (-256) means "no noise floor
  // reported" — never a plausible -256 dBm reading.
  const std::string no_noise =
      " wlan3 0000 54. -47. -256. 0 0 0 0 0 0 0 54\n";
  const ProcWirelessStats no_noise_stats =
      atm::parseProcNetWirelessStats(no_noise, "wlan3");
  CHECK(no_noise_stats.present);
  CHECK(no_noise_stats.level_dbm.has_value());
  CHECK(*no_noise_stats.level_dbm == -47);
  CHECK(!no_noise_stats.noise_dbm.has_value());
}

// -------------------------------------------------------------------------
// judgeWireless: association and value freshness
// -------------------------------------------------------------------------

void test_association() {
  run("judgeWireless association");

  WirelessProbe probe;
  probe.phy80211_present = true;

  // Carrier 1 -> associated (mac80211 raises the carrier on association).
  NetworkInterfaceInfo info = makeInfo("wlan0", 10, NetworkInterfaceType::Wifi);
  info.link.link_type = 801u;
  info.link.carrier = 1;
  NetworkWirelessInfo assoc = atm::judgeWireless(info, probe, {}, {});
  CHECK(assoc.presence == WirelessPresence::Wireless);
  CHECK(assoc.association == WirelessAssociation::Associated);
  CHECK(assoc.has_carrier);
  CHECK(assoc.carrier_exposed);

  // Carrier 0 -> disconnected.
  info.link.carrier = 0;
  NetworkWirelessInfo disc = atm::judgeWireless(info, probe, {}, {});
  CHECK(disc.association == WirelessAssociation::Disconnected);
  CHECK(!disc.has_carrier);

  // NO carrier file: "operstate up" must NOT imply association.
  info.link.carrier.reset();
  info.link.operstate = "up";
  NetworkWirelessInfo unknown = atm::judgeWireless(info, probe, {}, {});
  CHECK(unknown.association == WirelessAssociation::Unknown);

  info.link.operstate = "down";
  NetworkWirelessInfo unknown2 = atm::judgeWireless(info, probe, {}, {});
  CHECK(unknown2.association == WirelessAssociation::Unknown);

  // A non-wireless interface is never "associated".
  NetworkInterfaceInfo wired = makeInfo("eth0", 11, NetworkInterfaceType::Ethernet);
  wired.link.link_type = 1u;
  wired.link.carrier = 1;
  NetworkWirelessInfo wired_r = atm::judgeWireless(wired, {}, {}, {});
  CHECK(wired_r.presence == WirelessPresence::NotWireless);
  CHECK(wired_r.association == WirelessAssociation::Unknown);
}

void test_dynamic_field_freshness() {
  run("dynamic field freshness");

  WirelessProbe probe;
  probe.phy80211_present = true;

  // Available on first read; negative dBm preserved, never absolute-valued.
  NetworkInterfaceInfo info = makeInfo("wlan0", 20, NetworkInterfaceType::Wifi);
  info.link.link_type = 801u;
  info.wireless.present = true;
  info.wireless.link = 54;
  info.wireless.level = -42;
  info.wireless.noise = -96;
  NetworkWirelessInfo fresh = atm::judgeWireless(info, probe, {}, {});
  CHECK(fresh.field_state == WirelessFieldState::Available);
  CHECK(fresh.link_quality.has_value());
  CHECK(*fresh.link_quality == 54);
  CHECK(fresh.signal_dbm.has_value());
  CHECK(*fresh.signal_dbm == -42);
  CHECK(fresh.noise_dbm.has_value());
  CHECK(*fresh.noise_dbm == -96);

  // Clean absence (driver exposes no WIRELESS_EXT stats): unavailable, and no
  // misleading zero is manufactured.
  NetworkInterfaceInfo bare = makeInfo("wlan0", 20, NetworkInterfaceType::Wifi);
  bare.link.link_type = 801u;
  NetworkWirelessInfo none = atm::judgeWireless(bare, probe, {}, {});
  CHECK(none.field_state == WirelessFieldState::Unavailable);
  CHECK(!none.signal_dbm.has_value());

  // A temporary loss after a good read preserves the last values as stale.
  NetworkWirelessInfo stale = atm::judgeWireless(bare, probe, {}, fresh);
  CHECK(stale.field_state == WirelessFieldState::Stale);
  CHECK(stale.signal_dbm.has_value());
  CHECK(*stale.signal_dbm == -42);
  CHECK(stale.noise_dbm.has_value());

  // /proc/net/wireless is a fallback only when the snapshot has no values.
  NetworkInterfaceInfo no_we = makeInfo("wlan0", 20, NetworkInterfaceType::Wifi);
  no_we.link.link_type = 801u;
  ProcWirelessStats proc;
  proc.present = true;
  proc.level_dbm = -50;
  proc.noise_dbm = -100;
  NetworkWirelessInfo fallback = atm::judgeWireless(no_we, probe, proc, {});
  CHECK(fallback.field_state == WirelessFieldState::Available);
  CHECK(fallback.signal_dbm.has_value());
  CHECK(*fallback.signal_dbm == -50);
  CHECK(fallback.noise_dbm.has_value());
  CHECK(*fallback.noise_dbm == -100);

  // The snapshot's own values win over the /proc fallback.
  NetworkInterfaceInfo both = makeInfo("wlan0", 20, NetworkInterfaceType::Wifi);
  both.link.link_type = 801u;
  both.wireless.present = true;
  both.wireless.level = -42;
  NetworkWirelessInfo prefer = atm::judgeWireless(both, probe, proc, {});
  CHECK(prefer.signal_dbm.has_value());
  CHECK(*prefer.signal_dbm == -42);
}

// -------------------------------------------------------------------------
// Formatting: units preserved, no invented percentages, no fake zeros
// -------------------------------------------------------------------------

void test_formatting() {
  run("formatting");

  CHECK(atm::formatWirelessSignal(std::nullopt) == "N/A");
  CHECK(atm::formatWirelessSignal(std::optional<int>{-42}) == "-42 dBm");
  CHECK(atm::formatWirelessSignal(std::optional<int>{5}) == "5 dBm");

  CHECK(atm::formatWirelessLinkQuality(std::nullopt, true) == "N/A");
  CHECK(atm::formatWirelessLinkQuality(std::optional<int>(54), true) ==
        "54 /70 (mac80211 driver scale)");
  CHECK(atm::formatWirelessLinkQuality(std::optional<int>(54), false) == "54");
  CHECK(atm::formatWirelessLinkQuality(std::optional<int>(54), true).find('%') ==
        std::string::npos);   // never a percentage

  CHECK(atm::formatWirelessNoise(std::nullopt) == "N/A");
  CHECK(atm::formatWirelessNoise(std::optional<int>{-96}) == "-96 dBm");

  NetworkWirelessInfo w;
  w.presence = WirelessPresence::NotWireless;
  CHECK(atm::describeWirelessLine(w) == "not a wireless interface");

  NetworkWirelessInfo online;
  online.presence = WirelessPresence::Wireless;
  online.association = WirelessAssociation::Associated;
  online.signal_dbm = -42;
  online.link_quality = 54;
  online.is_mac80211 = true;
  online.field_state = WirelessFieldState::Available;
  const std::string line = atm::describeWirelessLine(online);
  CHECK(line.find("associated") != std::string::npos);
  CHECK(line.find("-42 dBm") != std::string::npos);
  CHECK(line.find("54 /70") != std::string::npos);
  CHECK(line.find('%') == std::string::npos);

  // Stale fields are flagged, never silently presented as current.
  NetworkWirelessInfo stale = online;
  stale.field_state = WirelessFieldState::Stale;
  CHECK(atm::describeWirelessLine(stale).find("stale") != std::string::npos);
}

// -------------------------------------------------------------------------
// NetworkWirelessMonitor: caching, identity, history, eviction
// -------------------------------------------------------------------------

void test_monitor_lifecycle() {
  run("monitor lifecycle");

  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("atm_wireless_monitor_" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  makeWirelessTree(root, "wlan0");

  NetworkWirelessMonitor monitor(g_wireless_alerts, root, std::chrono::seconds(60), 8);

  NetworkInterfaceInfo wlan = makeInfo("wlan0", 30, NetworkInterfaceType::Wifi);
  wlan.link.link_type = 801u;
  wlan.link.carrier = 1;
  wlan.wireless.present = true;
  wlan.wireless.link = 54;
  wlan.wireless.level = -42;
  wlan.wireless.noise = -96;

  // A wired interface alongside must stay NotWireless.
  NetworkInterfaceInfo eth = makeInfo("enp0s3", 31, NetworkInterfaceType::Ethernet);
  eth.link.link_type = 1u;

  monitor.update(makeSnapshot({wlan, eth}));

  const NetworkWirelessInfo *w = monitor.tracked(wlan.identity());
  CHECK(w != nullptr);
  CHECK(w->present);
  CHECK(w->presence == WirelessPresence::Wireless);
  CHECK(w->is_mac80211);
  CHECK(w->phy_name == "phy0");
  CHECK(w->association == WirelessAssociation::Associated);
  CHECK(w->signal_dbm.has_value());
  CHECK(*w->signal_dbm == -42);

  const NetworkWirelessInfo *e = monitor.tracked(eth.identity());
  CHECK(e != nullptr);
  CHECK(e->presence == WirelessPresence::NotWireless);

  // One signal sample per update; a second update (within the probe interval,
  // same identity) only refreshes values and adds a sample.
  const std::chrono::steady_clock::time_point first_probe = w->last_probe;
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  wlan.wireless.level = -43;
  monitor.update(makeSnapshot({wlan, eth}));
  w = monitor.tracked(wlan.identity());
  CHECK(*w->signal_dbm == -43);
  CHECK(w->last_probe == first_probe);  // cached probe — no sysfs re-read
  CHECK(w->history.size() == 2);
  CHECK(monitor.wirelessHistory(wlan.identity()) != nullptr);

  // Rename keeps the stable identity and re-probes (new sysfs path).
  wlan.name = "wlan0-up";
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  monitor.update(makeSnapshot({wlan, eth}));
  CHECK(monitor.tracked(wlan.identity()) != nullptr);
  CHECK(monitor.tracked(wlan.identity())->name == "wlan0-up");
  CHECK(monitor.tracked(wlan.identity())->last_probe != first_probe);

  // Recreating the interface (new ifindex) starts a fresh record; the old one
  // is retained but marked gone.
  NetworkInterfaceInfo recreated = wlan;
  recreated.name = "wlan0";
  recreated.link.ifindex = 32;
  monitor.update(makeSnapshot({recreated, eth}));
  const NetworkWirelessInfo *fresh = monitor.tracked(recreated.identity());
  CHECK(fresh != nullptr);
  CHECK(fresh->present);
  const NetworkWirelessInfo *old = monitor.tracked(
      makeInfo("wlan0-up", 30, NetworkInterfaceType::Wifi).identity());
  CHECK(old != nullptr);
  CHECK(!old->present);

  // setHistoryMaxSamples bounds the ring.
  monitor.setHistoryMaxSamples(3);
  for (int i = 0; i < 5; ++i) {
    recreated.wireless.level = -40 - i;
    monitor.update(makeSnapshot({recreated, eth}));
  }
  const NetworkWirelessInfo *bounded = monitor.tracked(recreated.identity());
  CHECK(bounded->history.size() <= 3);
  CHECK(monitor.historyMaxSamples() == 3);

  std::filesystem::remove_all(root);
}

void test_monitor_global_unreadable() {
  run("monitor global unreadable");

  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("atm_wireless_unread_" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  makeWirelessTree(root, "wlan0");

  NetworkWirelessMonitor monitor(g_wireless_alerts, root, std::chrono::seconds(60), 8);
  NetworkInterfaceInfo wlan = makeInfo("wlan0", 40, NetworkInterfaceType::Wifi);
  wlan.link.link_type = 801u;
  wlan.wireless.present = true;
  wlan.wireless.level = -42;
  monitor.update(makeSnapshot({wlan}));
  CHECK(monitor.tracked(wlan.identity())->field_state == WirelessFieldState::Available);

  // sysfs unreadable this tick: last values preserved, marked stale, no probe.
  const std::chrono::steady_clock::time_point probe_at =
      monitor.tracked(wlan.identity())->last_probe;
  NetworkInterfaceSnapshot broken = makeSnapshot({wlan});
  broken.sysfs_readable = false;
  monitor.update(broken);
  const NetworkWirelessInfo *stale = monitor.tracked(wlan.identity());
  CHECK(stale->field_state == WirelessFieldState::Stale);
  CHECK(stale->signal_dbm.has_value());
  CHECK(*stale->signal_dbm == -42);
  CHECK(stale->last_probe == probe_at);

  // Recovery: values refresh and the state returns to available.
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  wlan.wireless.level = -38;
  monitor.update(makeSnapshot({wlan}));
  const NetworkWirelessInfo *recovered = monitor.tracked(wlan.identity());
  CHECK(recovered->field_state == WirelessFieldState::Available);
  CHECK(*recovered->signal_dbm == -38);

  std::filesystem::remove_all(root);
}

void test_monitor_proc_fallback_and_eviction() {
  run("monitor proc fallback + eviction");

  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("atm_wireless_proc_" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  makeIfaceDir(root, "wlan0");
  // No /wireless directory; only a /proc/net/wireless entry.
  writeFile(root / "proc" / "net" / "wireless",
            "Inter-| sta-|   Quality  |   Discarded packets    | Missed | WE\n"
            " wlan0    0000  54.  -50.  -100.  0  0  0  0  0  0  0  54\n");

  NetworkWirelessMonitor monitor(g_wireless_alerts, root, std::chrono::seconds(60), 8);
  NetworkInterfaceInfo wlan = makeInfo("wlan0", 50, NetworkInterfaceType::Wifi);
  wlan.link.link_type = 801u;
  monitor.update(makeSnapshot({wlan}));
  const NetworkWirelessInfo *w = monitor.tracked(wlan.identity());
  CHECK(w != nullptr);
  CHECK(w->presence == WirelessPresence::Wireless);
  CHECK(w->signal_dbm.has_value());
  CHECK(*w->signal_dbm == -50);   // /proc/net/wireless dBm fallback used
  CHECK(w->noise_dbm.has_value());
  CHECK(*w->noise_dbm == -100);
  CHECK(w->history.size() == 1);

  // Eviction: over the tracked bound, the oldest GONE identities are dropped
  // while live entries are never evicted.
  NetworkWirelessMonitor eviction_monitor(g_wireless_alerts, root, std::chrono::seconds(60), 2);
  std::vector<NetworkInterfaceInfo> batch;
  for (int i = 0; i < 70; ++i) {
    NetworkInterfaceInfo iface = makeInfo(
        "wl-gone" + std::to_string(i), 60 + i, NetworkInterfaceType::Wifi);
    batch.push_back(iface);
  }
  eviction_monitor.update(makeSnapshot(batch));
  CHECK(eviction_monitor.entries().size() == 70);  // all live: no eviction yet
  NetworkInterfaceInfo survivor = batch.front();
  eviction_monitor.update(makeSnapshot({survivor}));
  const NetworkWirelessInfo *kept = eviction_monitor.tracked(survivor.identity());
  CHECK(kept != nullptr && kept->present);
  CHECK(eviction_monitor.entries().size() ==
        NetworkWirelessMonitor::kMaxTrackedWirelessInterfaces);

  std::filesystem::remove_all(root);
}

void test_monitor_empty_wireless_dir_proc_fallback() {
  run("monitor empty /wireless dir falls back to /proc/net/wireless");

  // A mac80211 interface whose /wireless directory exists but is EMPTY (many
  // drivers never fill its link/level/noise files) must still pick up the
  // dynamic values from /proc/net/wireless instead of reporting "unavailable".
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("atm_wireless_empty_dir_" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  makeIfaceDir(root, "wlan0");
  std::filesystem::create_directories(root / "sys" / "class" / "net" / "wlan0" /
                                      "wireless");
  writeFile(root / "sys" / "class" / "net" / "wlan0" / "phy80211" / "name",
            "phy0\n");
  std::filesystem::create_directories(root / "sys" / "class" / "net" / "wlan0" /
                                      "phy80211");
  writeFile(root / "proc" / "net" / "wireless",
            "Inter-| sta-|   Quality  |   Discarded packets    | Missed | WE\n"
            " wlan0    0000  65.  -45.  -256.  0  0  0  0  0  0  0  54\n");

  NetworkWirelessMonitor monitor(g_wireless_alerts, root, std::chrono::seconds(60), 8);
  NetworkInterfaceInfo wlan = makeInfo("wlan0", 60, NetworkInterfaceType::Wifi);
  wlan.link.link_type = 801u;
  wlan.wireless.present = true;  // the /wireless directory exists, but empty
  monitor.update(makeSnapshot({wlan}));
  const NetworkWirelessInfo *w = monitor.tracked(wlan.identity());
  CHECK(w != nullptr);
  CHECK(w->presence == WirelessPresence::Wireless);
  CHECK(w->field_state == WirelessFieldState::Available);
  CHECK(w->signal_dbm.has_value());
  CHECK(*w->signal_dbm == -45);       // from /proc/net/wireless, not guessed
  CHECK(!w->noise_dbm.has_value());   // -256 sentinel -> absent, never "-256 dBm"
  CHECK(w->history.size() == 1);

  // A wired interface never consults the table and stays cleanly NotWireless.
  NetworkInterfaceInfo eth = makeInfo("enp0s3", 61, NetworkInterfaceType::Ethernet);
  eth.link.link_type = 1u;
  monitor.update(makeSnapshot({eth}));
  const NetworkWirelessInfo *e = monitor.tracked(eth.identity());
  CHECK(e != nullptr);
  CHECK(e->presence == WirelessPresence::NotWireless);
  CHECK(!e->signal_dbm.has_value());

  std::filesystem::remove_all(root);
}

void test_reset_and_wireless_history_api() {
  run("reset and wirelessHistory");

  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("atm_wireless_reset_" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  makeWirelessTree(root, "wlan0");

  NetworkWirelessMonitor monitor(g_wireless_alerts, root, std::chrono::seconds(60), 8);
  NetworkInterfaceInfo wlan = makeInfo("wlan0", 70, NetworkInterfaceType::Wifi);
  wlan.link.link_type = 801u;
  wlan.wireless.present = true;
  wlan.wireless.level = -42;
  monitor.update(makeSnapshot({wlan}));
  CHECK(monitor.wirelessHistory(wlan.identity()) != nullptr);
  CHECK(monitor.wirelessHistory("no-such") == nullptr);
  CHECK(monitor.entries().size() == 1);

  monitor.reset();
  CHECK(monitor.entries().empty());
  CHECK(monitor.tracked(wlan.identity()) == nullptr);
  CHECK(monitor.wirelessHistory(wlan.identity()) == nullptr);

  std::filesystem::remove_all(root);
}

// -------------------------------------------------------------------------
// Step 50: per-tick history sampling, honest gaps, transition events
// -------------------------------------------------------------------------

void test_monitor_history_sampling_and_gaps() {
  run("monitor history sampling + gaps");

  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("atm_wireless_whistory_" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  makeWirelessTree(root, "wlan0");

  NetworkWirelessMonitor monitor(g_wireless_alerts, root, std::chrono::seconds(60), 16);
  NetworkInterfaceInfo wlan = makeInfo("wlan0", 80, NetworkInterfaceType::Wifi);
  wlan.link.link_type = 801u;
  wlan.link.carrier = 1;
  wlan.wireless.present = true;
  wlan.wireless.link = 54;
  wlan.wireless.level = -42;
  wlan.wireless.noise = -96;

  monitor.update(makeSnapshot({wlan}));
  const NetworkWirelessInfo *w = monitor.tracked(wlan.identity());
  CHECK(w != nullptr);
  CHECK(w->history.size() == 1);
  CHECK(w->history.samples().front().valid);

  // A stale tick (fresh values temporarily gone) still records one sample per
  // tick — marked invalid, values nullopt — so coverage/span stay honest and
  // a gap is never interpolated into a fake reading.
  NetworkInterfaceInfo stale_info =
      makeInfo("wlan0", 80, NetworkInterfaceType::Wifi);
  stale_info.link.link_type = 801u;
  stale_info.link.carrier = 1;
  stale_info.wireless.present = true;  // directory present, files unreadable
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  monitor.update(makeSnapshot({stale_info}));
  w = monitor.tracked(wlan.identity());
  CHECK(w->history.size() == 2);
  const atm::WirelessHistorySample &gap = w->history.samples().back();
  CHECK(!gap.valid);
  CHECK(!gap.signal_dbm.has_value());  // preserved display value is NOT copied
  CHECK(!gap.link_quality.has_value());
  CHECK(gap.association == WirelessAssociation::Associated);

  // Recovery: the third tick is a valid sample again, with no fabricated zeros
  // anywhere in the ring.
  wlan.wireless.level = -40;
  monitor.update(makeSnapshot({wlan}));
  w = monitor.tracked(wlan.identity());
  CHECK(w->history.size() == 3);
  CHECK(w->history.samples().back().valid);
  CHECK(*w->history.samples().back().signal_dbm == -40.0);

  // Exactly one sample per tick: four more updates stay at one per tick and
  // bitrate/frequency/channel stay nullopt (unavailable through native sources).
  for (int i = 0; i < 4; ++i) {
    wlan.wireless.level = -40 - i;
    monitor.update(makeSnapshot({wlan}));
  }
  w = monitor.tracked(wlan.identity());
  CHECK(w->history.size() == 7);
  for (const atm::WirelessHistorySample &s : w->history.samples()) {
    CHECK(!s.bitrate_bps.has_value());
    CHECK(!s.frequency_mhz.has_value());
    CHECK(!s.channel.has_value());
  }

  std::filesystem::remove_all(root);
}

void test_monitor_non_wireless_clears_history() {
  run("monitor non-wireless clears history");

  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("atm_wireless_clear_" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  makeWirelessTree(root, "wlan0");

  NetworkWirelessMonitor monitor(g_wireless_alerts, root, std::chrono::seconds(60), 8);
  NetworkInterfaceInfo wlan = makeInfo("wlan0", 90, NetworkInterfaceType::Wifi);
  wlan.link.link_type = 801u;
  wlan.link.carrier = 1;
  wlan.wireless.present = true;
  wlan.wireless.level = -42;
  monitor.update(makeSnapshot({wlan}));
  CHECK(monitor.tracked(wlan.identity())->history.size() == 1);

  // The identity stops being wireless (here via a rename, which forces a fresh
  // sysfs probe): history, connection events and the event state machine all
  // reset — nothing is carried into a non-wireless link.
  wlan.name = "wlan0-nw";        // rename forces a re-probe of the new path
  wlan.link.link_type = 1u;      // ARPHRD_ETHER
  wlan.wireless.present = false;
  monitor.update(makeSnapshot({wlan}));
  const NetworkWirelessInfo *w = monitor.tracked(wlan.identity());
  CHECK(w != nullptr);
  CHECK(w->presence == WirelessPresence::NotWireless);
  CHECK(w->history.empty());
  CHECK(w->connection_events.empty());
  CHECK(w->last_sample_wall == std::chrono::system_clock::time_point{});

  std::filesystem::remove_all(root);
}

void test_monitor_connection_events() {
  run("monitor debounced connection events");

  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("atm_wireless_conn_" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  makeWirelessTree(root, "wlan0");

  atm::AlertManager alerts;
  NetworkWirelessMonitor monitor(alerts, root, std::chrono::seconds(60), 8);
  NetworkInterfaceInfo wlan = makeInfo("wlan0", 100, NetworkInterfaceType::Wifi);
  wlan.link.link_type = 801u;
  wlan.wireless.present = true;
  wlan.wireless.level = -42;

  // The first sample establishes the baseline silently: no "unknown ->" event
  // is ever emitted for the initial association.
  wlan.link.carrier = 1;
  monitor.update(makeSnapshot({wlan}));
  const NetworkWirelessInfo *w = monitor.tracked(wlan.identity());
  CHECK(w->association == WirelessAssociation::Associated);
  CHECK(w->connection_events.empty());
  CHECK(alerts.history().empty());

  // Repeated same-state ticks never append connection events.
  wlan.wireless.level = -43;
  monitor.update(makeSnapshot({wlan}));
  wlan.wireless.level = -44;
  monitor.update(makeSnapshot({wlan}));
  w = monitor.tracked(wlan.identity());
  CHECK(w->connection_events.empty());

  // A single disassociated tick is only a pending candidate (debounce): the
  // event is not committed until the state persists for a second tick.
  wlan.link.carrier = 0;
  monitor.update(makeSnapshot({wlan}));
  w = monitor.tracked(wlan.identity());
  CHECK(w->association == WirelessAssociation::Disconnected);
  CHECK(w->connection_events.empty());

  wlan.wireless.level = -45;
  monitor.update(makeSnapshot({wlan}));
  w = monitor.tracked(wlan.identity());
  CHECK(w->connection_events.size() == 1);
  const atm::WirelessConnectionEvent &first =
      w->connection_events.samples().front();
  CHECK(first.type == atm::WirelessConnectionEventType::Disassociated);
  CHECK(first.previous_association == WirelessAssociation::Associated);
  CHECK(first.new_association == WirelessAssociation::Disconnected);
  CHECK(first.interface_name == "wlan0");
  CHECK(first.confident);
  CHECK(!alerts.history().empty());
  const atm::AlertEvent &warning = alerts.history().back();
  CHECK(warning.type == atm::AlertType::WirelessConnectionChanged);
  CHECK(warning.severity == atm::AlertSeverity::Warning);
  CHECK(!warning.is_recovery);

  // Re-associated: also debounced, and announced as a recovery.
  wlan.link.carrier = 1;
  monitor.update(makeSnapshot({wlan}));
  w = monitor.tracked(wlan.identity());
  CHECK(w->connection_events.size() == 1);  // still pending
  monitor.update(makeSnapshot({wlan}));
  w = monitor.tracked(wlan.identity());
  CHECK(w->connection_events.size() == 2);
  const atm::WirelessConnectionEvent &second =
      w->connection_events.samples().back();
  CHECK(second.type == atm::WirelessConnectionEventType::Reconnected);
  CHECK(second.previous_association == WirelessAssociation::Disconnected);
  CHECK(second.new_association == WirelessAssociation::Associated);
  const atm::AlertEvent &recovery = alerts.history().back();
  CHECK(recovery.type == atm::AlertType::WirelessConnectionChanged);
  CHECK(recovery.severity == atm::AlertSeverity::Normal);
  CHECK(recovery.is_recovery);

  std::filesystem::remove_all(root);
}

int main() {
  test_classification_strict();
  test_read_wireless_probe();
  test_proc_net_wireless_parsing();
  test_association();
  test_dynamic_field_freshness();
  test_formatting();
  test_monitor_lifecycle();
  test_monitor_global_unreadable();
  test_monitor_proc_fallback_and_eviction();
  test_monitor_empty_wireless_dir_proc_fallback();
  test_reset_and_wireless_history_api();
  test_monitor_history_sampling_and_gaps();
  test_monitor_non_wireless_clears_history();
  test_monitor_connection_events();

  std::fprintf(stderr, "PASS: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}