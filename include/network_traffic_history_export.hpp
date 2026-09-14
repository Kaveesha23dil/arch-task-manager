#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "network_traffic_history.hpp"

namespace atm {

struct NetworkLinkMetrics;        // link speed/duplex monitor state (export metadata)
struct NetworkInterfaceHardware;   // hardware/driver monitor state (export metadata)
struct NetworkWirelessInfo;        // wireless monitor state (export metadata)
struct TrackedInterface;           // link-state monitor record (export metadata)

/// Supported network-traffic-history export formats. CSV and JSON are the only
/// two: both are plain-text, self-describing and easy to inspect or import.
enum class NetworkTrafficExportFormat { Csv, Json };

/// Human-readable name of an export format ("CSV" / "JSON").
[[nodiscard]] const char *networkTrafficExportFormatName(
    NetworkTrafficExportFormat format);

/// One exported history row for a single sample tick.
///
/// The wall-clock `timestamp` is anchored to the series' last update so every
/// exported row is self-consistent even though the retained rings only store
/// steady-clock ordering time points. A std::nullopt metric means the value
/// was unavailable for that tick (missing counter, unsafe rate window, reset)
/// and is never a fake zero. Bit rates are derived as 8 * bytes-per-second —
/// the model itself stores byte rates only, so this conversion is documented
/// rather than hidden.
struct NetworkTrafficExportRow {
  std::chrono::system_clock::time_point timestamp{};
  std::optional<double> rx_bytes_per_second;
  std::optional<double> tx_bytes_per_second;
  std::optional<double> rx_bits_per_second;
  std::optional<double> tx_bits_per_second;
  std::optional<double> rx_bytes_total;   // cumulative kernel counter
  std::optional<double> tx_bytes_total;   // cumulative kernel counter
  std::optional<double> rx_packets_per_second;
  std::optional<double> tx_packets_per_second;
  std::optional<double> rx_errors_per_second;
  std::optional<double> tx_errors_per_second;
  std::optional<double> rx_dropped_per_second;
  std::optional<double> tx_dropped_per_second;
};

/// Summary values associated with an exported series — the same numbers the
/// traffic-history view shows (current/peak rates, retained-window totals,
/// coverage). Computed from the retained rings by the snapshot builder.
struct NetworkTrafficExportSummary {
  std::optional<double> current_rx_bytes_per_second;
  std::optional<double> current_tx_bytes_per_second;
  std::optional<double> peak_rx_bytes_per_second;
  std::optional<double> peak_tx_bytes_per_second;
  std::optional<double> total_rx_bytes;  // difference of the retained window
  std::optional<double> total_tx_bytes;
  std::size_t sample_count = 0;
  double coverage = 0.0;          // sample_count / max_samples (0..1)
  bool history_complete = false;  // the ring filled up to the retention bound
  bool totals_approximate = false;  // aggregate members changed in the window
  std::chrono::system_clock::time_point last_update{};
};

/// Link speed/duplex metadata serialized alongside one exported series when the
/// monitoring layer can attribute it to a single physical interface. Absent for
/// the aggregate and for non-physical interfaces — never a fake zero or a stale
/// firmware value.
struct NetworkTrafficExportLink {
  bool physical = false;             // is a physical Ethernet/Wi-Fi/InfiniBand
  bool link_active = false;          // availability == Connected at export time
  std::string speed_state;           // "valid" / "stale" / "unavailable" / ...
  std::optional<int> speed_mbps;     // last successfully reported value, if any
  std::string speed_unit;            // the sysfs unit, "Mb/s"
  std::string duplex;                // "full" / "half" / "unknown"
  std::string duplex_state;          // "valid" / "stale" / "unavailable" / ...
  std::chrono::system_clock::time_point last_update{};  // fresh-read time
};

/// Hardware/driver metadata serialized alongside one exported series when it
/// maps to a single interface. Series-scoped (one value per export, never
/// repeated per sample in the JSON form); unavailable values follow the export
/// conventions (empty CSV fields, null JSON values) — never fake zeros.
struct NetworkTrafficExportHardware {
  std::string interface_type;    // "Ethernet" / "Wifi" / ... ("" when absent)
  std::string hardware_class;    // "physical" / "virtual" / "unknown"
  bool device_related = false;   // the interface resolves to a device directory
  std::string device_bus;        // "pci" / "usb" / "platform" / ... ("" absent)
  std::string device_id;         // raw sysfs dev_id value ("" when absent)
  std::string driver;            // basename of the bound driver ("" when absent)
  std::string driver_state;      // "available" / "stale" / "unavailable" / ...
  std::string mac_address;       // formatted sysfs address ("" when unavailable)
  std::optional<int> ifindex;    // stable kernel interface index
  std::optional<int> name_assign_type;  // raw sysfs name_assign_type
  std::string carrier_state;     // "yes" / "no carrier" / "unknown"
  std::string oper_state;        // "up" / "down" / ... / "unknown"
  std::string availability;      // "Connected" / "Down" / "Unknown" / ...
  std::chrono::system_clock::time_point last_read{};  // metadata probe time
};

/// Wireless metadata serialized alongside one exported series when it maps to a
/// single wireless interface. Series-scoped (one value per export, never
/// repeated per sample in the JSON form). Only values obtainable from the
/// kernel's native sysfs / /proc/net/wireless interfaces are exported; the
/// kernel does not expose SSID/BSSID/channel/frequency/bitrate via those
/// interfaces (they require nl80211), so those fields are intentionally absent
/// from the export model — never guessed, never fabricated. No credentials or
/// network keys are ever included.
struct NetworkTrafficExportWireless {
  std::string interface_name;  // current kernel name
  std::string presence;        // "wireless" / "not wireless" / "unknown"
  std::string phy_name;        // "phy0" ("" when absent)
  std::optional<int> phy_index;
  std::string phy_state;       // "available" / "stale" / "unavailable" / ...
  bool is_mac80211 = false;    // phy80211 present -> the /70 quality-scale note
  std::optional<int> link_quality;  // raw WIRELESS_EXT quality (driver scale)
  std::optional<int> signal_dbm;    // received signal strength, dBm
  std::optional<int> noise_dbm;     // noise floor, dBm
  std::string association;          // "associated" / "disconnected" / "unknown"
  bool enabled = false;             // administrative state
  std::string carrier_state;        // "yes" / "no carrier" / "unknown"
  std::string field_state;          // "available" / "stale" / "unavailable"
  std::chrono::system_clock::time_point last_read{};
};

/// One exported wireless-history sample row (Step 50). Mirrors the live
/// WirelessHistorySample model: every metric is an explicit null when it was
/// unavailable for that tick — never a fabricated zero. `valid` marks ticks
/// whose dynamic fields were freshly read (invalid ticks create honest gaps,
/// never interpolated values). The wall-clock `timestamp` is anchored to the
/// wireless history's newest sample the same way the traffic rows anchor to the
/// series' last update.
struct NetworkTrafficExportWirelessRow {
  std::chrono::system_clock::time_point timestamp{};
  std::string association;          // "associated" / "disconnected" / "unknown"
  bool valid = false;
  std::optional<double> signal_dbm;
  std::optional<int> link_quality;
  std::string link_quality_scale;   // "mac80211 /70" / "driver-defined" / ""
  std::optional<double> bitrate_bps;
  std::optional<double> frequency_mhz;
  std::optional<int> channel;
};

/// One exported wireless connection event (Step 51). Mirrors the live
/// WirelessConnectionEvent: the wall-clock display time, the stable lowercase
/// event name, the confidence flag, the carrier/presence source and — for
/// transitions between associations — the before/after association, signal,
/// frequency and channel. Access-point identity is never exported: only the
/// two booleans (fingerprint changed while an identity was reliably reported)
/// describe a roam.
struct NetworkTrafficExportWirelessEvent {
  std::string timestamp_iso8601;   // wall-clock display time ("" when unknown)
  std::string interface_name;
  std::string event;               // wirelessConnectionEventTypeName()
  bool confident = false;
  std::string source;              // "carrier" / "presence" / "ap_identity"
  std::string previous_association;  // wirelessAssociationName()
  std::string new_association;
  std::optional<double> previous_signal_dbm;
  std::optional<double> new_signal_dbm;
  std::optional<double> previous_frequency_mhz;
  std::optional<double> new_frequency_mhz;
  std::optional<int> previous_channel;
  std::optional<int> new_channel;
  bool access_point_changed = false;  // roam flagged an AP fingerprint change
  bool access_point_reliable = false; // the AP identity was reliably reported
};

/// Wireless history (summary + per-sample rows) captured into an export
/// snapshot. Only present when the selected interface is wireless and its
/// history ring holds recorded samples. The numbers match — and never exceed —
/// what the wireless-history view displays: signal/quality per-metric basic
/// statistics are computed over the retained valid samples only.
struct NetworkTrafficExportWirelessHistory {
  std::size_t sample_count = 0;
  std::size_t valid_sample_count = 0;
  double coverage = 0.0;
  bool history_complete = false;
  double span_seconds = 0.0;

  std::optional<double> current_signal_dbm;
  std::optional<double> min_signal_dbm;
  std::optional<double> max_signal_dbm;
  std::optional<double> avg_signal_dbm;

  std::optional<int> current_link_quality;
  std::optional<double> current_bitrate_bps;
  std::optional<double> current_frequency_mhz;
  std::optional<int> current_channel;

  std::chrono::system_clock::time_point last_update{};
  std::vector<NetworkTrafficExportWirelessRow> samples;  // ascending order

  // Connection events (Step 51). Like the traffic rows these are anchored to
  // the wireless history's newest sample; every event in the ring is exported
  // (the ring is bounded at kMaxWirelessConnectionEvents). Only the boolean
  // AP-change/guarantee flags travel through the export, never AP identity.
  std::vector<NetworkTrafficExportWirelessEvent> events;  // ascending order
};

/// Implementation-free, immutable snapshot of one traffic series ready for
/// serialization. Contains only plain data — no pointers, no mutexes, no UI
/// objects and no internal bookkeeping. Building this snapshot before
/// serialization guarantees every exported array uses one consistent set of
/// timestamps and that the live history containers are never iterated while a
/// refresh updates them.
struct NetworkTrafficExportSnapshot {
  bool aggregate = false;
  std::string identity;      // "all" or "idx:<ifindex>"/"name:<name>"
  std::string display_name;  // "All interfaces" or the interface name
  std::chrono::system_clock::time_point last_update{};
  std::size_t max_samples = 0;                     // retention bound
  std::optional<double> sampling_interval_seconds;  // median delta, if >=2 rows
  double retention_window_seconds = 0.0;            // first..last sample span
  NetworkTrafficExportSummary summary;
  std::vector<NetworkTrafficExportRow> samples;  // ascending timestamp order

  // Link speed/duplex metadata when the exported series maps to a single
  // physical interface (std::nullopt for the aggregate / non-physical links).
  std::optional<NetworkTrafficExportLink> link;

  // Hardware/driver metadata when the exported series maps to a single
  // interface (std::nullopt for the aggregate / when no hardware was captured).
  std::optional<NetworkTrafficExportHardware> hardware;

  // Wireless metadata when the exported series maps to a single wireless
  // interface (std::nullopt for the aggregate / non-wireless interfaces).
  std::optional<NetworkTrafficExportWireless> wireless;

  // Wireless history when the exported series maps to a wireless interface with
  // recorded samples (std::nullopt otherwise). The values match what the
  // wireless-history view displays — see NetworkTrafficExportWirelessHistory.
  std::optional<NetworkTrafficExportWirelessHistory> wireless_history;
};

/// Builds a stable snapshot of `series` for export. Reads only the series'
/// retained ring buffers, merges a sample per unique tick (never duplicates a
/// timestamp, never mixes identities), anchors wall-clock timestamps to the
/// series' last update, and computes the summary. Pure and read-only: the
/// series is never mutated. `max_samples` is the current retention bound (used
/// for coverage/completeness); pass NetworkTrafficHistory::historyMaxSamples().
/// When `link_metrics` points to the selected interface's metrics they are
/// captured into snapshot.link; when `hardware` points at the selected
/// interface's hardware/driver metadata it is captured into snapshot.hardware
/// together with the underlying `info` (interface index, type name, MAC) and
/// `link_state` (carrier/operational state). When `wireless` points at the
/// selected interface's wireless record (and that record is a wireless
/// interface) it is captured into snapshot.wireless, and when its history ring
/// holds recorded samples those are captured into snapshot.wireless_history
/// (per-sample rows anchored to the newest sample plus the summary numbers the
/// wireless-history view shows). All parameters are additive — existing fields
/// are unchanged and any missing one stays unavailable.
[[nodiscard]] NetworkTrafficExportSnapshot buildNetworkTrafficExportSnapshot(
    const NetworkTrafficSeries &series, std::size_t max_samples,
    const NetworkLinkMetrics *link_metrics = nullptr,
    const NetworkInterfaceHardware *hardware = nullptr,
    const NetworkWirelessInfo *wireless = nullptr,
    const NetworkInterfaceInfo *info = nullptr,
    const TrackedInterface *link_state = nullptr);

/// Formats a wall-clock time point as ISO-8601 local time
/// "YYYY-MM-DDTHH:MM:SS". This is the timestamp format used by every export
/// (CSV rows, JSON history/summary/sample timestamps).
[[nodiscard]] std::string formatNetworkTrafficTimestamp(
    std::chrono::system_clock::time_point timestamp);

/// RFC-4180-style field escaping for CSV: the field is quoted only when it
/// contains a comma, quote, CR or NL; quotes inside are doubled. Never throws.
[[nodiscard]] std::string escapeNetworkTrafficCsvField(std::string_view field);

/// Serializes a snapshot to CSV with a stable column order:
/// timestamp, interface, identity, rx/tx bytes per second, rx/tx bits per
/// second, cumulative rx/tx bytes, rx/tx packets per second, rx/tx errors per
/// second, rx/tx drops per second, the trailing link_speed/duplex columns, the
/// hardware metadata columns and — when wireless metadata was captured with the
/// snapshot — the trailing wireless presence/phy/signal/noise/association/
/// state columns. All metadata columns are append-only additions, so the
/// leading historical columns are unchanged; without captured metadata the
/// trailing columns are empty. When a wireless history was captured it is
/// appended as a clearly-headed second section (its own header, one row per
/// wireless sample and a prefixed summary line). Unavailable metrics are empty
/// fields (never misleading zeros); headers are always emitted, so a snapshot
/// with no rows still produces a valid, readable CSV.
[[nodiscard]] std::string generateNetworkTrafficCsv(
    const NetworkTrafficExportSnapshot &snapshot);

/// Serializes a snapshot to valid JSON with stable field names: export
/// metadata, interface identity/name/aggregate flag, an optional "link" object
/// (link speed/duplex metadata, null when absent), an optional "hardware"
/// object (interface index, MAC, type, physical/virtual classification,
/// driver/device relationships and carrier/operational state, null when
/// absent), an optional "wireless" object (wireless presence, PHY, signal/noise
/// dBm, raw link quality, carrier-derived association and freshness, null when
/// absent), an optional "wireless_history" object (per-sample rows plus the
/// signal/quality summary the wireless-history view shows, null when absent)
/// and history span and sampling interval, unit information, the summary
/// values and the sample array. Unavailable values are null; an empty sample
/// array is valid; no internal implementation state is ever serialized.
[[nodiscard]] std::string generateNetworkTrafficJson(
    const NetworkTrafficExportSnapshot &snapshot);

/// Outcome of one network-traffic-history export attempt.
enum class NetworkTrafficExportStatus {
  Success,
  EmptyHistory,       // the selected series retains no samples to export
  UnsupportedFormat,  // the format enum is out of range
  InvalidPath,        // the destination path is empty or not a usable file
  WriteError,         // open / write / flush / rename failed (errno_value set)
};

/// Result of one network-traffic-history export attempt.
struct NetworkTrafficExportResult {
  NetworkTrafficExportStatus status = NetworkTrafficExportStatus::EmptyHistory;
  int errno_value = 0;    // original errno for WriteError / InvalidPath
  std::size_t bytes = 0;  // bytes written to disk
  std::string path;       // destination actually written (on Success)
};

/// Human-readable description of a NetworkTrafficExportStatus.
[[nodiscard]] const char *networkTrafficExportStatusMessage(
    NetworkTrafficExportStatus status);

/// Atomically writes `contents` to `path`: temporary file in the same
/// directory, write, flush, close, rename. A crash never leaves a partially
/// written export and an existing file is replaced only by the atomic rename
/// (an all-or-nothing replacement). Uses only native <fstream>/<filesystem>
/// APIs — no shell commands or subprocesses. `errno_value` is set on failure.
/// Never throws.
[[nodiscard]] NetworkTrafficExportResult writeNetworkTrafficExport(
    const std::string &path, const std::string &contents);

/// Performs a complete export: serializes `snapshot` in `format` and atomically
/// writes the result to `path`. Returns EmptyHistory when the snapshot has no
/// samples and UnsupportedFormat for an invalid format; otherwise the write
/// outcome. Never reads the network, never touches the history source.
[[nodiscard]] NetworkTrafficExportResult exportNetworkTrafficHistory(
    const std::string &path, const NetworkTrafficExportSnapshot &snapshot,
    NetworkTrafficExportFormat format);

/// Builds the default export filename, e.g. "network-traffic-eth0.csv" or
/// "network-traffic-all.json" for the aggregate. The display name is passed
/// through the shared report-name sanitizer so a hostile or unusual interface
/// name can never traverse paths; the result is always a plain filename.
[[nodiscard]] std::string defaultNetworkTrafficExportFilename(
    const std::string &identity, const std::string &display_name,
    NetworkTrafficExportFormat format);

}  // namespace atm