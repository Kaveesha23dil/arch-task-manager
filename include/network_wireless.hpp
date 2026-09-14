#pragma once

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

/// Wireless presence of one interface, determined only from reliable native
/// kernel metadata: the "phy80211" directory, the legacy "wireless" (WIRELESS_EXT)
/// directory, or an IEEE80211-family ARPHRD link type. A wireless-sounding name
/// or driver alone never classifies an interface as wireless.
enum class WirelessPresence {
  Unknown,       // no probe has been absorbed for the identity yet
  NotWireless,   // no reliable native wireless signal was found
  Wireless,      // phy80211 and/or /wireless directory present, or IEEE80211 ARPHRD
};

/// Association state of a wireless interface. This is KERNEL-LEVEL only and
/// carrier-derived: mac80211 raises the interface carrier on association, so
/// "Associated" means "the kernel carrier bit is set" — never "BSSID-verified".
/// The kernel does not expose SSID/BSSID/channel/frequency/bitrate via sysfs,
/// so those fields are intentionally absent (they require nl80211).
enum class WirelessAssociation {
  Unknown,       // wireless, but no reliable association evidence is exposed
  Associated,    // carrier set (mac80211 raises carrier on association)
  Disconnected,  // carrier clear
  Unavailable,   // underlying metadata was temporarily unreadable
};

/// Freshness of the dynamic (per-tick) wireless fields.
enum class WirelessFieldState {
  Unknown,       // never judged yet
  Available,     // values read this tick
  Stale,         // temporary failure — last valid values preserved
  Unavailable,   // clean absence (driver exposes no WIRELESS_EXT stats)
};

[[nodiscard]] const char *wirelessPresenceName(WirelessPresence presence);
[[nodiscard]] const char *wirelessAssociationName(WirelessAssociation association);
[[nodiscard]] const char *wirelessFieldStateName(WirelessFieldState state);

/// The WIRELESS_EXT link-quality scale used by mac80211's WE compatibility
/// layer (cfg80211 reports quality on a 0..70 scale). Only applies to
/// mac80211-backed devices (a "phy80211" directory exists); legacy WE drivers
/// may use any scale, so the raw value is shown with its source scale and is
/// never converted into a percentage.
inline constexpr int kWirelessMac80211WextQualityMax = 70;

/// One cached probe of the static wireless metadata (the phy80211 relationship).
/// This is what changes least often and is therefore read on a reread interval,
/// never once per UI render.
struct WirelessProbe {
  bool probe_ok = true;            // the probe did not hit a permission failure
  bool access_denied = false;      // a sysfs path existed but was not readable
  bool phy80211_present = false;   // <iface>/phy80211 exists (mac80211 device)
  bool wireless_dir_present = false;  // <iface>/wireless (WIRELESS_EXT) exists
  bool metadata_available = false;    // any native wireless metadata was readable
  std::string phy_name;              // e.g. "phy0"
  std::optional<int> phy_index;      // raw phy80211/index value
};

/// Probes `<root>/sys/class/net/<iface>/phy80211` and `<iface>/wireless` with
/// native filesystem APIs (no subprocesses, no shell). `root` defaults to "/"
/// (the real sysfs tree) but may be an alternate root for hermetic tests.
/// Returns a `WirelessProbe`; never throws.
[[nodiscard]] WirelessProbe readWirelessProbe(
    const std::filesystem::path &root, const std::string &iface);

/// The kernel's "measurement invalid" sentinel for the /proc/net/wireless
/// level/noise columns (IW_QUAL_LEVEL_INVALID / IW_QUAL_NOISE_INVALID, 0x100,
/// printed signed as -256). A driver that reports no noise floor writes -256
/// here; a radio floor of "-256 dBm" is physically meaningless, so it is
/// treated as unavailable rather than displayed as a real measurement.
inline constexpr int kIwQualInvalidSentinel = -256;

/// Per-interface stats parsed from /proc/net/wireless. Only the signal level and
/// noise (dBm, normally negative) are extracted; the "link" quality column uses
/// a driver-defined scale and is deliberately not exposed here.
struct ProcWirelessStats {
  bool present = false;                // the interface had a line in the table
  std::optional<int> level_dbm;        // signal strength in dBm
  std::optional<int> noise_dbm;        // noise floor in dBm
};

/// Parses the full text of /proc/net/wireless and returns the stats for `iface`.
/// Pure and hermetic-testable: whitespace-padded headers, missing interfaces and
/// syntactically invalid values are handled without ever inventing a value.
[[nodiscard]] ProcWirelessStats parseProcNetWirelessStats(
    const std::string &text, const std::string &iface);

/// Reads `<path>` (normally "<root>/proc/net/wireless") and parses `iface`'s
/// entry. An unreadable/missing file returns an absent record, never an error.
[[nodiscard]] ProcWirelessStats readProcNetWirelessStats(
    const std::filesystem::path &path, const std::string &iface);

/// One interface's tracked wireless status. The static PHY fields come from the
/// cached probe; the dynamic fields (link quality, signal, noise, association,
/// carrier) are re-derived every tick from the existing interface-details
/// snapshot — there is no second polling loop. Fields are only ever filled from
/// native kernel metadata; an unknown value is its explicit state, never a
/// guessed zero. Settings such as SSID/BSSID/channel/frequency/bitrate are
/// intentionally absent because the kernel does not expose them via sysfs.
struct NetworkWirelessInfo {
  std::string identity;          // stable key ("idx:<ifindex>" / "name:<name>")
  std::string name;              // current kernel name
  bool present = true;           // false once the identity has disappeared

  WirelessPresence presence = WirelessPresence::Unknown;
  std::string phy_name;                      // "phy0" (cached, when known)
  std::optional<int> phy_index;              // phy80211/index (cached)
  WirelessFieldState phy_state = WirelessFieldState::Unknown;
  bool is_mac80211 = false;      // phy80211 directory present (WE-quality /70)

  std::optional<int> link_quality;  // raw WIRELESS_EXT quality (driver scale)
  std::optional<int> signal_dbm;    // received signal strength in dBm
  std::optional<int> noise_dbm;     // noise floor in dBm
  WirelessFieldState field_state = WirelessFieldState::Unknown;

  WirelessAssociation association = WirelessAssociation::Unknown;  // carrier-derived
  bool enabled = false;             // administrative state (IFF_UP)
  bool carrier_exposed = false;     // the driver exposes a "carrier" file
  bool has_carrier = false;

  // Per-identity received-signal-strength history (one sample per tick; the
  // ring is bounded and rebuilt when the history retention changes).
  ResourceHistory<TimedSample> signal_history{0};

  std::chrono::system_clock::time_point last_read{};  // last successful tick
  std::chrono::steady_clock::time_point first_seen{};
  std::chrono::steady_clock::time_point last_probe{};
  bool probe_ok = true;  // last static-probe attempt succeeded (internal flag)
};

/// Derives one interface's per-tick wireless view from the latest details
/// snapshot, the cached probe and an optional /proc/net/wireless fallback, while
/// preserving the previous record's last valid values across a temporary
/// failure (marked stale). Pure and testable: no syscalls, no global state.
[[nodiscard]] NetworkWirelessInfo judgeWireless(
    const NetworkInterfaceInfo &info, const WirelessProbe &probe,
    const ProcWirelessStats &proc, const NetworkWirelessInfo &previous);

// --- Pure, testable formatting helpers --------------------------------------

/// "-42 dBm" (negative values preserved) or "N/A" when the signal is unknown —
/// an unknown signal is never rendered as "0 dBm".
[[nodiscard]] std::string formatWirelessSignal(const std::optional<int> &signal_dbm);

/// Raw WIRELESS_EXT link quality in its driver-defined scale — "54 /70" for a
/// mac80211 device (the documented cfg80211 WE-compat scale), otherwise just the
/// raw value "54". Never a percentage, never compared with signal strength.
[[nodiscard]] std::string formatWirelessLinkQuality(const std::optional<int> &link,
                                                    bool is_mac80211);

/// "-96 dBm" or "N/A" when the noise floor is unknown.
[[nodiscard]] std::string formatWirelessNoise(const std::optional<int> &noise_dbm);

/// Compact one-line wireless summary for the traffic-history section
/// ("associated (carrier) \u2014 signal -42 dBm, link quality 54 /70").
[[nodiscard]] std::string describeWirelessLine(const NetworkWirelessInfo &wireless);

/// How long a cached PHY probe is reused before it is re-probed. PHY identity
/// (name/index) changes far less often than the per-tick signal values, so a
/// fresh probe every N seconds is plenty; new/renamed identities and failed
/// probes are always re-probed on the next tick.
inline constexpr std::chrono::seconds kWirelessRereadInterval{10};

/// Monitors per-interface wireless status.
///
/// Fed from the existing NetworkInterfaceMonitor snapshot exactly once per tick
/// by the application's monitoring loop (no second polling loop, nothing read on
/// the UI-render path). Static PHY metadata is cached and only re-probed when an
/// interface appears, its name (identity) changes, the previous probe failed, or
/// the reread interval elapsed. The dynamic WIRELESS_EXT values (link quality,
/// signal, noise) come from the already-read snapshot; /proc/net/wireless is
/// consulted as a native fallback only when a wireless interface exposes no
/// /wireless directory. A vanished interface is retained and marked gone, and a
/// recreated interface (new ifindex) naturally starts a fresh record instead of
/// inheriting an old one's values.
///
/// Limitations (see Step 49 scope): SSID, BSSID, channel, frequency and
/// negotiated bitrate are not available through sysfs or /proc/net/wireless
/// (they require the nl80211 netlink API, which this monitoring-only step does
/// not add), so they are intentionally absent. Signal-worthiness alerting is
/// likewise out of scope: the existing network-traffic alert engine is limited
/// to traffic-series rules, and adding wireless rules there would require an
/// alert-engine redesign, so this monitor does not emit alerts.
class NetworkWirelessMonitor {
 public:
  static constexpr std::size_t kMaxTrackedWirelessInterfaces = 64;

  explicit NetworkWirelessMonitor(
      std::filesystem::path root = "/",
      std::chrono::milliseconds reread_interval = kWirelessRereadInterval,
      std::size_t history_max_samples = kDefaultInterfaceHistorySamples);

  NetworkWirelessMonitor(const NetworkWirelessMonitor &) = delete;
  NetworkWirelessMonitor &operator=(const NetworkWirelessMonitor &) = delete;
  ~NetworkWirelessMonitor() = default;

  /// Processes one discovery snapshot (produced by NetworkInterfaceMonitor).
  void update(const NetworkInterfaceSnapshot &snapshot);

  /// Latest wireless status for an identity; nullptr when never seen.
  [[nodiscard]] const NetworkWirelessInfo *tracked(
      const std::string &identity) const;

  /// All retained status records (present and gone identities).
  [[nodiscard]] const std::unordered_map<std::string, NetworkWirelessInfo> &
  entries() const {
    return tracked_;
  }

  /// The received-signal-strength history for an identity (or nullptr).
  [[nodiscard]] const ResourceHistory<TimedSample> *signalHistory(
      const std::string &identity) const;

  void setHistoryMaxSamples(std::size_t max_samples);
  [[nodiscard]] std::size_t historyMaxSamples() const { return history_max_samples_; }

  void reset();

 private:
  std::filesystem::path root_;
  std::chrono::milliseconds reread_interval_;
  std::size_t history_max_samples_;
  std::unordered_map<std::string, NetworkWirelessInfo> tracked_;

  void evictOverflow();
};

}  // namespace atm