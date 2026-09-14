#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "alert_manager.hpp"
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

/// One wireless-history sample, captured at most once per interface per refresh
/// cycle (never on the UI-render path). Every metric is strongly optional: an
/// unavailable metric is std::nullopt — never a fabricated zero — and
/// `valid == false` marks a tick whose dynamic fields could not be freshly read
/// (stale/unavailable), so consumers can compute honest coverage and never
/// mistake a gap for a real measurement. Samples are never interpolated; a
/// missing/stale tick is simply a gap.
///
/// Bitrate, frequency and channel are not exposed by the kernel's sysfs or
/// /proc/net/wireless interfaces (they require the nl80211 netlink API), so in
/// production they stay std::nullopt; the model carries them anyway so the
/// export/summary machinery is complete and unit-testable with deterministic
/// fixtures.
struct WirelessHistorySample {
  std::chrono::steady_clock::time_point timestamp{};  // ordering
  WirelessAssociation association = WirelessAssociation::Unknown;
  std::optional<double> signal_dbm;    // received signal strength in dBm
  std::optional<int> link_quality;     // raw WIRELESS_EXT value (driver scale)
  bool is_mac80211 = false;            // link-quality scale metadata (mac80211 "/70")
  std::optional<double> bitrate_bps;   // canonical bits per second
  std::optional<double> frequency_mhz; // centre frequency, MHz
  std::optional<int> channel;          // channel index, when reliably known
  bool valid = false;                  // dynamic fields freshly read this tick
};

/// One observed wireless connection state for connection-event detection.
/// Values come from the existing per-tick interface snapshot (never a second
/// polling loop); a metric the approved native sources (sysfs, /proc/net/
/// wireless) cannot expose stays std::nullopt and never causes a guess.
struct WirelessObservation {
  bool present = false;             // identity discovered in the snapshot this tick
  bool wireless = true;             // WirelessPresence::Wireless
  WirelessAssociation association = WirelessAssociation::Unknown;
  std::optional<double> signal_dbm;       // dBm at event time (display hint)
  std::optional<double> frequency_mhz;    // centre frequency (nl80211-only)
  std::optional<int> channel;             // channel index (nl80211-only)
  std::optional<std::uint64_t> ap_fingerprint;  // one-way hash, never raw identity
  bool ap_identity_reliable = false;  // false => fingerprints cannot be trusted
};

/// The kind of wireless connection event recorded in the bounded per-interface
/// history. Type names are stable, human-readable lowercase strings (see
/// wirelessConnectionEventTypeName) shared by the UI and the export formats.
enum class WirelessConnectionEventType {
  Associated,         // association gained for the first time
  Disassociated,      // association lost (carrier drop / carrier lost)
  Reconnected,        // association regained after a prior association
  Roamed,             // re-associated with a reliably different AP (handoff)
  InterfaceUnavailable,  // the wireless interface (or its association) went away
  InterfaceAvailable,    // an unavailable wireless interface became available
  StateUnknown,          // association can no longer be determined
};

/// One recorded wireless connection event (Step 51). Deduplicated and
/// debounced: repeated same-state ticks never append events, a transient
/// change must persist for kWirelessConnectionDebounceTicks consecutive ticks
/// before it is committed, and the very first observation establishes the
/// baseline silently. Raw AP identity (SSID/BSSID) is never stored or exported;
/// only the one-way fingerprint-change and reliability flags appear.
struct WirelessConnectionEvent {
  std::chrono::steady_clock::time_point timestamp{};   // ordering (confirm tick)
  std::chrono::system_clock::time_point wall_clock{};  // "shown at" display time
  WirelessConnectionEventType type = WirelessConnectionEventType::Associated;
  bool confident = true;             // false for state_unknown / uncertain steps
  std::string source;                // "carrier" / "presence" / "ap_identity"
  std::string interface_name;        // kernel name at event time
  WirelessAssociation previous_association = WirelessAssociation::Unknown;
  WirelessAssociation new_association = WirelessAssociation::Unknown;
  std::optional<double> previous_signal_dbm;
  std::optional<double> new_signal_dbm;
  std::optional<double> previous_frequency_mhz;
  std::optional<double> new_frequency_mhz;
  std::optional<int> previous_channel;
  std::optional<int> new_channel;
  bool ap_fingerprint_changed = false;  // roaming: reliable AP identity changed
  bool ap_identity_reliable = false;    // whether AP identity was reliable
};

/// Upper bound on the retained connection-event history per interface.
inline constexpr std::size_t kMaxWirelessConnectionEvents = 64;

/// How many consecutive ticks a candidate state must persist before it is
/// committed (and hence appears in the history, UI, alerts and exports). A
/// flapping state that never persists this long is ignored entirely.
inline constexpr unsigned kWirelessConnectionDebounceTicks = 2;

/// Longest gap between observations (wall clock) before the tracker treats the
/// history as discontinuous (e.g. after suspend/resume) and silently
/// rebaselines without emitting an event. wall-clock gaps are used because the
/// steady clock stops on many platforms during suspension.
inline constexpr std::chrono::seconds kWirelessConnectionResetGap{30};

/// Per-interface connection-event tracking state (debouncing and baselines).
/// Not a user-facing value: it only drives when a connection event is
/// announced. Moved along with the record so a renamed interface keeps its
/// event continuity.
struct WirelessConnectionTracker {
  WirelessObservation confirmed;          // last committed observation
  bool confirmed_valid = false;           // a baseline has been established
  bool has_been_associated = false;       // this record has ever been associated

  // Confirmation-window candidate (debounce).
  bool pending_valid = false;
  WirelessObservation pending;
  unsigned pending_remaining = 0;         // consecutive ticks left to confirm

  std::chrono::system_clock::time_point last_observation_wall{};
};

/// Two observations describe the same lifecycle connection state when their
/// identity presence and carrier-derived association match. Signal/quality/
/// frequency changes alone never constitute a state change; a reliably
/// reported AP-identity handoff (while associated) is a state change.
[[nodiscard]] bool sameWirelessConnectionState(const WirelessObservation &a,
                                               const WirelessObservation &b);

/// Classifies the committed change from `before` to `after`. Pure and
/// deterministic: no I/O, no global state. Returns std::nullopt for no change
/// (signal/frequency only). `has_been_associated` decides Associated vs
/// Reconnected.
[[nodiscard]] std::optional<WirelessConnectionEventType>
classifyWirelessConnectionChange(const WirelessObservation &before,
                                 const WirelessObservation &after,
                                 bool has_been_associated);

/// Stable, lowercase, human-readable type name shared by the UI and exports:
/// "associated"/"disassociated"/"reconnected"/"roamed"/"interface unavailable"/
/// "interface available"/"state unknown".
[[nodiscard]] const char *wirelessConnectionEventTypeName(
    WirelessConnectionEventType type);

/// One-way (FNV-1a) digest of an opaque AP identity string (e.g. a BSSID).
/// Raw identities are never stored or exported — only this digest is ever
/// compared, so roaming can be detected without recording identifying data.
[[nodiscard]] std::uint64_t wirelessApFingerprint(std::string_view identity);

/// Advances one interface's connection-event state machine with this tick's
/// observation: applies the confirmation debounce and the suspend/long-gap
/// rebaseline, returns the committed event (if any) and mutates `tracker` in
/// place. Pure and deterministic: the same tracker + observation/timestamp
/// sequence always yields the same event stream.
[[nodiscard]] std::optional<WirelessConnectionEvent>
advanceWirelessConnectionTracker(
    WirelessConnectionTracker &tracker, const WirelessObservation &observation,
    std::chrono::steady_clock::time_point now,
    std::chrono::system_clock::time_point wall_clock,
    const std::string &interface_name,
    unsigned debounce_ticks = kWirelessConnectionDebounceTicks,
    std::chrono::seconds reset_gap = kWirelessConnectionResetGap);

/// Compact human-readable description of one connection event for the UI
/// (e.g. "disassociated  wlan0  associated -> disconnected  signal -42 dBm ->
/// -45 dBm"). Pure ASCII, never contains '|' or a line break.
[[nodiscard]] std::string describeWirelessConnectionEvent(
    const WirelessConnectionEvent &event);

/// One interface's tracked wireless status. The static PHY fields come from the
/// cached probe; the dynamic fields (link quality, signal, noise, association,
/// carrier) are re-derived every tick from the existing interface-details
/// snapshot — there is no second polling loop. Fields are only ever filled from
/// native kernel metadata; an unknown value is its explicit state, never a
/// guessed zero. Settings such as SSID/BSSID are intentionally absent because
/// the kernel does not expose them via sysfs.
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

  // Per-identity wireless sample history: one self-contained sample per tick
  // (never on the render path), bounded and rebuilt when the history retention
  // changes. Replaces the Step 49 per-metric signal ring so association, scale
  // metadata and the optional metrics travel with each sample, and gaps stay
  // honest. The ring is cleared when the identity stops being wireless.
  ResourceHistory<WirelessHistorySample> history{0};

  // Bounded connection-event history (newest last): association changes,
  // reconnections, roaming, availability transitions and unknown-state events,
  // deduplicated and debounced (Step 51).
  ResourceHistory<WirelessConnectionEvent> connection_events{0};

  // Drives which events (if any) are announced each tick. Kept with the record
  // so a renamed interface keeps its event continuity; reset whenever the
  // identity stops being wireless.
  WirelessConnectionTracker connection_tracker;

  // Wall clock captured when the newest history sample was recorded. The
  // exported wireless-history timestamps are anchored to this time the same way
  // the traffic export anchors to the series' last update.
  std::chrono::system_clock::time_point last_sample_wall{};

  std::chrono::system_clock::time_point last_read{};  // last successful tick
  std::chrono::steady_clock::time_point first_seen{};
  std::chrono::steady_clock::time_point last_probe{};
  bool probe_ok = true;  // last static-probe attempt succeeded (internal flag)
};

/// Derived summary of one interface's wireless history — the numbers the
/// wireless-history section shows (current/min/max/average over the retained
/// valid samples, sample counts, coverage, span and transition count). Pure and
/// computed strictly from the retained ring, never from the live kernel state.
struct WirelessHistorySummary {
  bool has_data = false;
  std::size_t sample_count = 0;      // retained samples
  std::size_t valid_sample_count = 0;  // ticks with freshly-read dynamic fields
  double coverage = 0.0;             // sample_count / retention bound (0..1)
  double valid_coverage = 0.0;       // valid_sample_count / retention bound
  bool history_complete = false;     // sample_count >= retention bound
  double span_seconds = 0.0;         // newest - oldest sample span
  std::size_t event_count = 0;       // connection events across the history

  // Signal (dBm). Per-metric statistics include only samples that carried a
  // value; invalid/gap ticks contribute nothing.
  std::optional<double> current_signal_dbm;
  std::optional<double> min_signal_dbm;
  std::optional<double> max_signal_dbm;
  std::optional<double> avg_signal_dbm;
  std::size_t signal_sample_count = 0;

  // Link quality (raw WIRELESS_EXT value on its driver scale).
  std::optional<int> current_link_quality;
  std::optional<int> min_link_quality;
  std::optional<int> max_link_quality;
  std::optional<double> avg_link_quality;
  std::size_t link_quality_sample_count = 0;

  // Connection bitrate (canonical bits per second; unavailable on this stack).
  std::optional<double> current_bitrate_bps;
  std::optional<double> min_bitrate_bps;
  std::optional<double> max_bitrate_bps;
  std::optional<double> avg_bitrate_bps;
  std::size_t bitrate_sample_count = 0;

  // Centre frequency (MHz) and channel (unavailable on this stack).
  std::optional<double> current_frequency_mhz;
  std::optional<double> min_frequency_mhz;
  std::optional<double> max_frequency_mhz;
  std::optional<double> avg_frequency_mhz;
  std::size_t frequency_sample_count = 0;

  std::optional<int> current_channel;
  std::size_t channel_sample_count = 0;

  std::chrono::system_clock::time_point last_update{};  // newest sample wall time
};

/// Summarises `wireless`'s retained history. Never touches the live kernel
/// state; the returned summary is a pure function of the retained ring.
[[nodiscard]] WirelessHistorySummary summarizeWirelessHistory(
    const NetworkWirelessInfo &wireless, std::size_t max_samples);

// -------------------------------------------------------------------------
// Step 52 — Wireless connection quality summary and stability analysis.
//
// A single pure pass over the already-retained rings (sample history and
// connection events) produces an honest, explainable picture of how stable an
// interface's wireless connection has been: signal/bitrate statistics with
// variability, how much time was actually spent connected/disconnected versus
// unobserved, how long the worst outage lasted, and how the recent events
// (disconnections, reconnections, roaming, availability flaps) add up.
//
// The classification is intentionally conservative and transparent:
//  * Everything is derived from data that actually exists; a missing metric is
//    never invented as a zero and never penalises the score.
//  * Factors whose data was insufficient are excluded and the remaining
//    weights are renormalised (their ratios are preserved) — so e.g. a
//    production interface, where the native sources expose no bitrate, is not
//    unfairly downgraded just because the bitrate factor was not applicable.
//  * Below a data-coverage gate the grade is Unknown and no score is emitted;
//    a sparse history does not coerce into a misleading grade.
//  * Intervals separated by a gap longer than kWirelessConnectionResetGap
//    (e.g. after suspend/resume) count as unobserved and break continuous
//    runs, so pause-then-resume never fabricates connected time.
//  * Scores are a renormalised weighted mean of the included factors, each
//    with a documented weight and a plain-English reason, so the UI can show
//    exactly why an interface got the grade it got.
// -------------------------------------------------------------------------

/// Qualitative stability grade. Ordering matters: Unknown < Poor < Fair < Good
/// < Excellent.
enum class WirelessStability {
  Unknown,     // insufficient/absent data — no classification, no score
  Poor,        // score below kWirelessFairThreshold
  Fair,        // score in [fair, good)
  Good,        // score in [good, excellent)
  Excellent,   // score at least kWirelessExcellentThreshold
};

/// Stable, lowercase name shared by the UI and the exports: "unknown"/"poor"/
/// "fair"/"good"/"excellent".
[[nodiscard]] const char *wirelessStabilityName(WirelessStability stability);

// --- Assessment thresholds (documented in the classification doc comment) ----

/// Minimum freshly-read samples retained before a stability grade is even
/// attempted. Below this the window is "insufficient data" (Unknown, no score),
/// so a sparsely sampled history never coerces into a misleading grade.
inline constexpr std::size_t kWirelessQualityMinValidSamples = 5;

/// Minimum determined time (connected + disconnected, seconds) before the
/// connectivity-derived factors are meaningful.
inline constexpr double kWirelessQualityMinDeterminedSeconds = 5.0;

/// A factor only participates when at least this many of its own values are
/// present; below it the factor is excluded and the other weights are
/// renormalised (ratios preserved, values never invented).
inline constexpr std::size_t kWirelessQualityMinFactorSamples = 3;

/// Score bands (score out of 100).
inline constexpr double kWirelessExcellentThreshold = 80.0;
inline constexpr double kWirelessGoodThreshold = 60.0;
inline constexpr double kWirelessFairThreshold = 40.0;

/// Signal-strength mapping anchors: -30 dBm is "best", -90 dBm is "worst".
inline constexpr double kWirelessSignalBestDbm = -30.0;
inline constexpr double kWirelessSignalWorstDbm = -90.0;

/// Inter-sample gaps longer than this (steady clock) are discontinuities whose
/// time is reported as unobserved and which break continuous connectivity runs.
/// Mirrors kWirelessConnectionResetGap so both layers agree what "continuous"
/// means (suspension can therefore never fabricate connected time).
inline constexpr std::chrono::seconds kWirelessQualityMaxGap =
    kWirelessConnectionResetGap;

/// One factor used by the stability score, with a plain-English reason so the
/// UI can explain the grade. `value` is the factor's raw quality on [0,1] when
/// it could be computed; `weight` is its renormalised weight (these always sum
/// to 1 among the factors actually present); `weight_reason` explains why the
/// factor is included (or why it is not).
struct WirelessStabilityFactor {
  std::string name;          // e.g. "connection continuity"
  std::string detail;        // plain-English summary of the data behind it
  double weight = 0.0;       // renormalised contribution to the score
  std::optional<double> value;  // quality on [0,1]; engaged only when computed
};

/// Result of the deterministic stability classification.
struct WirelessStabilityAssessment {
  WirelessStability stability = WirelessStability::Unknown;
  std::optional<double> score;  // 0..100; absent (Unknown) when coverage fell short
  std::vector<WirelessStabilityFactor> factors;  // in weight order when present
};

/// Step 52 summary of one interface's wireless connection quality, computed
/// strictly from the retained rings (never the live kernel state). All fields
/// are pure functions of the history samples and connection events, so the
/// values in the UI, the CSV export and the JSON export are always identical.
struct WirelessQualitySummary {
  bool has_data = false;  // any retained sample or connection event at all

  // Window and coverage.
  std::chrono::system_clock::time_point window_start{};
  std::chrono::system_clock::time_point window_end{};
  std::size_t sample_count = 0;
  std::size_t valid_sample_count = 0;
  double coverage = 0.0;         // sample_count / retention bound (0..1)
  double valid_coverage = 0.0;   // valid_sample_count / retention bound
  double span_seconds = 0.0;     // newest - oldest retained-sample span
  double unobserved_seconds = 0.0;  // long-gap / unknown-state interval time

  // Signal (dBm) and bitrate (bps) statistics over the samples that carried a
  // value, including the population standard deviation for variability. Values
  // from stale/gap ticks that still carried a preserved reading are counted
  // just like fresh ones; a tick without the metric contributes nothing.
  std::size_t signal_sample_count = 0;
  std::optional<double> current_signal_dbm;
  std::optional<double> avg_signal_dbm;
  std::optional<double> min_signal_dbm;
  std::optional<double> max_signal_dbm;
  std::optional<double> signal_stddev_dbm;
  double valid_with_signal = 0.0;  // signal carriers / valid ticks (0..1)
  std::size_t bitrate_sample_count = 0;
  std::optional<double> current_bitrate_bps;
  std::optional<double> avg_bitrate_bps;
  std::optional<double> min_bitrate_bps;
  std::optional<double> max_bitrate_bps;
  std::optional<double> bitrate_stddev_bps;
  double valid_with_bitrate = 0.0;  // bitrate carriers / valid ticks (0..1)

  // Connectivity, derived from consecutive sample pairs: time actually spent
  // connected/disconnected, the longest continuous single run of each, and the
  // time that could not be attributed to either state.
  double connected_seconds = 0.0;
  double disconnected_seconds = 0.0;
  double longest_connected_seconds = 0.0;
  double longest_disconnected_seconds = 0.0;

  // Events from the retained connection-event history (counts by type). These
  // drive the frequency/recovery/availability/roaming factors.
  std::size_t disconnection_count = 0;
  std::size_t reconnection_count = 0;
  std::size_t association_count = 0;
  std::size_t roaming_count = 0;
  std::size_t interface_unavailable_count = 0;
  std::size_t temporary_gap_count = 0;  // retained ticks whose read was invalid

  std::chrono::system_clock::time_point last_update{};
  WirelessStabilityAssessment assessment;
};

/// Computes the Step 52 connection-quality summary for `wireless`. Pure and
/// deterministic: a single bounded pass over the retained sample ring and the
/// retained connection-event ring; never performs I/O, never mutates state.
/// `max_samples` is the same retention bound the monitor and the Step 49/50
/// summary use, so `coverage`/`valid_coverage` are consistent with the rest of
/// the interface details. The name of the game is honesty: unobserved time is
/// never labelled connected/disconnected, and a grade is withheld entirely
/// when the data cannot support one.
[[nodiscard]] WirelessQualitySummary summarizeWirelessQuality(
    const NetworkWirelessInfo &wireless, std::size_t max_samples);

/// Renders the Step 52 quality/stability section for the interface-details
/// view. Pure and unit-testable (mirrors the other renderWireless* helpers):
/// returns a multi-line, pure-ASCII block that also states plainly when the
/// interface is not wireless, when there is no data, when coverage is
/// insufficient for a grade, and why each factor contributed what it did.
[[nodiscard]] std::string renderWirelessQualitySummary(
    const NetworkWirelessInfo &wireless, std::size_t max_samples);

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

/// "866.7 Mb/s" or "1.5 Gb/s" or "54 Mb/s" from the canonical bits-per-second
/// value (decimal 1000-based), or "N/A" when the rate is unknown. The internal
/// unit is always bits per second; only the display scales.
[[nodiscard]] std::string formatWirelessBitrate(const std::optional<double> &bps);

/// "2412 MHz" or "N/A" when the centre frequency is unknown.
[[nodiscard]] std::string formatWirelessFrequency(const std::optional<double> &mhz);

/// "6" or "N/A" when the channel is unknown or not reliably reported.
[[nodiscard]] std::string formatWirelessChannel(const std::optional<int> &channel);

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
/// Limitations (see Step 49/50/51 scope): SSID and BSSID are not available
/// through sysfs or /proc/net/wireless (they require the nl80211 netlink API),
/// so they are intentionally absent (only one-way fingerprints of the AP
/// identity are ever compared, and only booleans are displayed/exported). The
/// negotiated bitrate, centre frequency and channel are likewise not exposed by
/// those native sources and therefore stay unknown (nullopt) in production —
/// the history/event model carries them so the export/summary machinery is
/// complete and deterministically testable. Because no native source exposes an
/// AP identity, a Roamed event is never produced in production; the detection
/// logic is exercised end-to-end through the pure classifier/tracker functions.
/// Connection events (disconnect/reconnect/availability) are forwarded to the
/// central AlertManager and the desktop-notification sink; roaming is by design
/// never notified.
class NetworkWirelessMonitor {
 public:
  static constexpr std::size_t kMaxTrackedWirelessInterfaces = 64;

  explicit NetworkWirelessMonitor(
      AlertManager &alerts, std::filesystem::path root = "/",
      std::chrono::milliseconds reread_interval = kWirelessRereadInterval,
      std::size_t history_max_samples = kDefaultInterfaceHistorySamples);

  NetworkWirelessMonitor(const NetworkWirelessMonitor &) = delete;
  NetworkWirelessMonitor &operator=(const NetworkWirelessMonitor &) = delete;
  ~NetworkWirelessMonitor() = default;

  /// Desktop-notification delivery callback; invoked for connection events
  /// worth surfacing. NotificationManager still applies the global settings
  /// and its own per-source cooldown. Roaming events never reach the sink.
  using EventSink = void (*)(const AlertEvent &);
  void setEventSink(EventSink sink) { event_sink_ = sink; }

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

  /// The wireless sample history (incl. connection events) for an identity;
  /// nullptr when never seen.
  [[nodiscard]] const ResourceHistory<WirelessHistorySample> *wirelessHistory(
      const std::string &identity) const;

  void setHistoryMaxSamples(std::size_t max_samples);
  [[nodiscard]] std::size_t historyMaxSamples() const { return history_max_samples_; }

  void reset();

 private:
  AlertManager &alerts_;
  std::filesystem::path root_;
  std::chrono::milliseconds reread_interval_;
  std::size_t history_max_samples_;
  EventSink event_sink_ = nullptr;
  std::unordered_map<std::string, NetworkWirelessInfo> tracked_;

  void evictOverflow();
  void recordConnectionEvent(const NetworkWirelessInfo &wireless,
                             const WirelessConnectionEvent &event);
};

}  // namespace atm