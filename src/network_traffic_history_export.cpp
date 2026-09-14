#include "network_traffic_history_export.hpp"

#include <errno.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <utility>

#include "network_interface_hardware.hpp"
#include "network_link_metrics.hpp"
#include "network_link_state.hpp"
#include "network_wireless.hpp"
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

/// JSON number literal for an optional integer (the reported link speed in
/// Mb/s), or "null" when unavailable.
std::string jsonNumber(const std::optional<int> &value) {
  if (!value.has_value()) {
    return "null";
  }
  return std::to_string(*value);
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
    const NetworkTrafficSeries &series, std::size_t max_samples,
    const NetworkLinkMetrics *link_metrics,
    const NetworkInterfaceHardware *hardware,
    const NetworkWirelessInfo *wireless,
    const NetworkInterfaceInfo *info,
    const TrackedInterface *link_state) {
  NetworkTrafficExportSnapshot snapshot;
  snapshot.aggregate = series.aggregate;
  snapshot.identity = series.identity;
  snapshot.display_name = series.display_name;
  snapshot.last_update = series.last_update;
  snapshot.max_samples = max_samples;

  // Capture the link speed/duplex metadata (if the monitoring layer provided
  // metrics for the selected interface) before any stream is touched.
  if (link_metrics != nullptr) {
    NetworkTrafficExportLink link;
    link.physical = link_metrics->physical;
    link.link_active = link_metrics->link_active;
    link.speed_state = networkSpeedStateName(link_metrics->speed_state);
    link.speed_mbps = link_metrics->speed_mbps;
    link.speed_unit = "Mb/s";  // the unit the kernel reports "speed" in
    link.duplex = networkDuplexModeName(link_metrics->duplex);
    link.duplex_state = networkDuplexStateName(link_metrics->duplex_state);
    link.last_update = link_metrics->last_update;
    snapshot.link = link;
  }

  // Capture the hardware/driver metadata (if the monitoring layer provided a
  // record for the selected interface) before any stream is touched. The
  // interface type, MAC and index come from the details snapshot; carrier and
  // operational state from the link-state record; everything else from the
  // hardware monitor's cached sysfs metadata.
  if (hardware != nullptr && info != nullptr) {
    NetworkTrafficExportHardware hw;
    hw.interface_type = networkInterfaceTypeName(info->type);
    hw.hardware_class = networkDeviceKindName(hardware->device_kind);
    hw.device_related = hardware->device_related;
    hw.device_bus = hardware->device_bus;
    hw.device_id = hardware->device_id;
    hw.driver = hardware->driver;
    hw.driver_state = networkHardwareStateName(hardware->driver_state);
    hw.mac_address = formatMacAddress(info->link.mac_address);
    hw.ifindex = info->link.ifindex;
    hw.name_assign_type = hardware->name_assign_type;
    hw.last_read = hardware->last_read;

    if (link_state != nullptr) {
      hw.carrier_state = networkCarrierStateName(link_state->carrier);
      hw.oper_state = networkOperStateName(link_state->oper);
      hw.availability =
          networkLinkAvailabilityName(link_state->availability());
    } else {
      hw.carrier_state = networkCarrierStateName(NetworkCarrierState::Unknown);
      hw.oper_state = networkOperStateName(NetworkOperState::Unknown);
      hw.availability =
          networkLinkAvailabilityName(NetworkLinkAvailability::Unknown);
    }
    snapshot.hardware = hw;
  }

  // Capture the wireless metadata (when the monitoring layer provided a record
  // for the selected interface and that record is a wireless interface). Only
  // values obtainable from the native sysfs tree / /proc/net/wireless are
  // exported; the kernel does not expose SSID/BSSID/channel/frequency/bitrate
  // through those interfaces (they require nl80211), so they are never guessed.
  if (wireless != nullptr && wireless->present &&
      wireless->presence == WirelessPresence::Wireless) {
    NetworkTrafficExportWireless wl;
    wl.interface_name = wireless->name;
    wl.presence = wirelessPresenceName(wireless->presence);
    wl.phy_name = wireless->phy_name;
    wl.phy_index = wireless->phy_index;
    wl.phy_state = wirelessFieldStateName(wireless->phy_state);
    wl.is_mac80211 = wireless->is_mac80211;
    wl.link_quality = wireless->link_quality;
    wl.signal_dbm = wireless->signal_dbm;
    wl.noise_dbm = wireless->noise_dbm;
    wl.association = wirelessAssociationName(wireless->association);
    wl.enabled = wireless->enabled;
    wl.carrier_state = wireless->carrier_exposed
                           ? (wireless->has_carrier ? "yes" : "no carrier")
                           : "unknown";
    wl.field_state = wirelessFieldStateName(wireless->field_state);
    wl.last_read = wireless->last_read;
    snapshot.wireless = wl;
  }

  // Capture the wireless history (summary + per-sample rows) when the selected
  // interface is wireless and its ring holds recorded samples. Timestamps are
  // anchored to the newest recorded sample's wall clock (last_sample_wall) the
  // same way the traffic rows anchor to the series' last update; per-metric
  // statistics are computed only over samples that carried each value, so
  // invalid/stale gaps contribute nothing and never read as a fabricated zero.
  if (wireless != nullptr && wireless->present &&
      wireless->presence == WirelessPresence::Wireless &&
      (!wireless->history.empty() || !wireless->connection_events.empty())) {
    NetworkTrafficExportWirelessHistory wh;
    const auto &samples = wireless->history.samples();
    wh.sample_count = samples.size();

    const auto anchor_wall =
        wireless->last_sample_wall != std::chrono::system_clock::time_point{}
            ? wireless->last_sample_wall
            : wireless->last_read;
    const std::chrono::steady_clock::time_point newest_tick =
        samples.empty() ? std::chrono::steady_clock::time_point{}
                        : samples.back().timestamp;

    double signal_sum = 0.0;
    std::optional<double> signal_min, signal_max;
    std::size_t signal_count = 0;
    for (const WirelessHistorySample &sample : samples) {
      NetworkTrafficExportWirelessRow row;
      const double offset_seconds =
          std::chrono::duration<double>(newest_tick - sample.timestamp).count();
      row.timestamp =
          anchor_wall -
          std::chrono::duration_cast<std::chrono::system_clock::duration>(
              std::chrono::duration<double>(offset_seconds));
      row.association = wirelessAssociationName(sample.association);
      row.valid = sample.valid;
      row.signal_dbm = sample.signal_dbm;
      row.link_quality = sample.link_quality;
      if (sample.link_quality.has_value()) {
        row.link_quality_scale = sample.is_mac80211 ? "mac80211 /70"
                                                    : "driver-defined";
      }
      row.bitrate_bps = sample.bitrate_bps;
      row.frequency_mhz = sample.frequency_mhz;
      row.channel = sample.channel;
      wh.samples.push_back(std::move(row));

      if (sample.valid) {
        ++wh.valid_sample_count;
      }
      if (sample.signal_dbm.has_value()) {
        ++signal_count;
        const double value = *sample.signal_dbm;
        signal_sum += value;
        wh.current_signal_dbm = value;
        if (!signal_min.has_value() || value < *signal_min) {
          signal_min = value;
        }
        if (!signal_max.has_value() || value > *signal_max) {
          signal_max = value;
        }
      }
      if (sample.link_quality.has_value()) {
        wh.current_link_quality = *sample.link_quality;
      }
      if (sample.bitrate_bps.has_value()) {
        wh.current_bitrate_bps = *sample.bitrate_bps;
      }
      if (sample.frequency_mhz.has_value()) {
        wh.current_frequency_mhz = *sample.frequency_mhz;
      }
      if (sample.channel.has_value()) {
        wh.current_channel = *sample.channel;
      }
    }
    if (signal_count > 0) {
      wh.min_signal_dbm = signal_min;
      wh.max_signal_dbm = signal_max;
      wh.avg_signal_dbm = signal_sum / static_cast<double>(signal_count);
    }
    wh.span_seconds =
        samples.empty()
            ? 0.0
            : std::chrono::duration<double>(newest_tick -
                                            samples.front().timestamp)
                  .count();
    if (max_samples > 0) {
      wh.coverage =
          std::min(1.0, static_cast<double>(wh.sample_count) /
                            static_cast<double>(max_samples));
      wh.history_complete = wh.sample_count >= max_samples;
    }
    wh.last_update = anchor_wall;

    // Connection events feed the same ascending-order section as the samples:
    // each event carries its own wall-clock display time (never anchored off a
    // sample), the stable lowercase event name and the confidence/source flags.
    for (const WirelessConnectionEvent &event :
         wireless->connection_events.samples()) {
      NetworkTrafficExportWirelessEvent export_event;
      export_event.timestamp_iso8601 =
          formatNetworkTrafficTimestamp(event.wall_clock);
      export_event.interface_name = event.interface_name;
      export_event.event =
          wirelessConnectionEventTypeName(event.type);
      export_event.confident = event.confident;
      export_event.source = event.source;
      export_event.previous_association =
          wirelessAssociationName(event.previous_association);
      export_event.new_association =
          wirelessAssociationName(event.new_association);
      export_event.previous_signal_dbm = event.previous_signal_dbm;
      export_event.new_signal_dbm = event.new_signal_dbm;
      export_event.previous_frequency_mhz = event.previous_frequency_mhz;
      export_event.new_frequency_mhz = event.new_frequency_mhz;
      export_event.previous_channel = event.previous_channel;
      export_event.new_channel = event.new_channel;
      export_event.access_point_changed = event.ap_fingerprint_changed;
      export_event.access_point_reliable = event.ap_identity_reliable;
      wh.events.push_back(std::move(export_event));
    }
    snapshot.wireless_history = std::move(wh);
  }

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
      "rx_dropped_per_second,tx_dropped_per_second,"
      "link_speed_mbps,link_speed_unit,link_speed_state,"
      "link_duplex,link_duplex_state,link_last_update,"
      "interface_index,mac_address,interface_type,hardware_class,"
      "device_related,driver,device_id,link_carrier,link_operstate,"
      "wireless_presence,wireless_phy_name,wireless_phy_index,"
      "wireless_signal_dbm,wireless_noise_dbm,wireless_link_quality,"
      "wireless_association,wireless_enabled,wireless_field_state\n";

  std::string out;
  out.reserve(kHeader.size() + snapshot.samples.size() * 96u);
  out += kHeader;

  const std::string interface =
      escapeNetworkTrafficCsvField(snapshot.display_name);
  const std::string identity = escapeNetworkTrafficCsvField(snapshot.identity);

  // The link metadata is series-scoped (one value per export), so the same
  // six fields are repeated on every row. Without captured link metrics every
  // field stays empty — the leading historical columns are unchanged and the
  // trailing columns are additive only.
  std::array<std::string, 6> link_columns =
      std::array<std::string, 6>{};
  if (snapshot.link.has_value()) {
    const NetworkTrafficExportLink &link = *snapshot.link;
    link_columns = {
        link.speed_mbps.has_value()
            ? formatExportDouble(static_cast<double>(*link.speed_mbps))
            : std::string{},
        link.speed_unit,
        link.speed_state,
        link.duplex,
        link.duplex_state,
        link.last_update == std::chrono::system_clock::time_point{}
            ? std::string{}
            : formatNetworkTrafficTimestamp(link.last_update),
    };
  }

  // The hardware/driver metadata is series-scoped too: nine more append-only
  // trailing columns, empty until a hardware record is captured for the
  // interface. Availability-driven values are null-equivalent (empty CSV
  // fields) rather than zeros, matching the historical columns' conventions.
  std::array<std::string, 9> hardware_columns =
      std::array<std::string, 9>{};
  if (snapshot.hardware.has_value()) {
    const NetworkTrafficExportHardware &hw = *snapshot.hardware;
    hardware_columns = {
        hw.ifindex.has_value() ? std::to_string(*hw.ifindex) : std::string{},
        hw.mac_address,
        hw.interface_type,
        hw.hardware_class,
        hw.device_related ? "yes" : "no",
        hw.driver,
        hw.device_id,
        // Carrier/operational presence uses the canonical link-state names
        // ("yes"/"no carrier"); exported as discrete 1/0/empty markers.
        hw.carrier_state == "yes" ? "1"
            : hw.carrier_state == "no carrier" ? "0"
                                               : std::string{},
        hw.oper_state == "up" ? "1" : "0",
    };
  }

  // The wireless metadata is series-scoped too: nine more append-only trailing
  // columns, empty until a wireless record is captured for the interface.
  // Unavailable values are empty CSV fields (never zeros); SSID/BSSID/channel/
  // frequency/bitrate are not exposed by the kernel via sysfs and
  // /proc/net/wireless
  // so they are never fabricated columns.
  std::array<std::string, 9> wireless_columns =
      std::array<std::string, 9>{};
  if (snapshot.wireless.has_value()) {
    const NetworkTrafficExportWireless &wl = *snapshot.wireless;
    wireless_columns = {
        wl.presence,
        wl.phy_name,
        wl.phy_index.has_value() ? std::to_string(*wl.phy_index)
                                 : std::string{},
        wl.signal_dbm.has_value() ? std::to_string(*wl.signal_dbm)
                                  : std::string{},
        wl.noise_dbm.has_value() ? std::to_string(*wl.noise_dbm)
                                 : std::string{},
        wl.link_quality.has_value() ? std::to_string(*wl.link_quality)
                                    : std::string{},
        wl.association,
        wl.enabled ? "1" : "0",
        wl.field_state,
    };
  }

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
    for (const std::string &field : link_columns) {
      appendCsvField(out, field);
    }
    for (const std::string &field : hardware_columns) {
      appendCsvField(out, field);
    }
    for (const std::string &field : wireless_columns) {
      appendCsvField(out, field);
    }
    if (!out.empty() && out.back() == ',') {
      out.pop_back();  // drop the trailing separator (39 fields -> 38 commas)
    }
    out += '\n';
  }

  // Wireless history: a clearly-headed second section, appended only when the
  // interface is wireless and its history ring holds recorded samples. It uses
  // its own stable column order (wireless_timestamp first), a leading blank
  // separator line, and the same conventions as the main table: unavailable
  // metrics are empty fields (never zeros) and the association/quality-scale
  // columns are human-readable text. Includes the summary as a trailing line.
  if (snapshot.wireless_history.has_value() &&
      (!snapshot.wireless_history->samples.empty() ||
       !snapshot.wireless_history->events.empty())) {
    const NetworkTrafficExportWirelessHistory &wh = *snapshot.wireless_history;
    out += "\ntimestamp,interface,association,valid,signal_dbm,"
           "link_quality,link_quality_scale,bitrate_bps,frequency_mhz,"
           "channel\n";
    for (const NetworkTrafficExportWirelessRow &row : wh.samples) {
      out += escapeNetworkTrafficCsvField(
                 formatNetworkTrafficTimestamp(row.timestamp));
      out += ',';
      out += interface;
      out += ',';
      out += row.association;
      out += ',';
      out += row.valid ? "1" : "0";
      out += ',';
      out += row.signal_dbm.has_value()
                 ? formatExportDouble(*row.signal_dbm)
                 : std::string{};
      out += ',';
      out += row.link_quality.has_value() ? std::to_string(*row.link_quality)
                                          : std::string{};
      out += ',';
      out += escapeNetworkTrafficCsvField(row.link_quality_scale);
      out += ',';
      out += row.bitrate_bps.has_value()
                 ? formatExportDouble(*row.bitrate_bps)
                 : std::string{};
      out += ',';
      out += row.frequency_mhz.has_value()
                 ? formatExportDouble(*row.frequency_mhz)
                 : std::string{};
      out += ',';
      out += row.channel.has_value() ? std::to_string(*row.channel)
                                     : std::string{};
      out += '\n';
    }
    // A one-line summary, clearly prefixed so it can never be mistaken for a
    // sample row.
    out += "summary,signal_dbm_current=";
    out += jsonNumber(wh.current_signal_dbm);
    out += ",signal_dbm_min=";
    out += jsonNumber(wh.min_signal_dbm);
    out += ",signal_dbm_max=";
    out += jsonNumber(wh.max_signal_dbm);
    out += ",signal_dbm_avg=";
    out += jsonNumber(wh.avg_signal_dbm);
    out += ",valid_samples=";
    out += std::to_string(wh.valid_sample_count);
    out += ",coverage=";
    out += formatExportDouble(wh.coverage);
    out += ",span_seconds=";
    out += formatExportDouble(wh.span_seconds);
    out += '\n';

    // Connection events: a third clearly-headed section appended only when the
    // interface recorded any. Each row carries the event's own wall-clock
    // display time, its stable lowercase type name, the confidence/source flags
    // and — for association transitions — the raw before/after association,
    // signal, frequency and channel. Access-point identity is never exported:
    // only the recognition booleans (change + reliability) describe a roam.
    if (!wh.events.empty()) {
      out += "\ntimestamp,interface,event,confident,source,"
             "previous_association,new_association,previous_signal_dbm,"
             "new_signal_dbm,previous_frequency_mhz,new_frequency_mhz,"
             "previous_channel,new_channel,access_point_changed,"
             "access_point_reliable\n";
      for (const NetworkTrafficExportWirelessEvent &event : wh.events) {
        out += escapeNetworkTrafficCsvField(event.timestamp_iso8601);
        out += ',';
        out += escapeNetworkTrafficCsvField(event.interface_name);
        out += ',';
        out += escapeNetworkTrafficCsvField(event.event);
        out += ',';
        out += event.confident ? "1" : "0";
        out += ',';
        out += escapeNetworkTrafficCsvField(event.source);
        out += ',';
        out += escapeNetworkTrafficCsvField(event.previous_association);
        out += ',';
        out += escapeNetworkTrafficCsvField(event.new_association);
        out += ',';
        out += event.previous_signal_dbm.has_value()
                   ? formatExportDouble(*event.previous_signal_dbm)
                   : std::string{};
        out += ',';
        out += event.new_signal_dbm.has_value()
                   ? formatExportDouble(*event.new_signal_dbm)
                   : std::string{};
        out += ',';
        out += event.previous_frequency_mhz.has_value()
                   ? formatExportDouble(*event.previous_frequency_mhz)
                   : std::string{};
        out += ',';
        out += event.new_frequency_mhz.has_value()
                   ? formatExportDouble(*event.new_frequency_mhz)
                   : std::string{};
        out += ',';
        out += event.previous_channel.has_value()
                   ? std::to_string(*event.previous_channel)
                   : std::string{};
        out += ',';
        out += event.new_channel.has_value()
                   ? std::to_string(*event.new_channel)
                   : std::string{};
        out += ',';
        out += event.access_point_changed ? "1" : "0";
        out += ',';
        out += event.access_point_reliable ? "1" : "0";
        out += '\n';
      }
    }
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

  // Link speed/duplex metadata captured with the snapshot, or null when the
  // export has none (aggregate / non-physical selection / metrics not captured).
  const std::string link_json = [&] {
    if (!snapshot.link.has_value()) {
      return std::string("null");
    }
    const NetworkTrafficExportLink &link = *snapshot.link;
    return jsonObject({
        {"physical", link.physical ? "true" : "false"},
        {"link_active", link.link_active ? "true" : "false"},
        {"speed_state", jsonEscape(link.speed_state)},
        {"speed_mbps", jsonNumber(link.speed_mbps)},
        {"speed_unit", jsonEscape(link.speed_unit)},
        {"duplex", jsonEscape(link.duplex)},
        {"duplex_state", jsonEscape(link.duplex_state)},
        {"last_update",
         jsonEscape(formatNetworkTrafficTimestamp(link.last_update))},
    });
  }();

  const std::string hardware_json = [&] {
    if (!snapshot.hardware.has_value()) {
      return std::string("null");
    }
    const NetworkTrafficExportHardware &hw = *snapshot.hardware;
    return jsonObject({
        {"interface_type", jsonEscape(hw.interface_type)},
        {"hardware_class", jsonEscape(hw.hardware_class)},
        {"device_related", hw.device_related ? "true" : "false"},
        {"device_bus", jsonEscape(hw.device_bus)},
        {"device_id", jsonEscape(hw.device_id)},
        {"driver", jsonEscape(hw.driver)},
        {"driver_state", jsonEscape(hw.driver_state)},
        {"mac_address", jsonEscape(hw.mac_address)},
        {"ifindex", jsonNumber(hw.ifindex)},
        {"name_assign_type", jsonNumber(hw.name_assign_type)},
        {"carrier_state", jsonEscape(hw.carrier_state)},
        {"oper_state", jsonEscape(hw.oper_state)},
        {"availability", jsonEscape(hw.availability)},
        {"last_read", jsonEscape(formatNetworkTrafficTimestamp(hw.last_read))},
    });
  }();

  // Wireless metadata captured with the snapshot, or null when absent (aggregate
  // / non-wireless interface). SSID/BSSID/channel/frequency/bitrate require the
  // nl80211 netlink API and are intentionally absent from the model — the
  // kernel does not expose them through sysfs or /proc/net/wireless.
  const std::string wireless_json = [&] {
    if (!snapshot.wireless.has_value()) {
      return std::string("null");
    }
    const NetworkTrafficExportWireless &wl = *snapshot.wireless;
    return jsonObject({
        {"interface_name", jsonEscape(wl.interface_name)},
        {"presence", jsonEscape(wl.presence)},
        {"phy_name", jsonEscape(wl.phy_name)},
        {"phy_index", jsonNumber(wl.phy_index)},
        {"phy_state", jsonEscape(wl.phy_state)},
        {"is_mac80211", wl.is_mac80211 ? "true" : "false"},
        {"link_quality", jsonNumber(wl.link_quality)},
        {"signal_dbm", jsonNumber(wl.signal_dbm)},
        {"noise_dbm", jsonNumber(wl.noise_dbm)},
        {"association", jsonEscape(wl.association)},
        {"enabled", wl.enabled ? "true" : "false"},
        {"carrier_state", jsonEscape(wl.carrier_state)},
        {"field_state", jsonEscape(wl.field_state)},
        {"last_read", jsonEscape(formatNetworkTrafficTimestamp(wl.last_read))},
    });
  }();

  // Wireless history captured with the snapshot, or null when absent (aggregate
  // / non-wireless interface / no recorded samples). Per-sample rows plus the
  // summary the wireless-history view shows; unavailable metrics are null and
  // never fabricated zeros. SSID/BSSID are not exposed by the kernel via sysfs
  // or /proc/net/wireless and stay absent.
  const std::string wireless_history_json = [&] {
    if (!snapshot.wireless_history.has_value() ||
        (snapshot.wireless_history->samples.empty() &&
         snapshot.wireless_history->events.empty())) {
      return std::string("null");
    }
    const NetworkTrafficExportWirelessHistory &wh =
        *snapshot.wireless_history;
    std::string start_timestamp = "null";
    std::string end_timestamp = "null";
    if (!wh.samples.empty()) {
      start_timestamp =
          jsonEscape(formatNetworkTrafficTimestamp(wh.samples.front().timestamp));
      end_timestamp =
          jsonEscape(formatNetworkTrafficTimestamp(wh.samples.back().timestamp));
    }
    std::vector<std::string> event_objects;
    event_objects.reserve(wh.events.size());
    for (const NetworkTrafficExportWirelessEvent &event : wh.events) {
      event_objects.push_back(jsonObject({
          {"timestamp", jsonEscape(event.timestamp_iso8601)},
          {"interface_name", jsonEscape(event.interface_name)},
          {"event", jsonEscape(event.event)},
          {"confident", event.confident ? "true" : "false"},
          {"source", jsonEscape(event.source)},
          {"previous_association", jsonEscape(event.previous_association)},
          {"new_association", jsonEscape(event.new_association)},
          {"previous_signal_dbm", jsonNumber(event.previous_signal_dbm)},
          {"new_signal_dbm", jsonNumber(event.new_signal_dbm)},
          {"previous_frequency_mhz", jsonNumber(event.previous_frequency_mhz)},
          {"new_frequency_mhz", jsonNumber(event.new_frequency_mhz)},
          {"previous_channel", jsonNumber(event.previous_channel)},
          {"new_channel", jsonNumber(event.new_channel)},
          {"access_point_changed",
           event.access_point_changed ? "true" : "false"},
          {"access_point_reliable",
           event.access_point_reliable ? "true" : "false"},
      }));
    }
    std::vector<std::string> sample_objects;
    sample_objects.reserve(wh.samples.size());
    for (const NetworkTrafficExportWirelessRow &row : wh.samples) {
      sample_objects.push_back(jsonObject({
          {"timestamp",
           jsonEscape(formatNetworkTrafficTimestamp(row.timestamp))},
          {"association", jsonEscape(row.association)},
          {"valid", row.valid ? "true" : "false"},
          {"signal_dbm", jsonNumber(row.signal_dbm)},
          {"link_quality", jsonNumber(row.link_quality)},
          {"link_quality_scale", jsonEscape(row.link_quality_scale)},
          {"bitrate_bps", jsonNumber(row.bitrate_bps)},
          {"frequency_mhz", jsonNumber(row.frequency_mhz)},
          {"channel", jsonNumber(row.channel)},
      }));
    }
    return jsonObject({
        {"start_timestamp", start_timestamp},
        {"end_timestamp", end_timestamp},
        {"sample_count", std::to_string(wh.sample_count)},
        {"valid_sample_count", std::to_string(wh.valid_sample_count)},
        {"coverage", jsonNumber(std::optional<double>(wh.coverage))},
        {"complete", wh.history_complete ? "true" : "false"},
        {"span_seconds", jsonNumber(std::optional<double>(wh.span_seconds))},
        {"current_signal_dbm", jsonNumber(wh.current_signal_dbm)},
        {"min_signal_dbm", jsonNumber(wh.min_signal_dbm)},
        {"max_signal_dbm", jsonNumber(wh.max_signal_dbm)},
        {"avg_signal_dbm", jsonNumber(wh.avg_signal_dbm)},
        {"current_link_quality", jsonNumber(wh.current_link_quality)},
        {"current_bitrate_bps", jsonNumber(wh.current_bitrate_bps)},
        {"current_frequency_mhz", jsonNumber(wh.current_frequency_mhz)},
        {"current_channel", jsonNumber(wh.current_channel)},
        {"last_update",
         jsonEscape(formatNetworkTrafficTimestamp(wh.last_update))},
        {"samples", jsonArray(sample_objects)},
        {"connection_events_count", std::to_string(wh.events.size())},
        {"connection_events", jsonArray(event_objects)},
    });
  }();

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
      {"link", link_json},
      {"hardware", hardware_json},
      {"wireless", wireless_json},
      {"wireless_history", wireless_history_json},
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
  if (snapshot.samples.empty() &&
      (snapshot.wireless_history.has_value() == false ||
       (snapshot.wireless_history->samples.empty() &&
        snapshot.wireless_history->events.empty()))) {
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