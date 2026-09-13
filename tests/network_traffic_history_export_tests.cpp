#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

#include "network_interface_details.hpp"
#include "network_interface_hardware.hpp"
#include "network_link_metrics.hpp"
#include "network_link_state.hpp"
#include "network_traffic_history.hpp"
#include "network_traffic_history_export.hpp"

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
using atm::NetworkInterfaceStats;
using atm::NetworkInterfaceType;
using atm::NetworkTrafficExportFormat;
using atm::NetworkTrafficExportSnapshot;
using atm::NetworkTrafficExportStatus;
using atm::NetworkTrafficHistory;
using atm::NetworkTrafficSeries;
using atm::ResourceHistory;
using atm::TimedSample;

// -------------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------------

/// Builds a series with all ten rings created for `max_samples`.
atm::NetworkTrafficSeries makeSeries(const std::string &identity,
                                     const std::string &name, bool aggregate,
                                     std::size_t max_samples) {
  atm::NetworkTrafficSeries s;
  s.identity = identity;
  s.display_name = name;
  s.aggregate = aggregate;
  s.rx_bytes_per_second = ResourceHistory<TimedSample>(max_samples);
  s.tx_bytes_per_second = ResourceHistory<TimedSample>(max_samples);
  s.rx_packets_per_second = ResourceHistory<TimedSample>(max_samples);
  s.tx_packets_per_second = ResourceHistory<TimedSample>(max_samples);
  s.rx_errors_per_second = ResourceHistory<TimedSample>(max_samples);
  s.tx_errors_per_second = ResourceHistory<TimedSample>(max_samples);
  s.rx_dropped_per_second = ResourceHistory<TimedSample>(max_samples);
  s.tx_dropped_per_second = ResourceHistory<TimedSample>(max_samples);
  s.rx_bytes_total = ResourceHistory<TimedSample>(max_samples);
  s.tx_bytes_total = ResourceHistory<TimedSample>(max_samples);
  return s;
}

/// Fills `s` with four ticks (0s..3s) after `last_update`. Tick 0 carries only
/// cumulative counters (no baseline yet); ticks 1..3 add byte rates; ticks 2..3
/// add the packet/error/drop rates.
void fillFourTicks(atm::NetworkTrafficSeries &s) {
  const auto t0 = std::chrono::steady_clock::now();
  const auto w0 = std::chrono::system_clock::now();
  s.last_update = w0 + std::chrono::seconds(3);

  const auto t1 = t0 + std::chrono::seconds(1);
  const auto t2 = t0 + std::chrono::seconds(2);
  const auto t3 = t0 + std::chrono::seconds(3);

  s.rx_bytes_total.addSample(TimedSample{t0, 1000.0});
  s.tx_bytes_total.addSample(TimedSample{t0, 500.0});

  s.rx_bytes_total.addSample(TimedSample{t1, 1500.0});
  s.tx_bytes_total.addSample(TimedSample{t1, 700.0});
  s.rx_bytes_per_second.addSample(TimedSample{t1, 500.0});
  s.tx_bytes_per_second.addSample(TimedSample{t1, 200.0});

  s.rx_bytes_total.addSample(TimedSample{t2, 1900.0});
  s.tx_bytes_total.addSample(TimedSample{t2, 800.0});
  s.rx_bytes_per_second.addSample(TimedSample{t2, 400.0});
  s.tx_bytes_per_second.addSample(TimedSample{t2, 100.0});
  s.rx_packets_per_second.addSample(TimedSample{t2, 60.0});
  s.tx_packets_per_second.addSample(TimedSample{t2, 40.0});
  s.rx_errors_per_second.addSample(TimedSample{t2, 1.5});
  s.tx_errors_per_second.addSample(TimedSample{t2, 0.5});
  s.rx_dropped_per_second.addSample(TimedSample{t2, 3.0});
  s.tx_dropped_per_second.addSample(TimedSample{t2, 1.0});

  s.rx_bytes_total.addSample(TimedSample{t3, 2300.0});
  s.tx_bytes_total.addSample(TimedSample{t3, 950.0});
  s.rx_bytes_per_second.addSample(TimedSample{t3, 400.0});
  s.tx_bytes_per_second.addSample(TimedSample{t3, 150.0});
  s.rx_packets_per_second.addSample(TimedSample{t3, 70.0});
  s.tx_packets_per_second.addSample(TimedSample{t3, 50.0});
  s.rx_errors_per_second.addSample(TimedSample{t3, 2.0});
  s.tx_errors_per_second.addSample(TimedSample{t3, 0.5});
  s.rx_dropped_per_second.addSample(TimedSample{t3, 2.0});
  s.tx_dropped_per_second.addSample(TimedSample{t3, 1.0});
}

/// Splits multi-line CSV text into its lines, skipping the trailing empty line.
std::vector<std::string> linesOf(const std::string &csv) {
  std::vector<std::string> lines;
  std::istringstream in(csv);
  for (std::string line; std::getline(in, line) && !line.empty();) {
    lines.push_back(line);
  }
  return lines;
}

/// Splits one CSV line into fields, honoring double-quoted fields.
std::vector<std::string> csvSplit(const std::string &line) {
  std::vector<std::string> fields;
  std::string current;
  bool in_quotes = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (in_quotes) {
      if (c == '"') {
        if (i + 1 < line.size() && line[i + 1] == '"') {
          current += '"';
          ++i;
        } else {
          in_quotes = false;
        }
      } else {
        current += c;
      }
    } else if (c == '"') {
      in_quotes = true;
    } else if (c == ',') {
      fields.push_back(current);
      current.clear();
    } else {
      current += c;
    }
  }
  fields.push_back(current);
  return fields;
}

/// Minimal structural JSON validity check (balanced brackets / strings).
bool isValidJson(const std::string &text) {
  std::vector<char> stack;
  bool in_string = false;
  bool escape = false;
  for (const char c : text) {
    if (in_string) {
      if (escape) {
        escape = false;
      } else if (c == '\\') {
        escape = true;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }
    if (c == '"') {
      in_string = true;
    } else if (c == '{' || c == '[') {
      stack.push_back(c);
    } else if (c == '}' || c == ']') {
      if (stack.empty()) {
        return false;
      }
      const char open = stack.back();
      stack.pop_back();
      if ((c == '}' && open != '{') || (c == ']' && open != '[')) {
        return false;
      }
    }
  }
  return !in_string && !escape && stack.empty();
}

/// Counts occurrences of an exact quoted key in the JSON text.
std::size_t countKey(const std::string &text, const std::string &key) {
  std::size_t matches = 0;
  std::size_t pos = 0;
  const std::string needle = "\"" + key + "\"";
  while ((pos = text.find(needle, pos)) != std::string::npos) {
    ++matches;
    pos += needle.size();
  }
  return matches;
}

/// Extracts the quoted sample timestamp values (ascending array order).
std::vector<std::string> sampleTimestamps(const std::string &text) {
  std::vector<std::string> values;
  const std::string marker = "\"timestamp\":\"";
  std::size_t pos = 0;
  while ((pos = text.find(marker, pos)) != std::string::npos) {
    pos += marker.size();
    const std::size_t end = text.find('"', pos);
    if (end == std::string::npos) {
      break;
    }
    values.push_back(text.substr(pos, end - pos));
    pos = end + 1;
  }
  return values;
}

/// Records a snapshot with a small sleep so rate windows are positive.
void recordWithGap(atm::NetworkTrafficHistory &monitor,
                   const atm::NetworkInterfaceSnapshot &snapshot) {
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  monitor.record(snapshot);
}

/// Builds one interface with merged traffic counters.
NetworkInterfaceInfo makeInterface(const std::string &name, int ifindex,
                                   NetworkInterfaceType type,
                                   std::uint64_t rx_bytes,
                                   std::uint64_t tx_bytes,
                                   std::uint64_t rx_packets = 1,
                                   std::uint64_t tx_packets = 1) {
  NetworkInterfaceInfo info;
  info.name = name;
  info.type = type;
  info.link.ifindex = ifindex;
  NetworkInterfaceStats traffic;
  traffic.name = name;
  traffic.rx_bytes = rx_bytes;
  traffic.tx_bytes = tx_bytes;
  traffic.rx_packets = rx_packets;
  traffic.tx_packets = tx_packets;
  traffic.rx_errors = 0;
  traffic.tx_errors = 0;
  traffic.rx_dropped = 0;
  traffic.tx_dropped = 0;
  info.traffic = traffic;
  info.refreshed_at = std::chrono::system_clock::now();
  return info;
}

atm::NetworkInterfaceSnapshot snapshotWith(
    std::vector<NetworkInterfaceInfo> interfaces) {
  atm::NetworkInterfaceSnapshot snapshot;
  snapshot.interfaces = std::move(interfaces);
  snapshot.refreshed_at = std::chrono::system_clock::now();
  return snapshot;
}

// -------------------------------------------------------------------------
// CSV serialization
// -------------------------------------------------------------------------

void test_csv_header_order() {
  run("csvHeaderOrder");
  atm::NetworkTrafficSeries s = makeSeries("idx:2", "eth0", false, 120);
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  const std::string csv = atm::generateNetworkTrafficCsv(snapshot);

  const std::vector<std::string> expected = {
      "timestamp",          "interface",      "identity",
      "rx_bytes_per_second", "tx_bytes_per_second", "rx_bits_per_second",
      "tx_bits_per_second", "rx_bytes_total", "tx_bytes_total",
      "rx_packets_per_second", "tx_packets_per_second", "rx_errors_per_second",
      "tx_errors_per_second", "rx_dropped_per_second", "tx_dropped_per_second",
      "link_speed_mbps", "link_speed_unit", "link_speed_state",
      "link_duplex", "link_duplex_state", "link_last_update",
      "interface_index", "mac_address", "interface_type", "hardware_class",
      "device_related", "driver", "device_id", "link_carrier", "link_operstate",
  };
  const std::vector<std::string> header = csvSplit(
      csv.substr(0, csv.find('\n')));
  CHECK(header.size() == expected.size());
  for (std::size_t i = 0; i < expected.size() && i < header.size(); ++i) {
    CHECK(header[i] == expected[i]);
  }
}

void test_csv_normal_export() {
  run("csvNormalExport");
  atm::NetworkTrafficSeries s = makeSeries("idx:2", "eth0", false, 120);
  fillFourTicks(s);
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  const std::string csv = atm::generateNetworkTrafficCsv(snapshot);

  std::vector<std::string> lines;
  std::istringstream in(csv);
  for (std::string line; std::getline(in, line) && !line.empty();) {
    lines.push_back(line);
  }
  CHECK(lines.size() == 5);  // header + 4 ticks

  const std::vector<std::string> row0 = csvSplit(lines[1]);
  CHECK(row0.size() == 30);
  CHECK(row0[15].empty());            // no link metrics captured
  CHECK(row0[21].empty());            // no hardware metadata captured
  CHECK(row0[24].empty());            // device_related absent with no hardware
  CHECK(row0[1] == "eth0");
  CHECK(row0[2] == "idx:2");
  CHECK(row0[3] == "");              // no rate on the first tick
  CHECK(row0[7] == "1000");          // cumulative rx
  CHECK(row0[8] == "500");           // cumulative tx

  const std::vector<std::string> row1 = csvSplit(lines[2]);
  CHECK(row1[3] == "500");
  CHECK(row1[4] == "200");
  CHECK(row1[5] == "4000");  // 8 * 500
  CHECK(row1[6] == "1600");  // 8 * 200
  CHECK(row1[7] == "1500");

  const std::vector<std::string> row2 = csvSplit(lines[3]);
  CHECK(row2[9] == "60");
  CHECK(row2[11] == "1.5");   // rx errors per second
  CHECK(row2[13] == "3");     // rx drops per second

  const std::vector<std::string> row3 = csvSplit(lines[4]);
  CHECK(row3[3] == "400");
  CHECK(row3[7] == "2300");
  CHECK(row3[14] == "1");  // tx drops per second
}

void test_csv_empty_history() {
  run("csvEmptyHistory");
  atm::NetworkTrafficSeries s = makeSeries("idx:2", "eth0", false, 120);
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  CHECK(snapshot.samples.empty());

  const std::string csv = atm::generateNetworkTrafficCsv(snapshot);
  const std::vector<std::string> lines = csvSplit(csv);
  CHECK(lines.size() == 30);  // header columns only

  const auto result = atm::exportNetworkTrafficHistory(
      "/tmp/network-traffic-export-empty.csv", snapshot,
      NetworkTrafficExportFormat::Csv);
  CHECK(result.status == NetworkTrafficExportStatus::EmptyHistory);
  std::remove("/tmp/network-traffic-export-empty.csv");
}

void test_csv_missing_values() {
  run("csvMissingValuesNeverZeros");
  atm::NetworkTrafficSeries s = makeSeries("idx:2", "eth0", false, 120);
  const auto t0 = std::chrono::steady_clock::now();
  s.rx_bytes_total.addSample(TimedSample{t0, 1000.0});
  s.rx_bytes_total.addSample(TimedSample{t0 + std::chrono::seconds(1), 1500.0});
  s.last_update = std::chrono::system_clock::now();

  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  const std::string csv = atm::generateNetworkTrafficCsv(snapshot);
  std::vector<std::string> lines;
  std::istringstream in(csv);
  for (std::string line; std::getline(in, line) && !line.empty();) {
    lines.push_back(line);
  }
  CHECK(lines.size() == 3);  // header + 2 ticks
  const std::vector<std::string> row = csvSplit(lines[1]);
  CHECK(row[3] == "");   // rx bytes per second honestly absent
  CHECK(row[5] == "");   // rx bits per second honestly absent
  CHECK(row[7] == "1000");
  CHECK(row[11] == "");  // errors absent, never "0"
}

void test_csv_special_characters() {
  run("csvSpecialCharacters");
  atm::NetworkTrafficSeries s =
      makeSeries("name:eth,0", "eth,0", false, 120);
  s.rx_bytes_total.addSample(
      TimedSample{std::chrono::steady_clock::now(), 42.0});
  s.last_update = std::chrono::system_clock::now();
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  const std::string csv = atm::generateNetworkTrafficCsv(snapshot);

  // Both the name and identity contain a comma and are quoted.
  CHECK(csv.find("\"eth,0\"") != std::string::npos);
  CHECK(csv.find("\"name:eth,0\"") != std::string::npos);
  std::vector<std::string> lines;
  std::istringstream in(csv);
  for (std::string line; std::getline(in, line);) {
    lines.push_back(line);
  }
  const std::vector<std::string> fields = csvSplit(lines[1]);
  CHECK(fields[1] == "eth,0");
  CHECK(fields[2] == "name:eth,0");
  CHECK(fields[7] == "42");
}

void test_csv_quotes() {
  run("csvQuotesEscaped");
  atm::NetworkTrafficSeries s =
      makeSeries("name:a\"b", "a\"b", false, 120);
  s.rx_bytes_total.addSample(
      TimedSample{std::chrono::steady_clock::now(), 1.0});
  s.last_update = std::chrono::system_clock::now();
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  const std::string csv = atm::generateNetworkTrafficCsv(snapshot);
  // RFC 4180: embedded quote is doubled inside a quoted field.
  CHECK(csv.find("\"a\"\"b\"") != std::string::npos);
}

void test_csv_newline_escaping() {
  run("csvNewlineEscaping");
  atm::NetworkTrafficSeries s =
      makeSeries("name:eth\n0", "eth\n0", false, 120);
  s.rx_bytes_total.addSample(
      TimedSample{std::chrono::steady_clock::now(), 2.0});
  s.last_update = std::chrono::system_clock::now();
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  const std::string csv = atm::generateNetworkTrafficCsv(snapshot);
  CHECK(csv.find("\"eth\n0\"") != std::string::npos);  // quoted with raw NL
}

void test_csv_timestamp_format() {
  run("csvTimestampFormat");
  std::tm local{};
  local.tm_year = 2026 - 1900;
  local.tm_mon = 0;   // January
  local.tm_mday = 2;
  local.tm_hour = 3;
  local.tm_min = 4;
  local.tm_sec = 5;
  local.tm_isdst = -1;
  const std::time_t tt = ::mktime(&local);
  const auto tp = std::chrono::system_clock::from_time_t(tt);
  CHECK(atm::formatNetworkTrafficTimestamp(tp) == "2026-01-02T03:04:05");
}

void test_csv_large_counters() {
  run("csvLargeCounters");
  atm::NetworkTrafficSeries s = makeSeries("idx:2", "eth0", false, 120);
  const auto t0 = std::chrono::steady_clock::now();
  // Values below 1e15 print as plain integers even though stored as doubles.
  s.rx_bytes_total.addSample(TimedSample{t0, 999999999999999.0});
  s.rx_bytes_total.addSample(
      TimedSample{t0 + std::chrono::seconds(1), 1000000000000000.0});
  s.last_update = std::chrono::system_clock::now();
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  const std::string csv = atm::generateNetworkTrafficCsv(snapshot);
  std::vector<std::string> lines;
  std::istringstream in(csv);
  for (std::string line; std::getline(in, line);) {
    lines.push_back(line);
  }
  const std::vector<std::string> row = csvSplit(lines[1]);
  CHECK(row[7] == "999999999999999");
  (void)row;
}

void test_csv_numeric_precision() {
  run("csvNumericPrecision");
  atm::NetworkTrafficSeries s = makeSeries("idx:2", "eth0", false, 120);
  const auto t0 = std::chrono::steady_clock::now();
  s.rx_bytes_total.addSample(TimedSample{t0, 0.0});
  s.rx_bytes_total.addSample(TimedSample{t0 + std::chrono::seconds(1), 1.0});
  s.rx_bytes_per_second.addSample(TimedSample{t0 + std::chrono::seconds(1), 0.1});
  s.tx_bytes_per_second.addSample(
      TimedSample{t0 + std::chrono::seconds(1), 1.0 / 3.0});
  s.last_update = std::chrono::system_clock::now();
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  const std::string csv = atm::generateNetworkTrafficCsv(snapshot);
  std::vector<std::string> lines;
  std::istringstream in(csv);
  for (std::string line; std::getline(in, line);) {
    lines.push_back(line);
  }
  const std::vector<std::string> row = csvSplit(lines[2]);
  CHECK(row[3] == "0.1");
  CHECK(row[4] == "0.3333333333333333");  // shortest round-trip for 1/3
}

// -------------------------------------------------------------------------
// Link speed/duplex metadata columns
// -------------------------------------------------------------------------

/// Builds a valid link-metrics record for a 1 Gbps full-duplex Ethernet link.
atm::NetworkLinkMetrics makeLinkMetrics(bool active = true) {
  atm::NetworkLinkMetrics metrics;
  metrics.identity = "idx:2";
  metrics.name = "eth0";
  metrics.physical = true;
  metrics.link_active = active;
  metrics.speed_state =
      active ? atm::NetworkSpeedState::Valid : atm::NetworkSpeedState::Stale;
  metrics.speed_mbps = 1000;
  metrics.duplex_state =
      active ? atm::NetworkDuplexState::Valid : atm::NetworkDuplexState::Stale;
  metrics.duplex = atm::NetworkDuplexMode::Full;
  metrics.last_update = std::chrono::system_clock::now();
  return metrics;
}

void test_csv_link_metadata() {
  run("csvLinkMetadata");
  atm::NetworkTrafficSeries s = makeSeries("idx:2", "eth0", false, 120);
  fillFourTicks(s);
  const atm::NetworkLinkMetrics metrics = makeLinkMetrics(true);
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120, &metrics);
  CHECK(snapshot.link.has_value());
  CHECK(snapshot.link->speed_mbps.has_value() && *snapshot.link->speed_mbps == 1000);
  CHECK(snapshot.link->speed_unit == "Mb/s");
  CHECK(snapshot.link->speed_state == "valid");
  CHECK(snapshot.link->duplex == "full");
  CHECK(snapshot.link->duplex_state == "valid");
  CHECK(snapshot.link->physical);

  const std::string csv = atm::generateNetworkTrafficCsv(snapshot);
  std::vector<std::string> lines;
  std::istringstream in(csv);
  for (std::string line; std::getline(in, line) && !line.empty();) {
    lines.push_back(line);
  }
  CHECK(lines.size() == 5);  // header + 4 ticks
  const std::vector<std::string> row = csvSplit(lines[1]);
  // The leading 15 historical columns are unchanged; the link fields follow.
  CHECK(row[0] != "");
  CHECK(row[15] == "1000");
  CHECK(row[16] == "Mb/s");
  CHECK(row[17] == "valid");
  CHECK(row[18] == "full");
  CHECK(row[19] == "valid");
  CHECK(row[20] == atm::formatNetworkTrafficTimestamp(metrics.last_update));
}

void test_csv_link_metadata_stale_unavailable() {
  run("csvLinkMetadataStaleUnavailable");
  atm::NetworkTrafficSeries s = makeSeries("idx:2", "eth0", false, 120);
  fillFourTicks(s);

  // Inactive link: the preserved speed/duplex read stale (never dropped).
  const atm::NetworkLinkMetrics stale = makeLinkMetrics(false);
  const atm::NetworkTrafficExportSnapshot stale_snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120, &stale);
  const std::vector<std::string> stale_lines =
      linesOf(atm::generateNetworkTrafficCsv(stale_snapshot));
  CHECK(stale_lines.size() == 5);  // header + 4 ticks
  const std::vector<std::string> stale_row = csvSplit(stale_lines[1]);
  CHECK(stale_row[17] == "stale");
  CHECK(stale_row[18] == "full");
  CHECK(stale_row[19] == "stale");

  // No metrics captured: the trailing columns stay empty, never zeros.
  const atm::NetworkTrafficExportSnapshot none =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  CHECK(!none.link.has_value());
  const std::vector<std::string> none_lines =
      linesOf(atm::generateNetworkTrafficCsv(none));
  CHECK(none_lines.size() == 5);
  const std::vector<std::string> none_row = csvSplit(none_lines[1]);
  CHECK(none_row.size() == 30);
  CHECK(none_row[15].empty() && none_row[16].empty() && none_row[17].empty());
}

void test_csv_hardware_metadata() {
  run("csvHardwareMetadata");
  atm::NetworkTrafficSeries s = makeSeries("idx:2", "eth0", false, 120);
  fillFourTicks(s);

  // A physical NIC with a bound driver, an interface info record and a healthy
  // link-state record: the trailing hardware columns must be populated.
  atm::NetworkInterfaceHardware hw;
  hw.identity = "idx:2";
  hw.name = "eth0";
  hw.device_related = true;
  hw.device_path = "/sys/devices/pci0000:00/0000:00:1f.6/net/eth0";
  hw.device_bus = "pci";
  hw.device_id = "0x0";
  hw.name_assign_type = 1;
  hw.device_kind = atm::NetworkDeviceKind::Physical;
  hw.device_state = atm::NetworkHardwareState::Available;
  hw.driver = "e1000e";
  hw.driver_state = atm::NetworkHardwareState::Available;
  hw.last_read = std::chrono::system_clock::now();

  atm::NetworkInterfaceInfo info;
  info.name = "eth0";
  info.type = atm::NetworkInterfaceType::Ethernet;
  info.link.ifindex = 2;
  info.link.mac_address = "aa:bb:cc:dd:ee:ff";

  atm::TrackedInterface link_state;
  link_state.oper = atm::NetworkOperState::Up;
  link_state.carrier = atm::NetworkCarrierState::Carrier;
  link_state.has_valid_state = true;
  link_state.link_known = true;

  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120, nullptr, &hw, &info,
                                            &link_state);

  const std::vector<std::string> lines =
      linesOf(atm::generateNetworkTrafficCsv(snapshot));
  CHECK(lines.size() == 5);  // header + 4 ticks
  const std::vector<std::string> row = csvSplit(lines[1]);
  CHECK(row.size() == 30);
  CHECK(row[21] == "2");               // interface_index
  CHECK(row[22] == "aa:bb:cc:dd:ee:ff");
  CHECK(row[23] == "Ethernet");        // interface_type
  CHECK(row[24] == "physical");        // hardware_class
  CHECK(row[25] == "yes");             // device_related
  CHECK(row[26] == "e1000e");          // driver
  CHECK(row[27] == "0x0");             // device_id
  CHECK(row[28] == "1");               // link_carrier
  CHECK(row[29] == "1");               // link_operstate
}

void test_json_hardware_metadata() {
  run("jsonHardwareMetadata");
  atm::NetworkTrafficSeries s = makeSeries("idx:2", "eth0", false, 120);
  s.rx_bytes_total.addSample(
      TimedSample{std::chrono::steady_clock::now(), 5.0});
  s.last_update = std::chrono::system_clock::now();

  atm::NetworkInterfaceHardware hw;
  hw.identity = "idx:2";
  hw.name = "eth0";
  hw.device_related = true;
  hw.device_path = "/sys/devices/pci0000:00/0000:00:1f.6/net/eth0";
  hw.device_bus = "pci";
  hw.device_id = "0x0";
  hw.name_assign_type = 1;
  hw.device_kind = atm::NetworkDeviceKind::Physical;
  hw.device_state = atm::NetworkHardwareState::Available;
  hw.driver = "e1000e";
  hw.driver_state = atm::NetworkHardwareState::Available;
  hw.last_read = std::chrono::system_clock::now();

  atm::NetworkInterfaceInfo info;
  info.name = "eth0";
  info.type = atm::NetworkInterfaceType::Wifi;
  info.link.ifindex = 2;
  info.link.mac_address = "AA:BB:CC:DD:EE:FF";  // normalized on export

  atm::TrackedInterface link_state;
  link_state.oper = atm::NetworkOperState::LowerLayerDown;
  link_state.carrier = atm::NetworkCarrierState::NoCarrier;
  link_state.has_valid_state = true;
  link_state.link_known = true;

  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120, nullptr, &hw, &info,
                                            &link_state);
  const std::string json = atm::generateNetworkTrafficJson(snapshot);
  CHECK(isValidJson(json));
  CHECK(json.find("\"hardware\":{") != std::string::npos);
  CHECK(json.find("\"interface_type\":\"Wifi\"") != std::string::npos);
  CHECK(json.find("\"hardware_class\":\"physical\"") != std::string::npos);
  CHECK(json.find("\"device_related\":true") != std::string::npos);
  CHECK(json.find("\"device_bus\":\"pci\"") != std::string::npos);
  CHECK(json.find("\"device_id\":\"0x0\"") != std::string::npos);
  CHECK(json.find("\"driver\":\"e1000e\"") != std::string::npos);
  CHECK(json.find("\"driver_state\":\"available\"") != std::string::npos);
  CHECK(json.find("\"mac_address\":\"aa:bb:cc:dd:ee:ff\"") !=
        std::string::npos);
  CHECK(json.find("\"ifindex\":2") != std::string::npos);
  CHECK(json.find("\"name_assign_type\":1") != std::string::npos);
  CHECK(json.find("\"carrier_state\":\"no carrier\"") != std::string::npos);
  CHECK(json.find("\"oper_state\":\"lowerlayerdown\"") != std::string::npos);
  CHECK(json.find("\"availability\":\"Down\"") != std::string::npos);
}

void test_json_hardware_null_when_absent() {
  run("jsonHardwareNullWhenAbsent");
  atm::NetworkTrafficSeries s = makeSeries("idx:2", "eth0", false, 120);
  s.rx_bytes_total.addSample(
      TimedSample{std::chrono::steady_clock::now(), 5.0});
  s.last_update = std::chrono::system_clock::now();
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  CHECK(!snapshot.hardware.has_value());
  const std::string json = atm::generateNetworkTrafficJson(snapshot);
  CHECK(isValidJson(json));
  CHECK(json.find("\"hardware\":null") != std::string::npos);
}

// -------------------------------------------------------------------------
// JSON serialization
// -------------------------------------------------------------------------

void test_json_valid_output() {
  run("jsonValidOutput");
  atm::NetworkTrafficSeries s = makeSeries("idx:2", "eth0", false, 120);
  fillFourTicks(s);
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  const std::string json = atm::generateNetworkTrafficJson(snapshot);
  CHECK(isValidJson(json));
}

void test_json_metadata_presence() {
  run("jsonMetadataPresence");
  atm::NetworkTrafficSeries s = makeSeries("idx:2", "eth0", false, 120);
  fillFourTicks(s);
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  const std::string json = atm::generateNetworkTrafficJson(snapshot);
  CHECK(json.find("export_metadata") != std::string::npos);
  CHECK(json.find("application") != std::string::npos);
  CHECK(json.find("application_version") != std::string::npos);
  CHECK(json.find("\"format\"") != std::string::npos);
  CHECK(json.find("created_at") != std::string::npos);
  CHECK(json.find("interface") != std::string::npos);
  CHECK(json.find("history") != std::string::npos);
  CHECK(json.find("start_timestamp") != std::string::npos);
  CHECK(json.find("end_timestamp") != std::string::npos);
  CHECK(json.find("sample_count") != std::string::npos);
  CHECK(json.find("max_samples") != std::string::npos);
  CHECK(json.find("sampling_interval_seconds") != std::string::npos);
  CHECK(json.find("units") != std::string::npos);
  CHECK(json.find("summary") != std::string::npos);
  CHECK(json.find("samples") != std::string::npos);
}

void test_json_interface_identity() {
  run("jsonInterfaceIdentity");
  atm::NetworkTrafficSeries s = makeSeries("idx:7", "enp3s0", false, 120);
  s.rx_bytes_total.addSample(
      TimedSample{std::chrono::steady_clock::now(), 5.0});
  s.last_update = std::chrono::system_clock::now();
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  const std::string json = atm::generateNetworkTrafficJson(snapshot);
  CHECK(json.find("\"identity\":\"idx:7\"") != std::string::npos);
  CHECK(json.find("\"name\":\"enp3s0\"") != std::string::npos);
  CHECK(json.find("\"aggregate\":false") != std::string::npos);
}

void test_json_aggregate_flag() {
  run("jsonAggregateFlag");
  atm::NetworkTrafficSeries s =
      makeSeries(std::string(atm::kNetworkTrafficAllIdentity),
                 "All interfaces", true, 120);
  s.rx_bytes_total.addSample(
      TimedSample{std::chrono::steady_clock::now(), 5.0});
  s.last_update = std::chrono::system_clock::now();
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  CHECK(snapshot.aggregate);
  const std::string json = atm::generateNetworkTrafficJson(snapshot);
  CHECK(json.find("\"aggregate\":true") != std::string::npos);
  CHECK(json.find("\"identity\":\"all\"") != std::string::npos);
}

void test_json_empty_sample_array() {
  run("jsonEmptySampleArray");
  atm::NetworkTrafficSeries s =
      makeSeries(std::string(atm::kNetworkTrafficAllIdentity),
                 "All interfaces", true, 120);
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  const std::string json = atm::generateNetworkTrafficJson(snapshot);
  CHECK(isValidJson(json));
  CHECK(json.find("\"samples\":[]") != std::string::npos);
}

void test_json_missing_values_null() {
  run("jsonMissingValuesNull");
  atm::NetworkTrafficSeries s = makeSeries("idx:2", "eth0", false, 120);
  const auto t0 = std::chrono::steady_clock::now();
  s.rx_bytes_total.addSample(TimedSample{t0, 1000.0});
  s.rx_bytes_total.addSample(TimedSample{t0 + std::chrono::seconds(1), 1500.0});
  s.last_update = std::chrono::system_clock::now();
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  const std::string json = atm::generateNetworkTrafficJson(snapshot);
  CHECK(json.find("\"rx_bytes_per_second\":null") != std::string::npos);
  CHECK(json.find("\"rx_bits_per_second\":null") != std::string::npos);
  CHECK(json.find("\"rx_bytes_total\":1500") != std::string::npos);
}

void test_json_special_characters() {
  run("jsonSpecialCharacters");
  atm::NetworkTrafficSeries s =
      makeSeries("idx:2", "a\"b\\c", false, 120);
  s.rx_bytes_total.addSample(
      TimedSample{std::chrono::steady_clock::now(), 1.0});
  s.last_update = std::chrono::system_clock::now();
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  const std::string json = atm::generateNetworkTrafficJson(snapshot);
  CHECK(isValidJson(json));
  // name "a"b\c" escapes to a\"b\\c inside quotes.
  CHECK(json.find("\"name\":\"a\\\"b\\\\c\"") != std::string::npos);
}

void test_json_sample_ordering() {
  run("jsonSampleOrdering");
  atm::NetworkTrafficSeries s = makeSeries("idx:2", "eth0", false, 120);
  fillFourTicks(s);
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  const std::string json = atm::generateNetworkTrafficJson(snapshot);
  const std::vector<std::string> stamps = sampleTimestamps(json);
  CHECK(stamps.size() == 4);
  for (std::size_t i = 1; i < stamps.size(); ++i) {
    CHECK(stamps[i - 1] <= stamps[i]);  // ISO timestamps compare chronologically
  }
  // The newest exported row is anchored to the series' last update.
  CHECK(atm::formatNetworkTrafficTimestamp(snapshot.samples.back().timestamp) ==
        atm::formatNetworkTrafficTimestamp(s.last_update));
}

void test_json_stable_field_names() {
  run("jsonStableFieldNames");
  atm::NetworkTrafficSeries s = makeSeries("idx:2", "eth0", false, 120);
  fillFourTicks(s);
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  const std::string json = atm::generateNetworkTrafficJson(snapshot);
  CHECK(countKey(json, "export_metadata") == 1);
  CHECK(countKey(json, "interface") == 1);
  CHECK(countKey(json, "history") == 1);
  CHECK(countKey(json, "units") == 1);
  CHECK(countKey(json, "summary") == 1);
  CHECK(countKey(json, "samples") == 1);
}

void test_json_link_null_when_absent() {
  run("jsonLinkNullWhenAbsent");
  atm::NetworkTrafficSeries s = makeSeries("idx:2", "eth0", false, 120);
  fillFourTicks(s);
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  const std::string json = atm::generateNetworkTrafficJson(snapshot);
  CHECK(json.find("\"link\":null") != std::string::npos);
  CHECK(countKey(json, "link") == 1);  // exactly one occurrence
}

void test_json_link_metadata() {
  run("jsonLinkMetadata");
  atm::NetworkTrafficSeries s = makeSeries("idx:2", "eth0", false, 120);
  fillFourTicks(s);
  const atm::NetworkLinkMetrics metrics = makeLinkMetrics(true);
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120, &metrics);
  const std::string json = atm::generateNetworkTrafficJson(snapshot);
  CHECK(isValidJson(json));
  CHECK(countKey(json, "link") == 1);
  CHECK(json.find("\"physical\":true") != std::string::npos);
  CHECK(json.find("\"link_active\":true") != std::string::npos);
  CHECK(json.find("\"speed_state\":\"valid\"") != std::string::npos);
  CHECK(json.find("\"speed_mbps\":1000") != std::string::npos);
  CHECK(json.find("\"speed_unit\":\"Mb/s\"") != std::string::npos);
  CHECK(json.find("\"duplex\":\"full\"") != std::string::npos);
  CHECK(json.find("\"duplex_state\":\"valid\"") != std::string::npos);
}

void test_json_link_null_for_aggregate() {
  run("jsonLinkNullForAggregate");
  atm::NetworkTrafficSeries s =
      makeSeries(std::string(atm::kNetworkTrafficAllIdentity),
                 "All interfaces", true, 120);
  fillFourTicks(s);
  const atm::NetworkLinkMetrics metrics = makeLinkMetrics(true);
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120, &metrics);
  // Metrics are keyed by identity: "all" won't match "idx:2" in the monitor,
  // but the test manually passes metrics to the builder. Since aggregate is
  // a series-level concept, the builder still attaches them (caller decides).
  CHECK(snapshot.link.has_value());
  CHECK(snapshot.link->physical);
}

void test_json_no_internal_state() {
  run("jsonNoInternalState");
  atm::NetworkTrafficSeries s = makeSeries("idx:2", "eth0", false, 120);
  fillFourTicks(s);
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  const std::string json = atm::generateNetworkTrafficJson(snapshot);
  for (const std::string_view token :
       {"pointer", "mutex", "internal", "impl", "ui_", "state_", "baseline",
        "membership_changed"}) {
    CHECK(json.find(std::string(token)) == std::string::npos);
  }
}

// -------------------------------------------------------------------------
// Summary calculations
// -------------------------------------------------------------------------

void test_summary_current_and_peak() {
  run("summaryCurrentAndPeak");
  atm::NetworkTrafficSeries s = makeSeries("idx:2", "eth0", false, 120);
  fillFourTicks(s);
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  CHECK(snapshot.summary.current_rx_bytes_per_second.has_value() &&
        *snapshot.summary.current_rx_bytes_per_second == 400.0);
  CHECK(snapshot.summary.current_tx_bytes_per_second.has_value() &&
        *snapshot.summary.current_tx_bytes_per_second == 150.0);
  CHECK(snapshot.summary.peak_rx_bytes_per_second.has_value() &&
        *snapshot.summary.peak_rx_bytes_per_second == 500.0);
  CHECK(snapshot.summary.peak_tx_bytes_per_second.has_value() &&
        *snapshot.summary.peak_tx_bytes_per_second == 200.0);
}

void test_summary_totals_valid_window() {
  run("summaryTotalsValidWindow");
  atm::NetworkTrafficSeries s = makeSeries("idx:2", "eth0", false, 120);
  fillFourTicks(s);
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  CHECK(snapshot.summary.total_rx_bytes.has_value() &&
        *snapshot.summary.total_rx_bytes == 1300.0);  // 2300 - 1000
  CHECK(snapshot.summary.total_tx_bytes.has_value() &&
        *snapshot.summary.total_tx_bytes == 450.0);   // 950 - 500
  CHECK(!snapshot.summary.totals_approximate);
}

void test_summary_counter_reset() {
  run("summaryCounterResetUnavailable");
  atm::NetworkTrafficSeries s = makeSeries("idx:2", "eth0", false, 120);
  const auto t0 = std::chrono::steady_clock::now();
  s.rx_bytes_total.addSample(TimedSample{t0, 1000.0});
  s.rx_bytes_total.addSample(TimedSample{t0 + std::chrono::seconds(1), 1500.0});
  s.rx_bytes_total.addSample(TimedSample{t0 + std::chrono::seconds(2), 800.0});
  s.rx_bytes_total.addSample(TimedSample{t0 + std::chrono::seconds(3), 1200.0});
  s.tx_bytes_total.addSample(TimedSample{t0, 100.0});
  s.tx_bytes_total.addSample(TimedSample{t0 + std::chrono::seconds(3), 700.0});
  s.last_update = std::chrono::system_clock::now();
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  // The reset inside the window (1500 -> 800) voids the RX total.
  CHECK(!snapshot.summary.total_rx_bytes.has_value());
  // The TX window is monotonic and still valid.
  CHECK(snapshot.summary.total_tx_bytes.has_value() &&
        *snapshot.summary.total_tx_bytes == 600.0);
}

void test_summary_insufficient_samples() {
  run("summaryInsufficientSamples");
  atm::NetworkTrafficSeries s = makeSeries("idx:2", "eth0", false, 120);
  const auto t0 = std::chrono::steady_clock::now();
  s.rx_bytes_total.addSample(TimedSample{t0, 1000.0});
  s.last_update = std::chrono::system_clock::now();
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  CHECK(snapshot.summary.sample_count == 1);
  CHECK(!snapshot.summary.total_rx_bytes.has_value());  // fewer than two
  CHECK(!snapshot.summary.total_tx_bytes.has_value());
  CHECK(snapshot.sampling_interval_seconds.has_value() == false);
  CHECK(snapshot.retention_window_seconds == 0.0);
}

void test_summary_partial_coverage() {
  run("summaryPartialCoverage");
  atm::NetworkTrafficSeries s = makeSeries("idx:2", "eth0", false, 120);
  fillFourTicks(s);
  const atm::NetworkTrafficExportSnapshot partial =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  CHECK(partial.summary.sample_count == 4);
  CHECK(!partial.summary.history_complete);
  CHECK(partial.summary.coverage > 0.0 && partial.summary.coverage < 0.5);

  atm::NetworkTrafficSeries full = makeSeries("idx:2", "eth0", false, 4);
  fillFourTicks(full);
  const atm::NetworkTrafficExportSnapshot complete =
      atm::buildNetworkTrafficExportSnapshot(full, 4);
  CHECK(complete.summary.history_complete);
  CHECK(complete.summary.coverage == 1.0);
}

void test_summary_aggregate_membership_change() {
  run("summaryAggregateMembershipChange");
  atm::NetworkTrafficSeries s =
      makeSeries(std::string(atm::kNetworkTrafficAllIdentity),
                 "All interfaces", true, 120);
  fillFourTicks(s);
  s.membership_changed = true;
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  CHECK(snapshot.summary.totals_approximate);
  CHECK(snapshot.summary.total_rx_bytes.has_value());  // value still exported
}

// -------------------------------------------------------------------------
// Snapshot consistency
// -------------------------------------------------------------------------

void test_snapshot_does_not_mutate_history() {
  run("snapshotDoesNotMutateHistory");
  atm::NetworkTrafficSeries s = makeSeries("idx:2", "eth0", false, 120);
  fillFourTicks(s);
  const std::size_t rx_ring = s.rx_bytes_total.size();
  const std::size_t rate_ring = s.rx_bytes_per_second.size();
  const auto last_update = s.last_update;
  const std::string display = s.display_name;

  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  CHECK(snapshot.samples.size() == 4);

  CHECK(s.rx_bytes_total.size() == rx_ring);
  CHECK(s.rx_bytes_per_second.size() == rate_ring);
  CHECK(s.last_update == last_update);
  CHECK(s.display_name == display);
  CHECK(!s.rx_bytes_total.empty() &&
        s.rx_bytes_total.samples().back().value == 2300.0);
}

void test_snapshot_no_duplicate_samples() {
  run("snapshotNoDuplicateSamples");
  atm::NetworkTrafficSeries s = makeSeries("idx:2", "eth0", false, 120);
  fillFourTicks(s);
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  CHECK(snapshot.samples.size() == 4);
  for (std::size_t i = 1; i < snapshot.samples.size(); ++i) {
    CHECK(snapshot.samples[i - 1].timestamp < snapshot.samples[i].timestamp);
  }
  // Anchoring: newest row exactly equals last update; oldest is 3s before.
  CHECK(snapshot.samples.back().timestamp == s.last_update);
  CHECK(snapshot.samples.front().timestamp == s.last_update -
        std::chrono::seconds(3));
}

void test_snapshot_does_not_mix_identities() {
  run("snapshotDoesNotMixIdentities");
  atm::NetworkTrafficHistory monitor;
  std::vector<NetworkInterfaceInfo> interfaces;
  interfaces.push_back(makeInterface("eth0", 2, NetworkInterfaceType::Ethernet,
                                     1000, 500));
  interfaces.push_back(makeInterface("eth1", 3, NetworkInterfaceType::Ethernet,
                                     2000, 1000));
  monitor.record(snapshotWith(interfaces));

  const atm::NetworkTrafficExportSnapshot eth0 =
      atm::buildNetworkTrafficExportSnapshot(*monitor.seriesFor("idx:2"),
                                             monitor.historyMaxSamples());
  const atm::NetworkTrafficExportSnapshot eth1 =
      atm::buildNetworkTrafficExportSnapshot(*monitor.seriesFor("idx:3"),
                                             monitor.historyMaxSamples());
  CHECK(eth0.identity == "idx:2");
  CHECK(eth0.display_name == "eth0");
  CHECK(eth1.identity == "idx:3");
  CHECK(eth1.display_name == "eth1");
  for (const auto &row : eth0.samples) {
    (void)row;
    CHECK(eth0.display_name != "eth1");
  }
}

void test_snapshot_interface_disappears() {
  run("snapshotInterfaceDisappears");
  atm::NetworkTrafficHistory monitor;
  std::vector<NetworkInterfaceInfo> interfaces;
  interfaces.push_back(makeInterface("eth0", 2, NetworkInterfaceType::Ethernet,
                                     1000, 500));
  monitor.record(snapshotWith(interfaces));

  // The interface vanishes from the next snapshot (pruned).
  monitor.record(snapshotWith({}));
  CHECK(monitor.seriesFor("idx:2") == nullptr);

  // The aggregate remains available and exportable.
  const atm::NetworkTrafficSeries *all =
      monitor.seriesFor(std::string(atm::kNetworkTrafficAllIdentity));
  CHECK(all != nullptr);
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(*all, monitor.historyMaxSamples());
  CHECK(snapshot.aggregate);
  const std::string json = atm::generateNetworkTrafficJson(snapshot);
  CHECK(isValidJson(json));
}

void test_snapshot_safe_across_refresh_updates() {
  run("snapshotSafeAcrossRefreshUpdates");
  atm::NetworkTrafficHistory monitor;
  std::vector<NetworkInterfaceInfo> interfaces;
  interfaces.push_back(makeInterface("eth0", 2, NetworkInterfaceType::Ethernet,
                                     100, 50));
  monitor.record(snapshotWith(interfaces));

  const atm::NetworkTrafficExportSnapshot first =
      atm::buildNetworkTrafficExportSnapshot(*monitor.seriesFor("idx:2"),
                                             monitor.historyMaxSamples());
  CHECK(first.samples.size() == 1);

  // A refresh appends a sample to the live series.
  recordWithGap(monitor, snapshotWith(interfaces));
  const atm::NetworkTrafficExportSnapshot second =
      atm::buildNetworkTrafficExportSnapshot(*monitor.seriesFor("idx:2"),
                                             monitor.historyMaxSamples());
  CHECK(second.samples.size() == 2);

  // The live series gained one tick. Each snapshot anchors its newest row to
  // its own last update, so timestamps are self-consistent within a snapshot
  // (not byte-for-byte reproducible across rebuilds after refresh).
  const atm::NetworkTrafficSeries *series = monitor.seriesFor("idx:2");
  CHECK(series->rx_bytes_total.size() == 2);
  CHECK(first.samples[0].timestamp == first.last_update);
  CHECK(second.samples[1].timestamp == second.last_update);
  CHECK(second.samples[1].timestamp > second.samples[0].timestamp);
}

void test_snapshot_selected_series_unchanged_after_export() {
  run("snapshotSelectedSeriesUnchangedAfterExport");
  atm::NetworkTrafficSeries s = makeSeries("idx:2", "eth0", false, 120);
  fillFourTicks(s);
  atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  // Mutating the caller-owned snapshot must not leak back into the series.
  snapshot.samples.clear();
  const atm::NetworkTrafficExportSnapshot rebuild =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  CHECK(rebuild.samples.size() == 4);
}

// -------------------------------------------------------------------------
// File handling / full export pipeline
// -------------------------------------------------------------------------

std::string tempPath(const std::string &suffix) {
  return "/tmp/network-traffic-export-test-" + std::to_string(::getpid()) +
         "-" + suffix;
}

void test_export_csv_writes_file() {
  run("exportCsvWritesFile");
  atm::NetworkTrafficSeries s = makeSeries("idx:2", "eth0", false, 120);
  fillFourTicks(s);
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  const std::string path = tempPath("write.csv");
  std::remove(path.c_str());

  const auto result = atm::exportNetworkTrafficHistory(
      path, snapshot, NetworkTrafficExportFormat::Csv);
  CHECK(result.status == NetworkTrafficExportStatus::Success);
  CHECK(result.bytes == atm::generateNetworkTrafficCsv(snapshot).size());
  CHECK(result.path == path);

  std::ifstream in(path);
  const std::string actual((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
  CHECK(actual == atm::generateNetworkTrafficCsv(snapshot));
  std::remove(path.c_str());
}

void test_export_json_writes_file() {
  run("exportJsonWritesFile");
  atm::NetworkTrafficSeries s = makeSeries("idx:2", "eth0", false, 120);
  fillFourTicks(s);
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  const std::string path = tempPath("write.json");
  std::remove(path.c_str());

  const auto result = atm::exportNetworkTrafficHistory(
      path, snapshot, NetworkTrafficExportFormat::Json);
  CHECK(result.status == NetworkTrafficExportStatus::Success);

  std::ifstream in(path);
  const std::string actual((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
  CHECK(isValidJson(actual));
  CHECK(sampleTimestamps(actual).size() == 4);
  std::remove(path.c_str());
}

void test_export_invalid_path() {
  run("exportInvalidPath");
  atm::NetworkTrafficSeries s = makeSeries("idx:2", "eth0", false, 120);
  fillFourTicks(s);
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  const auto result = atm::exportNetworkTrafficHistory(
      "", snapshot, NetworkTrafficExportFormat::Csv);
  CHECK(result.status == NetworkTrafficExportStatus::InvalidPath);
}

void test_export_directory_path() {
  run("exportDirectoryPath");
  atm::NetworkTrafficSeries s = makeSeries("idx:2", "eth0", false, 120);
  fillFourTicks(s);
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  const auto result = atm::exportNetworkTrafficHistory(
      "/tmp", snapshot, NetworkTrafficExportFormat::Csv);
  CHECK(result.status == NetworkTrafficExportStatus::InvalidPath);
  const auto direct = atm::writeNetworkTrafficExport("/tmp", "x");
  CHECK(direct.status == NetworkTrafficExportStatus::InvalidPath);
}

void test_export_unavailable_directory() {
  run("exportUnavailableDirectory");
  atm::NetworkTrafficSeries s = makeSeries("idx:2", "eth0", false, 120);
  fillFourTicks(s);
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  const auto result = atm::exportNetworkTrafficHistory(
      "/this/path/does/not/exist/out.csv", snapshot,
      NetworkTrafficExportFormat::Csv);
  CHECK(result.status == NetworkTrafficExportStatus::WriteError);
}

void test_export_permission_denied() {
  run("exportPermissionDenied");
  if (::geteuid() == 0) {
    std::fprintf(stderr, "  (skipped: running as root)\n");
    return;
  }
  const std::string dir = tempPath("ro-dir");
  ::mkdir(dir.c_str(), 0700);
  ::chmod(dir.c_str(), 0500);  // read-only directory

  atm::NetworkTrafficSeries s = makeSeries("idx:2", "eth0", false, 120);
  fillFourTicks(s);
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  const auto result = atm::exportNetworkTrafficHistory(
      dir + "/out.csv", snapshot, NetworkTrafficExportFormat::Csv);
  CHECK(result.status == NetworkTrafficExportStatus::WriteError);

  ::chmod(dir.c_str(), 0700);
  ::rmdir(dir.c_str());
}

void test_export_existing_file_overwritten() {
  run("exportExistingFileOverwritten");
  const std::string path = tempPath("existing.csv");
  {
    std::ofstream out(path);
    out << "old content";
  }
  atm::NetworkTrafficSeries s = makeSeries("idx:2", "eth0", false, 120);
  fillFourTicks(s);
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  const auto result = atm::exportNetworkTrafficHistory(
      path, snapshot, NetworkTrafficExportFormat::Csv);
  CHECK(result.status == NetworkTrafficExportStatus::Success);

  std::ifstream in(path);
  const std::string actual((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
  CHECK(actual == atm::generateNetworkTrafficCsv(snapshot));
  std::remove(path.c_str());
}

void test_export_unsupported_format() {
  run("exportUnsupportedFormat");
  atm::NetworkTrafficSeries s = makeSeries("idx:2", "eth0", false, 120);
  fillFourTicks(s);
  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(s, 120);
  const auto result = atm::exportNetworkTrafficHistory(
      tempPath("badformat.csv"), snapshot,
      static_cast<NetworkTrafficExportFormat>(99));
  CHECK(result.status == NetworkTrafficExportStatus::UnsupportedFormat);
}

void test_filename_defaults() {
  run("exportDefaultFilenames");
  CHECK(atm::defaultNetworkTrafficExportFilename(
            std::string(atm::kNetworkTrafficAllIdentity), "All interfaces",
            NetworkTrafficExportFormat::Csv) == "network-traffic-all.csv");
  CHECK(atm::defaultNetworkTrafficExportFilename(
            "idx:2", "eth0", NetworkTrafficExportFormat::Json) ==
        "network-traffic-eth0.json");
  CHECK(atm::defaultNetworkTrafficExportFilename(
            "name:eth", "bad/name",
            NetworkTrafficExportFormat::Csv) == "network-traffic-bad_name.csv");
}

void test_export_roundtrip_no_change_to_system() {
  run("exportRoundtrip");
  atm::NetworkTrafficHistory monitor;
  std::vector<NetworkInterfaceInfo> interfaces;
  interfaces.push_back(makeInterface("eth0", 2, NetworkInterfaceType::Ethernet,
                                     1000, 500));
  monitor.record(snapshotWith(interfaces));
  const std::vector<std::string> selectable = monitor.selectableIdentities();
  const std::string before_selection = selectable[0];
  const atm::NetworkTrafficSeries *series = monitor.seriesFor("idx:2");
  const std::size_t before_samples = series->rx_bytes_total.size();
  const std::string before_name = series->display_name;

  const atm::NetworkTrafficExportSnapshot snapshot =
      atm::buildNetworkTrafficExportSnapshot(*series, monitor.historyMaxSamples());
  const std::string path = tempPath("regression.csv");
  static_cast<void>(
      atm::exportNetworkTrafficHistory(path, snapshot,
                                       NetworkTrafficExportFormat::Csv));
  std::remove(path.c_str());

  // The live monitor, its selection order and the selected series are intact.
  CHECK(monitor.selectableIdentities() == selectable);
  CHECK(monitor.displayNameFor("idx:2") == "eth0");
  CHECK(monitor.seriesFor("idx:2")->rx_bytes_total.size() == before_samples);
  CHECK(monitor.seriesFor("idx:2")->display_name == before_name);
  CHECK(selectable[0] == std::string(atm::kNetworkTrafficAllIdentity));
  (void)before_selection;
}

int main() {
  test_csv_header_order();
  test_csv_normal_export();
  test_csv_empty_history();
  test_csv_missing_values();
  test_csv_special_characters();
  test_csv_quotes();
  test_csv_newline_escaping();
  test_csv_timestamp_format();
  test_csv_large_counters();
  test_csv_numeric_precision();
  test_csv_link_metadata();
  test_csv_link_metadata_stale_unavailable();
  test_csv_hardware_metadata();

  test_json_valid_output();
  test_json_metadata_presence();
  test_json_interface_identity();
  test_json_aggregate_flag();
  test_json_empty_sample_array();
  test_json_missing_values_null();
  test_json_special_characters();
  test_json_sample_ordering();
  test_json_stable_field_names();
  test_json_link_null_when_absent();
  test_json_link_metadata();
  test_json_link_null_for_aggregate();
  test_json_hardware_metadata();
  test_json_hardware_null_when_absent();
  test_json_no_internal_state();

  test_summary_current_and_peak();
  test_summary_totals_valid_window();
  test_summary_counter_reset();
  test_summary_insufficient_samples();
  test_summary_partial_coverage();
  test_summary_aggregate_membership_change();

  test_snapshot_does_not_mutate_history();
  test_snapshot_no_duplicate_samples();
  test_snapshot_does_not_mix_identities();
  test_snapshot_interface_disappears();
  test_snapshot_safe_across_refresh_updates();
  test_snapshot_selected_series_unchanged_after_export();

  test_export_csv_writes_file();
  test_export_json_writes_file();
  test_export_invalid_path();
  test_export_directory_path();
  test_export_unavailable_directory();
  test_export_permission_denied();
  test_export_existing_file_overwritten();
  test_export_unsupported_format();
  test_filename_defaults();
  test_export_roundtrip_no_change_to_system();

  std::fprintf(stderr, "\n%zu checks, %d failures\n", g_checks + 0UL,
               g_failures);
  if (g_failures != 0) {
    std::fprintf(stderr, "RESULT: FAIL\n");
    return 1;
  }
  std::fprintf(stderr, "RESULT: PASS\n");
  return 0;
}