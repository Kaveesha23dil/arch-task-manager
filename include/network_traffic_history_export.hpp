#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "network_traffic_history.hpp"

namespace atm {

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
};

/// Builds a stable snapshot of `series` for export. Reads only the series'
/// retained ring buffers, merges a sample per unique tick (never duplicates a
/// timestamp, never mixes identities), anchors wall-clock timestamps to the
/// series' last update, and computes the summary. Pure and read-only: the
/// series is never mutated. `max_samples` is the current retention bound (used
/// for coverage/completeness); pass NetworkTrafficHistory::historyMaxSamples().
[[nodiscard]] NetworkTrafficExportSnapshot buildNetworkTrafficExportSnapshot(
    const NetworkTrafficSeries &series, std::size_t max_samples);

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
/// second, rx/tx drops per second. Unavailable metrics are empty fields (never
/// misleading zeros); headers are always emitted, so a snapshot with no rows
/// still produces a valid, readable CSV.
[[nodiscard]] std::string generateNetworkTrafficCsv(
    const NetworkTrafficExportSnapshot &snapshot);

/// Serializes a snapshot to valid JSON with stable field names: export
/// metadata, interface identity/name/aggregate flag, history span and sampling
/// interval, unit information, the summary values and the sample array.
/// Unavailable values are null; an empty sample array is valid; no internal
/// implementation state is ever serialized.
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