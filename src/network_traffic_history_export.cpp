#include "network_traffic_history_export.hpp"

#include <errno.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <utility>

#include "process_report.hpp"  // sanitizeReportName (shared filename sanitizer)

namespace atm {

namespace {

constexpr std::string_view kApplicationName = "Arch Task Manager";
constexpr std::string_view kApplicationVersion = "0.1.0";

/// Formats a double deterministically and locale-independently (std::to_chars
/// never uses the active locale, so no thousands separators or comma decimals
/// can corrupt a CSV column). Integral values below 1e15 print as plain
/// integers — cumulative byte counters stay readable; everything else uses the
/// shortest round-trip representation. Non-finite values are never written.
std::string formatExportDouble(double value) {
  if (!std::isfinite(value)) {
    return {};
  }
  char buffer[64];
  if (std::trunc(value) == value && std::abs(value) < 1e15) {
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer),
                                      static_cast<long long>(value));
    return std::string(buffer, result.ptr);
  }
  const auto result =
      std::to_chars(buffer, buffer + sizeof(buffer), value,
                    std::chars_format::general);
  return std::string(buffer, result.ptr);
}

/// RFC-4180-style JSON string escaping of a UTF-8/byte string.
std::string jsonEscape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  out.push_back('"');
  for (const unsigned char c : text) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char escaped[8];
          std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
          out += escaped;
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  out.push_back('"');
  return out;
}

/// JSON number literal for an optional double: the value, or "null" when
/// unavailable (never a misleading zero).
std::string jsonNumber(const std::optional<double> &value) {
  if (!value.has_value()) {
    return "null";
  }
  const std::string formatted = formatExportDouble(*value);
  return formatted.empty() ? "null" : formatted;
}

/// Builds a JSON object literal from ordered key/value members.
std::string jsonObject(
    std::initializer_list<std::pair<std::string, std::string>> members) {
  std::string out = "{";
  bool first = true;
  for (const auto &[key, value] : members) {
    if (!first) {
      out += ',';
    }
    first = false;
    out += jsonEscape(key);
    out += ':';
    out += value;
  }
  out += '}';
  return out;
}

/// Builds a JSON array literal from pre-encoded element strings.
std::string jsonArray(const std::vector<std::string> &elements) {
  std::string out = "[";
  bool first = true;
  for (const std::string &element : elements) {
    if (!first) {
      out += ',';
    }
    first = false;
    out += element;
  }
  out += ']';
  return out;
}

/// Appends one CSV field followed by its separator, writing an empty field for
/// unavailable values (never a fake zero).
void appendCsvField(std::string &out, const std::string &field) {
  out += field;
  out += ',';
}

/// Appends one CSV numeric field for an optional double.
void appendCsvNumber(std::string &out, const std::optional<double> &value) {
  out += value.has_value() ? formatExportDouble(*value) : std::string{};
  out += ',';
}

/// Computes the total transferred during the retained window from the
/// (ordered) cumulative counters of that window. Requires at least two valid
/// readings; refuses to report a total when the cumulative counter reset or
/// wrapped inside the window (any non-monotonic step) or when the result would
/// be negative. Returns std::nullopt in all those cases — never a bogus number.
std::optional<double> retainedWindowTotal(
    const std::vector<double> &cumulative) {
  if (cumulative.size() < 2) {
    return std::nullopt;
  }
  for (std::size_t i = 1; i < cumulative.size(); ++i) {
    if (cumulative[i] < cumulative[i - 1]) {
      return std::nullopt;  // counter reset / wraparound inside the window
    }
  }
  const double delta = cumulative.back() - cumulative.front();
  return delta >= 0.0 ? std::optional<double>(delta) : std::nullopt;
}

}  // namespace

const char *networkTrafficExportFormatName(
    NetworkTrafficExportFormat format) {
  switch (format) {
    case NetworkTrafficExportFormat::Csv:
      return "CSV";
    case NetworkTrafficExportFormat::Json:
      return "JSON";
  }
  return "unknown";
}

std::string formatNetworkTrafficTimestamp(
    std::chrono::system_clock::time_point timestamp) {
  const std::time_t time = std::chrono::system_clock::to_time_t(timestamp);
  std::tm local{};
  if (::localtime_r(&time, &local) == nullptr) {
    return "1970-01-01T00:00:00";
  }
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &local);
  return buffer;
}

std::string escapeNetworkTrafficCsvField(std::string_view field) {
  const bool needs_quoting =
      field.find_first_of(",\"\n\r") != std::string_view::npos;
  if (!needs_quoting) {
    return std::string(field);
  }
  std::string out;
  out.reserve(field.size() + 2);
  out.push_back('"');
  for (const char c : field) {
    if (c == '"') {
      out += "\"\"";
    } else {
      out.push_back(c);
    }
  }
  out.push_back('"');
  return out;
}

NetworkTrafficExportSnapshot buildNetworkTrafficExportSnapshot(
    const NetworkTrafficSeries &series, std::size_t max_samples) {
  NetworkTrafficExportSnapshot snapshot;
  snapshot.aggregate = series.aggregate;
  snapshot.identity = series.identity;
  snapshot.display_name = series.display_name;
  snapshot.last_update = series.last_update;
  snapshot.max_samples = max_samples;

  // Merge every retained ring into one row per unique sample tick. A row is
  // created the first time a tick is seen; each metric is assigned only from
  // the ring that actually sampled it, so a partially populated history (no
  // baseline yet, an unavailable counter, a reset window) shows honest holes
  // rather than fabricated zeros. std::map keeps the rows in ascending tick
  // order without mutating any of the source series' containers.
  std::map<std::chrono::steady_clock::time_point, NetworkTrafficExportRow> rows;

  for (const TimedSample &sample : series.rx_bytes_per_second.samples()) {
    if (!std::isfinite(sample.value)) {
      continue;
    }
    NetworkTrafficExportRow &row = rows[sample.timestamp];
    row.rx_bytes_per_second = sample.value;
    row.rx_bits_per_second = sample.value * 8.0;
  }
  for (const TimedSample &sample : series.tx_bytes_per_second.samples()) {
    if (!std::isfinite(sample.value)) {
      continue;
    }
    NetworkTrafficExportRow &row = rows[sample.timestamp];
    row.tx_bytes_per_second = sample.value;
    row.tx_bits_per_second = sample.value * 8.0;
  }
  for (const TimedSample &sample : series.rx_bytes_total.samples()) {
    if (!std::isfinite(sample.value)) {
      continue;
    }
    rows[sample.timestamp].rx_bytes_total = sample.value;
  }
  for (const TimedSample &sample : series.tx_bytes_total.samples()) {
    if (!std::isfinite(sample.value)) {
      continue;
    }
    rows[sample.timestamp].tx_bytes_total = sample.value;
  }
  for (const TimedSample &sample : series.rx_packets_per_second.samples()) {
    if (!std::isfinite(sample.value)) {
      continue;
    }
    rows[sample.timestamp].rx_packets_per_second = sample.value;
  }
  for (const TimedSample &sample : series.tx_packets_per_second.samples()) {
    if (!std::isfinite(sample.value)) {
      continue;
    }
    rows[sample.timestamp].tx_packets_per_second = sample.value;
  }
  for (const TimedSample &sample : series.rx_errors_per_second.samples()) {
    if (!std::isfinite(sample.value)) {
      continue;
    }
    rows[sample.timestamp].rx_errors_per_second = sample.value;
  }
  for (const TimedSample &sample : series.tx_errors_per_second.samples()) {
    if (!std::isfinite(sample.value)) {
      continue;
    }
    rows[sample.timestamp].tx_errors_per_second = sample.value;
  }
  for (const TimedSample &sample : series.rx_dropped_per_second.samples()) {
    if (!std::isfinite(sample.value)) {
      continue;
    }
    rows[sample.timestamp].rx_dropped_per_second = sample.value;
  }
  for (const TimedSample &sample : series.tx_dropped_per_second.samples()) {
    if (!std::isfinite(sample.value)) {
      continue;
    }
    rows[sample.timestamp].tx_dropped_per_second = sample.value;
  }

  // Assign a consistent wall clock to every row. The newest retained tick is
  // anchored to the series' last update; older ticks shift by their steady
  // offset. This makes all exported timestamps self-consistent and monotonic
  // even though the rings only store ordering time points internally.
  if (!rows.empty()) {
    const auto latest_tick = rows.rbegin()->first;
    snapshot.samples.reserve(rows.size());
    for (const auto &entry : rows) {
      NetworkTrafficExportRow row = entry.second;  // copy, then fix the clock
      const double offset_seconds =
          std::chrono::duration<double>(latest_tick - entry.first).count();
      row.timestamp =
          series.last_update -
          std::chrono::duration_cast<std::chrono::system_clock::duration>(
              std::chrono::duration<double>(offset_seconds));
      snapshot.samples.push_back(std::move(row));
    }
  }

  // Summary values.
  NetworkTrafficExportSummary &summary = snapshot.summary;
  summary.sample_count = snapshot.samples.size();
  if (max_samples > 0) {
    summary.coverage =
        std::min(1.0, static_cast<double>(summary.sample_count) /
                          static_cast<double>(max_samples));
  }
  summary.history_complete = summary.sample_count >= max_samples;
  summary.last_update = series.last_update;
  summary.totals_approximate = series.aggregate && series.membership_changed;

  for (auto it = snapshot.samples.rbegin(); it != snapshot.samples.rend();
       ++it) {
    if (!summary.current_rx_bytes_per_second.has_value() &&
        it->rx_bytes_per_second.has_value()) {
      summary.current_rx_bytes_per_second = it->rx_bytes_per_second;
    }
    if (!summary.current_tx_bytes_per_second.has_value() &&
        it->tx_bytes_per_second.has_value()) {
      summary.current_tx_bytes_per_second = it->tx_bytes_per_second;
    }
  }
  for (const NetworkTrafficExportRow &row : snapshot.samples) {
    if (row.rx_bytes_per_second.has_value()) {
      summary.peak_rx_bytes_per_second =
          std::max(summary.peak_rx_bytes_per_second.value_or(0.0),
                   *row.rx_bytes_per_second);
    }
    if (row.tx_bytes_per_second.has_value()) {
      summary.peak_tx_bytes_per_second =
          std::max(summary.peak_tx_bytes_per_second.value_or(0.0),
                   *row.tx_bytes_per_second);
    }
  }

  std::vector<double> rx_cumulative;
  std::vector<double> tx_cumulative;
  rx_cumulative.reserve(snapshot.samples.size());
  tx_cumulative.reserve(snapshot.samples.size());
  for (const NetworkTrafficExportRow &row : snapshot.samples) {
    if (row.rx_bytes_total.has_value()) {
      rx_cumulative.push_back(*row.rx_bytes_total);
    }
    if (row.tx_bytes_total.has_value()) {
      tx_cumulative.push_back(*row.tx_bytes_total);
    }
  }
  summary.total_rx_bytes = retainedWindowTotal(rx_cumulative);
  summary.total_tx_bytes = retainedWindowTotal(tx_cumulative);

  // Sampling interval and retention span (median of the merged-tick deltas).
  if (snapshot.samples.size() >= 2) {
    std::vector<double> deltas;
    deltas.reserve(snapshot.samples.size() - 1);
    for (std::size_t i = 1; i < snapshot.samples.size(); ++i) {
      deltas.push_back(std::chrono::duration<double>(
                           snapshot.samples[i].timestamp -
                           snapshot.samples[i - 1].timestamp)
                           .count());
    }
    std::sort(deltas.begin(), deltas.end());
    const double median =
        deltas.size() % 2 == 1
            ? deltas[deltas.size() / 2]
            : (deltas[deltas.size() / 2 - 1] + deltas[deltas.size() / 2]) /
                  2.0;
    snapshot.sampling_interval_seconds = median;
    snapshot.retention_window_seconds = std::chrono::duration<double>(
        snapshot.samples.back().timestamp - snapshot.samples.front().timestamp)
                                            .count();
  }
  return snapshot;
}

std::string generateNetworkTrafficCsv(
    const NetworkTrafficExportSnapshot &snapshot) {
  static constexpr std::string_view kHeader =
      "timestamp,interface,identity,rx_bytes_per_second,"
      "tx_bytes_per_second,rx_bits_per_second,tx_bits_per_second,"
      "rx_bytes_total,tx_bytes_total,rx_packets_per_second,"
      "tx_packets_per_second,rx_errors_per_second,tx_errors_per_second,"
      "rx_dropped_per_second,tx_dropped_per_second\n";

  std::string out;
  out.reserve(kHeader.size() + snapshot.samples.size() * 96u);
  out += kHeader;

  const std::string interface =
      escapeNetworkTrafficCsvField(snapshot.display_name);
  const std::string identity = escapeNetworkTrafficCsvField(snapshot.identity);
  for (const NetworkTrafficExportRow &row : snapshot.samples) {
    appendCsvField(out,
                   escapeNetworkTrafficCsvField(
                       formatNetworkTrafficTimestamp(row.timestamp)));
    appendCsvField(out, interface);
    appendCsvField(out, identity);
    appendCsvNumber(out, row.rx_bytes_per_second);
    appendCsvNumber(out, row.tx_bytes_per_second);
    appendCsvNumber(out, row.rx_bits_per_second);
    appendCsvNumber(out, row.tx_bits_per_second);
    appendCsvNumber(out, row.rx_bytes_total);
    appendCsvNumber(out, row.tx_bytes_total);
    appendCsvNumber(out, row.rx_packets_per_second);
    appendCsvNumber(out, row.tx_packets_per_second);
    appendCsvNumber(out, row.rx_errors_per_second);
    appendCsvNumber(out, row.tx_errors_per_second);
    appendCsvNumber(out, row.rx_dropped_per_second);
    appendCsvNumber(out, row.tx_dropped_per_second);
    if (!out.empty() && out.back() == ',') {
      out.pop_back();  // drop the trailing separator (13 fields -> 12 commas)
    }
    out += '\n';
  }
  return out;
}

std::string generateNetworkTrafficJson(
    const NetworkTrafficExportSnapshot &snapshot) {
  const std::string created_at =
      formatNetworkTrafficTimestamp(std::chrono::system_clock::now());

  const NetworkTrafficExportSummary &summary = snapshot.summary;
  std::string start_timestamp = "null";
  std::string end_timestamp = "null";
  if (!snapshot.samples.empty()) {
    start_timestamp = jsonEscape(
        formatNetworkTrafficTimestamp(snapshot.samples.front().timestamp));
    end_timestamp = jsonEscape(
        formatNetworkTrafficTimestamp(snapshot.samples.back().timestamp));
  }

  const std::string interface = jsonObject({
      {"identity", jsonEscape(snapshot.identity)},
      {"name", jsonEscape(snapshot.display_name)},
      {"aggregate", snapshot.aggregate ? "true" : "false"},
  });

  const std::string history = jsonObject({
      {"start_timestamp", start_timestamp},
      {"end_timestamp", end_timestamp},
      {"sample_count", std::to_string(summary.sample_count)},
      {"max_samples", std::to_string(snapshot.max_samples)},
      {"coverage", jsonNumber(std::optional<double>(summary.coverage))},
      {"complete", summary.history_complete ? "true" : "false"},
      {"sampling_interval_seconds",
       jsonNumber(snapshot.sampling_interval_seconds)},
      {"retention_window_seconds",
       jsonNumber(std::optional<double>(snapshot.retention_window_seconds))},
  });

  const std::string units = jsonObject({
      {"byte_rate", jsonEscape("bytes per second")},
      {"bit_rate",
       jsonEscape("bits per second (derived as 8 x bytes per second)")},
      {"packet_rate", jsonEscape("packets per second")},
      {"cumulative_bytes", jsonEscape("bytes (kernel cumulative counters)")},
  });

  const std::string summary_json = jsonObject({
      {"current_rx_bytes_per_second",
       jsonNumber(summary.current_rx_bytes_per_second)},
      {"current_tx_bytes_per_second",
       jsonNumber(summary.current_tx_bytes_per_second)},
      {"peak_rx_bytes_per_second", jsonNumber(summary.peak_rx_bytes_per_second)},
      {"peak_tx_bytes_per_second", jsonNumber(summary.peak_tx_bytes_per_second)},
      {"total_rx_bytes", jsonNumber(summary.total_rx_bytes)},
      {"total_tx_bytes", jsonNumber(summary.total_tx_bytes)},
      {"totals_approximate", summary.totals_approximate ? "true" : "false"},
      {"history_complete", summary.history_complete ? "true" : "false"},
      {"last_update",
       jsonEscape(formatNetworkTrafficTimestamp(summary.last_update))},
  });

  std::vector<std::string> sample_objects;
  sample_objects.reserve(snapshot.samples.size());
  for (const NetworkTrafficExportRow &row : snapshot.samples) {
    sample_objects.push_back(jsonObject({
        {"timestamp", jsonEscape(formatNetworkTrafficTimestamp(row.timestamp))},
        {"rx_bytes_per_second", jsonNumber(row.rx_bytes_per_second)},
        {"tx_bytes_per_second", jsonNumber(row.tx_bytes_per_second)},
        {"rx_bits_per_second", jsonNumber(row.rx_bits_per_second)},
        {"tx_bits_per_second", jsonNumber(row.tx_bits_per_second)},
        {"rx_bytes_total", jsonNumber(row.rx_bytes_total)},
        {"tx_bytes_total", jsonNumber(row.tx_bytes_total)},
        {"rx_packets_per_second", jsonNumber(row.rx_packets_per_second)},
        {"tx_packets_per_second", jsonNumber(row.tx_packets_per_second)},
        {"rx_errors_per_second", jsonNumber(row.rx_errors_per_second)},
        {"tx_errors_per_second", jsonNumber(row.tx_errors_per_second)},
        {"rx_dropped_per_second", jsonNumber(row.rx_dropped_per_second)},
        {"tx_dropped_per_second", jsonNumber(row.tx_dropped_per_second)},
    }));
  }

  return jsonObject({
      {"export_metadata",
       jsonObject({
           {"application", jsonEscape(kApplicationName)},
           {"application_version", jsonEscape(kApplicationVersion)},
           {"format", jsonEscape("json")},
           {"created_at", jsonEscape(created_at)},
       })},
      {"interface", interface},
      {"history", history},
      {"units", units},
      {"summary", summary_json},
      {"samples", jsonArray(sample_objects)},
  });
}

const char *networkTrafficExportStatusMessage(
    NetworkTrafficExportStatus status) {
  switch (status) {
    case NetworkTrafficExportStatus::Success:
      return "exported successfully";
    case NetworkTrafficExportStatus::EmptyHistory:
      return "no network traffic history samples to export for this selection";
    case NetworkTrafficExportStatus::UnsupportedFormat:
      return "unsupported export format";
    case NetworkTrafficExportStatus::InvalidPath:
      return "the destination path is not a usable file";
    case NetworkTrafficExportStatus::WriteError:
      return "failed to write the export file";
  }
  return "unknown export status";
}

NetworkTrafficExportResult writeNetworkTrafficExport(
    const std::string &path, const std::string &contents) {
  NetworkTrafficExportResult result;
  if (path.empty()) {
    result.status = NetworkTrafficExportStatus::InvalidPath;
    result.errno_value = EINVAL;
    return result;
  }

  std::error_code ec;
  if (std::filesystem::is_directory(path, ec) && !ec) {
    result.status = NetworkTrafficExportStatus::InvalidPath;
    result.errno_value = EISDIR;
    return result;
  }

  const std::filesystem::path tmp =
      std::filesystem::path(path).string() + ".tmp";

  {
    std::ofstream out(tmp,
                      std::ios::binary | std::ios::out | std::ios::trunc);
    if (!out) {
      result.status = NetworkTrafficExportStatus::WriteError;
      result.errno_value = errno != 0 ? errno : EIO;
      return result;
    }
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    out.flush();
    if (!out) {
      result.status = NetworkTrafficExportStatus::WriteError;
      result.errno_value = errno != 0 ? errno : EIO;
      std::filesystem::remove(tmp, ec);
      return result;
    }
  }

  ec.clear();
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    result.status = NetworkTrafficExportStatus::WriteError;
    result.errno_value = errno != 0 ? errno : EIO;
    std::filesystem::remove(tmp, ec);
    return result;
  }

  result.status = NetworkTrafficExportStatus::Success;
  result.errno_value = 0;
  result.bytes = contents.size();
  result.path = path;
  return result;
}

NetworkTrafficExportResult exportNetworkTrafficHistory(
    const std::string &path, const NetworkTrafficExportSnapshot &snapshot,
    NetworkTrafficExportFormat format) {
  if (format != NetworkTrafficExportFormat::Csv &&
      format != NetworkTrafficExportFormat::Json) {
    return {NetworkTrafficExportStatus::UnsupportedFormat, 0, 0, {}};
  }
  if (snapshot.samples.empty()) {
    return {NetworkTrafficExportStatus::EmptyHistory, 0, 0, {}};
  }
  const std::string contents =
      format == NetworkTrafficExportFormat::Csv
          ? generateNetworkTrafficCsv(snapshot)
          : generateNetworkTrafficJson(snapshot);
  NetworkTrafficExportResult result = writeNetworkTrafficExport(path, contents);
  if (result.status == NetworkTrafficExportStatus::Success) {
    result.path = path;
  }
  return result;
}

std::string defaultNetworkTrafficExportFilename(
    const std::string &identity, const std::string &display_name,
    NetworkTrafficExportFormat format) {
  std::string base = "network-traffic";
  if (identity == std::string(kNetworkTrafficAllIdentity)) {
    base += "-all";
  } else {
    base += "-" + sanitizeReportName(display_name);
  }
  base += format == NetworkTrafficExportFormat::Csv ? ".csv" : ".json";
  return base;
}

}  // namespace atm