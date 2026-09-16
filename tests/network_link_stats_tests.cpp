#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

#include "network_link_stats.hpp"

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
using atm::NetworkLinkStatMetric;
using atm::NetworkLinkStatRates;
using atm::NetworkLinkStatRead;
using atm::NetworkLinkStatSample;
using atm::NetworkLinkStatState;
using atm::NetworkLinkStats;
using atm::NetworkLinkStatSummary;
using atm::NetworkLinkStatsMonitor;

using Steady = std::chrono::steady_clock;
using System = std::chrono::system_clock;

namespace {

const std::size_t kRxErrors =
    static_cast<std::size_t>(NetworkLinkStatMetric::RxErrors);
const std::size_t kTxErrors =
    static_cast<std::size_t>(NetworkLinkStatMetric::TxErrors);
const std::size_t kRxDropped =
    static_cast<std::size_t>(NetworkLinkStatMetric::RxDropped);
const std::size_t kTxDropped =
    static_cast<std::size_t>(NetworkLinkStatMetric::TxDropped);
const std::size_t kRxMissed =
    static_cast<std::size_t>(NetworkLinkStatMetric::RxMissedErrors);
const std::size_t kTxFifo =
    static_cast<std::size_t>(NetworkLinkStatMetric::TxFifoErrors);
const std::size_t kRxFifo =
    static_cast<std::size_t>(NetworkLinkStatMetric::RxFifoErrors);
const std::size_t kCollisions =
    static_cast<std::size_t>(NetworkLinkStatMetric::Collisions);
const std::size_t kRxCompressed =
    static_cast<std::size_t>(NetworkLinkStatMetric::RxCompressed);
const std::size_t kRxNohandler =
    static_cast<std::size_t>(NetworkLinkStatMetric::RxNohandler);

std::string metricName(NetworkLinkStatMetric metric) {
  return atm::networkLinkStatMetricName(metric);
}

bool near(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) < eps;
}

void writeFile(const std::filesystem::path &path,
               const std::string &content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << content;
}

/// Builds `<root>/sys/class/net/<name>/statistics` and returns its path.
std::filesystem::path makeStatisticsDir(const std::filesystem::path &root,
                                        const std::string &name) {
  const std::filesystem::path stats =
      root / "sys" / "class" / "net" / name / "statistics";
  std::filesystem::create_directories(stats);
  return stats;
}

NetworkInterfaceInfo makeInfo(const std::string &name, int ifindex,
                              NetworkInterfaceType type) {
  NetworkInterfaceInfo info;
  info.name = name;
  info.type = type;
  info.link.ifindex = ifindex;
  info.refreshed_at = System::now();
  return info;
}

NetworkInterfaceSnapshot makeSnapshot(
    const std::vector<NetworkInterfaceInfo> &interfaces) {
  NetworkInterfaceSnapshot snapshot;
  snapshot.interfaces = interfaces;
  snapshot.sysfs_readable = true;
  snapshot.addresses_readable = true;
  snapshot.refreshed_at = System::now();
  return snapshot;
}

/// A valid read, expressed directly (no filesystem).
NetworkLinkStatRead makeRead(std::uint64_t rx_errors, std::uint64_t tx_errors,
                             std::uint64_t rx_dropped,
                             std::uint64_t tx_dropped,
                             std::uint64_t collisions) {
  NetworkLinkStatRead read;
  read.read_ok = true;
  read.any_available = true;
  read.counters[kRxErrors] = rx_errors;
  read.counters[kTxErrors] = tx_errors;
  read.counters[kRxDropped] = rx_dropped;
  read.counters[kTxDropped] = tx_dropped;
  read.counters[kCollisions] = collisions;
  return read;
}

}  // namespace

// -------------------------------------------------------------------------
// Name tables / state names
// -------------------------------------------------------------------------

void test_metric_name_table() {
  run("MetricNameTable");
  CHECK(atm::kNetworkLinkStatMetricCount == 19);
  CHECK(std::string(metricName(NetworkLinkStatMetric::RxErrors)) == "rx_errors");
  CHECK(std::string(metricName(NetworkLinkStatMetric::TxErrors)) == "tx_errors");
  CHECK(std::string(metricName(NetworkLinkStatMetric::RxDropped)) == "rx_dropped");
  CHECK(std::string(metricName(NetworkLinkStatMetric::TxDropped)) == "tx_dropped");
  CHECK(std::string(metricName(NetworkLinkStatMetric::RxMissedErrors)) ==
        "rx_missed_errors");
  CHECK(std::string(metricName(NetworkLinkStatMetric::TxFifoErrors)) ==
        "tx_fifo_errors");
  CHECK(std::string(metricName(NetworkLinkStatMetric::RxFifoErrors)) ==
        "rx_fifo_errors");
  CHECK(std::string(metricName(NetworkLinkStatMetric::Collisions)) ==
        "collisions");
  CHECK(std::string(metricName(NetworkLinkStatMetric::RxOverErrors)) ==
        "rx_over_errors");
  CHECK(std::string(metricName(NetworkLinkStatMetric::RxFrameErrors)) ==
        "rx_frame_errors");
  CHECK(std::string(metricName(NetworkLinkStatMetric::RxLengthErrors)) ==
        "rx_length_errors");
  CHECK(std::string(metricName(NetworkLinkStatMetric::RxCrcErrors)) ==
        "rx_crc_errors");
  CHECK(std::string(metricName(NetworkLinkStatMetric::RxCompressed)) ==
        "rx_compressed");
  CHECK(std::string(metricName(NetworkLinkStatMetric::TxCompressed)) ==
        "tx_compressed");
  CHECK(std::string(metricName(NetworkLinkStatMetric::TxCarrierErrors)) ==
        "tx_carrier_errors");
  CHECK(std::string(metricName(NetworkLinkStatMetric::TxHeartbeatErrors)) ==
        "tx_heartbeat_errors");
  CHECK(std::string(metricName(NetworkLinkStatMetric::TxWindowErrors)) ==
        "tx_window_errors");
  CHECK(std::string(metricName(NetworkLinkStatMetric::Multicast)) ==
        "multicast");
  CHECK(std::string(metricName(NetworkLinkStatMetric::RxNohandler)) ==
        "rx_nohandler");
  CHECK(std::string(metricName(static_cast<NetworkLinkStatMetric>(
            atm::kNetworkLinkStatMetricCount))) == "unknown");
}

void test_state_names() {
  run("StateNames");
  CHECK(std::string(atm::networkLinkStatStateName(
            NetworkLinkStatState::Unknown)) == "unknown");
  CHECK(std::string(atm::networkLinkStatStateName(
            NetworkLinkStatState::Valid)) == "valid");
  CHECK(std::string(atm::networkLinkStatStateName(
            NetworkLinkStatState::Stale)) == "stale");
  CHECK(std::string(atm::networkLinkStatStateName(
            NetworkLinkStatState::Unavailable)) == "unavailable");
}

// -------------------------------------------------------------------------
// readNetworkLinkStats: hermetically built sysfs trees
// -------------------------------------------------------------------------

void test_read_full_statistics_tree() {
  run("ReadFullStatisticsTree");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-nls-test-full";
  std::filesystem::remove_all(root);
  const std::filesystem::path stats = makeStatisticsDir(root, "eth0");
  writeFile(stats / "rx_errors", "37\n");
  writeFile(stats / "tx_errors", "2\n");
  writeFile(stats / "rx_dropped", "11\n");
  writeFile(stats / "tx_dropped", "0\n");
  writeFile(stats / "rx_missed_errors", "0\n");
  writeFile(stats / "tx_fifo_errors", "1\n");
  writeFile(stats / "rx_fifo_errors", "0\n");
  writeFile(stats / "collisions", "9\n");
  writeFile(stats / "rx_over_errors", "0\n");
  writeFile(stats / "rx_frame_errors", "4\n");
  writeFile(stats / "rx_length_errors", "0\n");
  writeFile(stats / "rx_crc_errors", "6\n");
  writeFile(stats / "rx_compressed", "0\n");
  writeFile(stats / "tx_compressed", "0\n");
  writeFile(stats / "tx_carrier_errors", "0\n");
  writeFile(stats / "tx_heartbeat_errors", "0\n");
  writeFile(stats / "tx_window_errors", "0\n");
  writeFile(stats / "multicast", "123\n");
  writeFile(stats / "rx_nohandler", "0\n");

  const NetworkLinkStatRead read = atm::readNetworkLinkStats(root, "eth0");
  CHECK(read.read_ok);
  CHECK(!read.access_denied);
  CHECK(read.any_available);
  CHECK(*read.counters[kRxErrors] == 37);
  CHECK(*read.counters[kTxErrors] == 2);
  CHECK(*read.counters[kRxDropped] == 11);
  CHECK(*read.counters[kTxDropped] == 0);
  CHECK(*read.counters[kCollisions] == 9);
  CHECK(*read.counters[static_cast<std::size_t>(
            NetworkLinkStatMetric::Multicast)] == 123);
  std::filesystem::remove_all(root);
}

void test_read_partial_counters() {
  run("ReadPartialCounters");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-nls-test-partial";
  std::filesystem::remove_all(root);
  const std::filesystem::path stats = makeStatisticsDir(root, "wlan0");
  writeFile(stats / "rx_errors", "5\n");
  writeFile(stats / "tx_errors", "1\n");
  writeFile(stats / "rx_dropped", "80\n");

  const NetworkLinkStatRead read = atm::readNetworkLinkStats(root, "wlan0");
  CHECK(read.read_ok);
  CHECK(!read.access_denied);
  CHECK(read.any_available);
  CHECK(*read.counters[kRxErrors] == 5);
  CHECK(*read.counters[kTxErrors] == 1);
  CHECK(*read.counters[kRxDropped] == 80);
  CHECK(!read.counters[kTxDropped].has_value());
  CHECK(!read.counters[kCollisions].has_value());
  CHECK(!read.counters[static_cast<std::size_t>(
            NetworkLinkStatMetric::RxNohandler)].has_value());
  std::filesystem::remove_all(root);
}

void test_read_malformed_counters() {
  run("ReadMalformedCounters");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-nls-test-malformed";
  std::filesystem::remove_all(root);
  const std::filesystem::path stats = makeStatisticsDir(root, "eth0");
  writeFile(stats / "rx_errors", "not-a-number\n");
  writeFile(stats / "tx_errors", "12.5\n");
  writeFile(stats / "rx_dropped", "-3\n");
  writeFile(stats / "tx_dropped", " 7 \n");  // whitespace is trimmed
  writeFile(stats / "collisions", "12x\n");
  writeFile(stats / "rx_missed_errors", "0x10\n");
  writeFile(stats / "rx_fifo_errors", "18446744073709551616\n");
  writeFile(stats / "rx_frame_errors", "3\n");
  writeFile(stats / "rx_compressed", "\n");
  writeFile(stats / "tx_compressed", "0\n");

  const NetworkLinkStatRead read = atm::readNetworkLinkStats(root, "eth0");
  CHECK(read.read_ok);
  CHECK(!read.access_denied);
  CHECK(read.any_available);
  CHECK(!read.counters[kRxErrors].has_value());
  CHECK(!read.counters[kTxErrors].has_value());
  CHECK(!read.counters[kRxDropped].has_value());
  CHECK(*read.counters[kTxDropped] == 7);  // valid after trimming
  CHECK(!read.counters[kCollisions].has_value());
  CHECK(!read.counters[kRxMissed].has_value());
  CHECK(!read.counters[kRxFifo].has_value());
  CHECK(*read.counters[static_cast<std::size_t>(
            NetworkLinkStatMetric::RxFrameErrors)] == 3);
  CHECK(!read.counters[kRxCompressed].has_value());
  CHECK(*read.counters[static_cast<std::size_t>(
            NetworkLinkStatMetric::TxCompressed)] == 0);
  std::filesystem::remove_all(root);
}

void test_read_large_counter() {
  run("ReadLargeCounter");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-nls-test-large";
  std::filesystem::remove_all(root);
  const std::filesystem::path stats = makeStatisticsDir(root, "eth0");
  writeFile(stats / "rx_errors", "18446744073709551615\n");
  writeFile(stats / "tx_errors", "18446744073709551616\n");

  const NetworkLinkStatRead read = atm::readNetworkLinkStats(root, "eth0");
  CHECK(read.read_ok);
  CHECK(read.any_available);
  CHECK(*read.counters[kRxErrors] ==
        std::numeric_limits<std::uint64_t>::max());
  CHECK(!read.counters[kTxErrors].has_value());
  std::filesystem::remove_all(root);
}

void test_read_missing_statistics_dir() {
  run("ReadMissingStatisticsDir");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-nls-test-missing";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "sys" / "class" / "net");

  const NetworkLinkStatRead read =
      atm::readNetworkLinkStats(root, "no-such-iface");
  CHECK(!read.read_ok);
  CHECK(!read.access_denied);
  CHECK(!read.any_available);
  std::filesystem::remove_all(root);
}

void test_read_denied_statistics_dir() {
  run("ReadDeniedStatisticsDir");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-nls-test-denied";
  std::filesystem::remove_all(root);
  const std::filesystem::path stats = makeStatisticsDir(root, "eth0");
  writeFile(stats / "rx_errors", "3\n");
  const bool not_root = ::getuid() != 0;
  if (not_root) {
    std::filesystem::permissions(
        stats, std::filesystem::perms::owner_write |
                   std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace);
  }
  const NetworkLinkStatRead read = atm::readNetworkLinkStats(root, "eth0");
  if (not_root) {
    CHECK(read.access_denied);
    CHECK(!read.read_ok);
  } else {
    CHECK(read.read_ok);  // root bypasses directory permission checks
  }
  if (not_root) {
    std::filesystem::permissions(stats,
                                 std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::add);
  }
  std::filesystem::remove_all(root);
}

// -------------------------------------------------------------------------
// deriveNetworkLinkStatRates
// -------------------------------------------------------------------------

void test_derive_rates() {
  run("DeriveRates");
  NetworkLinkStatSample previous;
  previous.valid = true;
  previous.counters[kRxErrors] = 1000;
  previous.counters[kTxErrors] = 50;
  previous.counters[kRxDropped] = 10;
  previous.counters[kTxDropped] = 4;
  previous.counters[kCollisions] = 5;
  previous.counters[kRxMissed] = 1;
  previous.counters[kRxFifo] = 2;
  previous.counters[kTxFifo] = 3;
  previous.counters[static_cast<std::size_t>(
      NetworkLinkStatMetric::RxCrcErrors)] = 6;
  previous.counters[static_cast<std::size_t>(
      NetworkLinkStatMetric::RxFrameErrors)] = 7;
  previous.counters[static_cast<std::size_t>(
      NetworkLinkStatMetric::TxCarrierErrors)] = 8;

  NetworkLinkStatSample current = previous;
  current.counters[kRxErrors] = 1600;  // +600
  current.counters[kTxErrors] = 55;    // +5
  current.counters[kRxDropped] = 12;   // +2
  current.counters[kTxDropped] = 6;    // +2
  current.counters[kCollisions] = 9;   // +4
  current.counters[kRxMissed] = 2;     // +1
  current.counters[kRxFifo] = 2;       // 0
  current.counters[kTxFifo] = 6;       // +3
  current.counters[static_cast<std::size_t>(
      NetworkLinkStatMetric::RxCrcErrors)] = 8;  // +2
  current.counters[static_cast<std::size_t>(
      NetworkLinkStatMetric::RxFrameErrors)] = 7;  // 0
  current.counters[static_cast<std::size_t>(
      NetworkLinkStatMetric::TxCarrierErrors)] = 18;  // +10

  const NetworkLinkStatRates rates =
      atm::deriveNetworkLinkStatRates(previous, current, 5.0);
  CHECK(rates.rx_errors_per_second.has_value());
  CHECK(near(*rates.rx_errors_per_second, 120.0));
  CHECK(near(*rates.tx_errors_per_second, 1.0));
  CHECK(near(*rates.rx_dropped_per_second, 0.4));
  CHECK(near(*rates.tx_dropped_per_second, 0.4));
  CHECK(near(*rates.combined_errors_per_second, 121.0));
  CHECK(near(*rates.combined_drops_per_second, 0.8));
  CHECK(near(*rates.collisions_per_second, 0.8));
  CHECK(near(*rates.rx_missed_errors_per_second, 0.2));
  CHECK(near(*rates.rx_fifo_errors_per_second, 0.0));
  CHECK(near(*rates.tx_fifo_errors_per_second, 0.6));
  CHECK(near(*rates.rx_crc_errors_per_second, 0.4));
  CHECK(near(*rates.rx_frame_errors_per_second, 0.0));
  CHECK(near(*rates.tx_carrier_errors_per_second, 2.0));
}

void test_derive_missing_and_decreased() {
  run("DeriveMissingAndDecreased");
  NetworkLinkStatSample previous;
  previous.valid = true;
  previous.counters[kRxErrors] = 1000;
  previous.counters[kTxErrors] = 50;  // valid at previous
  previous.counters[kRxDropped] = 10;

  NetworkLinkStatSample current;  // default: all counters missing
  current.valid = true;
  current.counters[kTxErrors] = 30;   // decreased -> reset
  current.counters[kRxDropped] = 10;  // unchanged

  const NetworkLinkStatRates rates =
      atm::deriveNetworkLinkStatRates(previous, current, 5.0);
  CHECK(!rates.rx_errors_per_second.has_value());  // missing at current
  CHECK(!rates.tx_errors_per_second.has_value());  // decreased
  CHECK(near(*rates.rx_dropped_per_second, 0.0));
  CHECK(!rates.combined_errors_per_second.has_value());
  CHECK(!rates.combined_drops_per_second.has_value());  // tx_dropped missing
  CHECK(!rates.collisions_per_second.has_value());
}

void test_derive_invalid_windows() {
  run("DeriveInvalidWindows");
  NetworkLinkStatSample previous;
  previous.valid = true;
  previous.counters[kRxErrors] = 100;
  NetworkLinkStatSample current = previous;
  current.counters[kRxErrors] = 300;

  const NetworkLinkStatRates zero =
      atm::deriveNetworkLinkStatRates(previous, current, 0.0);
  CHECK(!zero.rx_errors_per_second.has_value());
  const NetworkLinkStatRates negative =
      atm::deriveNetworkLinkStatRates(previous, current, -2.0);
  CHECK(!negative.rx_errors_per_second.has_value());
  const NetworkLinkStatRates nan =
      atm::deriveNetworkLinkStatRates(previous, current,
                                      std::numeric_limits<double>::quiet_NaN());
  CHECK(!nan.rx_errors_per_second.has_value());
  const NetworkLinkStatRates inf = atm::deriveNetworkLinkStatRates(
      previous, current, std::numeric_limits<double>::infinity());
  CHECK(!inf.rx_errors_per_second.has_value());

  NetworkLinkStatSample stale_previous = previous;
  stale_previous.valid = false;
  const NetworkLinkStatRates invalid_prev =
      atm::deriveNetworkLinkStatRates(stale_previous, current, 5.0);
  CHECK(!invalid_prev.rx_errors_per_second.has_value());
  NetworkLinkStatSample stale_current = current;
  stale_current.valid = false;
  const NetworkLinkStatRates invalid_cur =
      atm::deriveNetworkLinkStatRates(previous, stale_current, 5.0);
  CHECK(!invalid_cur.rx_errors_per_second.has_value());
}

// -------------------------------------------------------------------------
// updateNetworkLinkStats
// -------------------------------------------------------------------------

void test_update_first_sample_baseline() {
  run("UpdateFirstSampleBaseline");
  const Steady::time_point t0 = Steady::now();
  const System::time_point wall0 = System::now();

  NetworkLinkStats empty;
  empty.identity = "idx:2";

  const NetworkLinkStats first =
      atm::updateNetworkLinkStats(empty, makeRead(1000, 50, 10, 4, 5), t0,
                                  wall0, 8);
  CHECK(first.state == NetworkLinkStatState::Valid);
  CHECK(first.history.size() == 1);
  CHECK(first.last_update == wall0);
  const NetworkLinkStatSample &sample = first.history.samples().back();
  CHECK(sample.valid);
  CHECK(sample.any_available);
  CHECK(*sample.counters[kRxErrors] == 1000);
  CHECK(!sample.rates.rx_errors_per_second.has_value());  // no baseline yet
  CHECK(sample.discontinuity_count == 0);
}

void test_update_rate_window() {
  run("UpdateRateWindow");
  const Steady::time_point t0 = Steady::now();
  const System::time_point wall0 = System::now();
  const System::time_point wall1 = wall0 + std::chrono::seconds(1);

  NetworkLinkStats empty;
  empty.identity = "idx:2";
  const NetworkLinkStats base =
      atm::updateNetworkLinkStats(empty, makeRead(1000, 50, 10, 4, 5), t0,
                                  wall0, 8);

  const NetworkLinkStats next = atm::updateNetworkLinkStats(
      base, makeRead(1600, 50, 10, 6, 7), t0 + std::chrono::seconds(10), wall1,
      8);
  CHECK(next.state == NetworkLinkStatState::Valid);
  CHECK(next.history.size() == 2);
  CHECK(next.last_update == wall1);
  const NetworkLinkStatSample &latest = next.history.samples().back();
  CHECK(latest.valid);
  CHECK(near(*latest.rates.rx_errors_per_second, 60.0));   // 600 / 10
  CHECK(near(*latest.rates.tx_errors_per_second, 0.0));    // 0 / 10
  CHECK(near(*latest.rates.rx_dropped_per_second, 0.0));
  CHECK(near(*latest.rates.tx_dropped_per_second, 0.2));   // 2 / 10
  CHECK(near(*latest.rates.collisions_per_second, 0.2));   // 2 / 10
  CHECK(near(*latest.rates.combined_errors_per_second, 60.0));
  CHECK(near(*latest.rates.combined_drops_per_second, 0.2));
  CHECK(latest.discontinuity_count == 0);
}

void test_update_stale_tick_preserves() {
  run("UpdateStaleTickPreserves");
  const Steady::time_point t0 = Steady::now();
  const System::time_point wall0 = System::now();

  NetworkLinkStats empty;
  empty.identity = "idx:2";
  const NetworkLinkStats base =
      atm::updateNetworkLinkStats(empty, makeRead(1000, 50, 10, 4, 5), t0,
                                  wall0, 8);

  NetworkLinkStatRead denied;
  denied.read_ok = false;
  denied.access_denied = true;
  const NetworkLinkStats stale =
      atm::updateNetworkLinkStats(base, denied,
                                  t0 + std::chrono::seconds(1),
                                  wall0 + std::chrono::seconds(1), 8);
  CHECK(stale.state == NetworkLinkStatState::Stale);
  CHECK(stale.history.size() == 2);
  const NetworkLinkStatSample &sample = stale.history.samples().back();
  CHECK(!sample.valid);
  CHECK(sample.any_available == false);
  CHECK(*sample.counters[kRxErrors] == 1000);  // carried, gap-free
  CHECK(*sample.counters[kCollisions] == 5);
  CHECK(!sample.rates.rx_errors_per_second.has_value());
}

void test_update_clean_absence_unavailable() {
  run("UpdateCleanAbsenceUnavailable");
  NetworkLinkStats empty;
  empty.identity = "idx:2";
  NetworkLinkStatRead absent;
  absent.read_ok = false;  // clean absence: no access_denied
  const NetworkLinkStats unavailable = atm::updateNetworkLinkStats(
      empty, absent, Steady::now(), System::now(), 8);
  CHECK(unavailable.state == NetworkLinkStatState::Unavailable);
  CHECK(!unavailable.history.samples().back().valid);
}

void test_update_readable_but_empty_unavailable() {
  run("UpdateReadableButEmptyUnavailable");
  NetworkLinkStats empty;
  empty.identity = "idx:2";
  NetworkLinkStatRead empty_dir;
  empty_dir.read_ok = true;  // listed, but no counter carried a value
  const NetworkLinkStats unavailable = atm::updateNetworkLinkStats(
      empty, empty_dir, Steady::now(), System::now(), 8);
  CHECK(unavailable.state == NetworkLinkStatState::Unavailable);
  CHECK(!unavailable.history.samples().back().valid);
}

void test_update_reset_discontinuity() {
  run("UpdateResetDiscontinuity");
  const Steady::time_point t0 = Steady::now();
  const System::time_point wall0 = System::now();

  NetworkLinkStats empty;
  empty.identity = "idx:2";
  const NetworkLinkStats base =
      atm::updateNetworkLinkStats(empty, makeRead(1000, 50, 10, 4, 5), t0,
                                  wall0, 8);

  const NetworkLinkStats reset = atm::updateNetworkLinkStats(
      base, makeRead(900, 55, 10, 4, 9), t0 + std::chrono::seconds(5), wall0,
      8);
  CHECK(reset.discontinuity_count == 1);
  const NetworkLinkStatSample &latest = reset.history.samples().back();
  CHECK(latest.discontinuity_count == 1);
  // Only rx_errors decreased: it is unavailable, the others still compute.
  CHECK(!latest.rates.rx_errors_per_second.has_value());
  CHECK(near(*latest.rates.tx_errors_per_second, 1.0));  // 5 / 5
  CHECK(near(*latest.rates.collisions_per_second, 0.8));  // 4 / 5
}

void test_update_long_gap_no_false_rate() {
  run("UpdateLongGapNoFalseRate");
  const Steady::time_point t0 = Steady::now();
  const System::time_point wall0 = System::now();

  NetworkLinkStats empty;
  empty.identity = "idx:2";
  const NetworkLinkStats base =
      atm::updateNetworkLinkStats(empty, makeRead(1000, 50, 10, 4, 5), t0,
                                  wall0, 8);

  // A stale tick in between must not become the rate baseline.
  NetworkLinkStatRead denied;
  denied.read_ok = false;
  denied.access_denied = true;
  const NetworkLinkStats stale = atm::updateNetworkLinkStats(
      base, denied, t0 + std::chrono::seconds(1), wall0, 8);

  const NetworkLinkStats next = atm::updateNetworkLinkStats(
      stale, makeRead(1600, 52, 12, 4, 7),
      t0 + std::chrono::seconds(11), wall0, 8);
  CHECK(next.state == NetworkLinkStatState::Valid);
  const NetworkLinkStatSample &latest = next.history.samples().back();
  // Baseline is the last VALID sample (1000 at t0), not the stale tick: 600/11.
  CHECK(near(*latest.rates.rx_errors_per_second, 600.0 / 11.0));
  CHECK(near(*latest.rates.tx_errors_per_second, 2.0 / 11.0));
}

void test_update_ring_bounded() {
  run("UpdateRingBounded");
  NetworkLinkStats record;
  record.identity = "idx:2";
  const Steady::time_point t0 = Steady::now();
  const System::time_point wall = System::now();

  for (int i = 0; i < 12; ++i) {
    record = atm::updateNetworkLinkStats(
        record, makeRead(1000u + static_cast<std::uint64_t>(i), 0, 0, 0, 0),
        t0 + std::chrono::seconds(i), wall, 8);
  }
  CHECK(record.history.size() == 8);  // oldest dropped
  CHECK(record.history.samples().front().counters[kRxErrors].has_value());
  CHECK(*record.history.samples().front().counters[kRxErrors] ==
        1000u + 12u - 8u);
}

// -------------------------------------------------------------------------
// summarizeNetworkLinkStats
// -------------------------------------------------------------------------

void test_summarize_baseline() {
  run("SummarizeBaseline");
  const Steady::time_point t0 = Steady::now();
  const System::time_point wall0 = System::now();

  NetworkLinkStats empty;
  empty.identity = "idx:2";
  const NetworkLinkStats first =
      atm::updateNetworkLinkStats(empty, makeRead(1000, 50, 10, 4, 5), t0,
                                  wall0, 8);
  const NetworkLinkStatSummary summary = atm::summarizeNetworkLinkStats(first, 8);
  CHECK(summary.has_data);
  CHECK(summary.has_valid_data);
  CHECK(summary.sample_count == 1);
  CHECK(summary.valid_sample_count == 1);
  CHECK(summary.stale_sample_count == 0);
  CHECK(summary.discontinuity_count == 0);
  CHECK(near(summary.coverage, 1.0 / 8.0));
  CHECK(near(summary.span_seconds, 0.0));
  CHECK(summary.last_update == wall0);
  CHECK(*summary.current[kRxErrors] == 1000);
  CHECK(!summary.rates.rx_errors_per_second.has_value());
  CHECK(!summary.window_delta[kRxErrors].has_value());  // single valid sample
}

void test_summarize_window() {
  run("SummarizeWindow");
  const Steady::time_point t0 = Steady::now();
  const System::time_point wall0 = System::now();
  const System::time_point wall1 = wall0 + std::chrono::seconds(1);

  NetworkLinkStats empty;
  empty.identity = "idx:2";
  const NetworkLinkStats base =
      atm::updateNetworkLinkStats(empty, makeRead(1000, 50, 10, 4, 5), t0,
                                  wall0, 8);
  const NetworkLinkStats next = atm::updateNetworkLinkStats(
      base, makeRead(1600, 50, 12, 6, 7), t0 + std::chrono::seconds(10), wall1,
      8);

  const NetworkLinkStatSummary summary = atm::summarizeNetworkLinkStats(next, 8);
  CHECK(summary.sample_count == 2);
  CHECK(summary.valid_sample_count == 2);
  CHECK(near(summary.coverage, 2.0 / 8.0));
  CHECK(near(summary.span_seconds, 10.0));
  CHECK(summary.last_update == wall1);
  CHECK(*summary.window_delta[kRxErrors] == 600);
  CHECK(*summary.window_delta[kRxDropped] == 2);
  CHECK(*summary.window_delta[kTxDropped] == 2);
  CHECK(*summary.window_delta[kCollisions] == 2);
  CHECK(*summary.window_delta[kTxErrors] == 0);
  CHECK(near(*summary.rates.rx_errors_per_second, 60.0));
}

void test_summarize_stale_and_reset() {
  run("SummarizeStaleAndReset");
  const Steady::time_point t0 = Steady::now();
  const System::time_point wall0 = System::now();

  NetworkLinkStats empty;
  empty.identity = "idx:2";
  const NetworkLinkStats base =
      atm::updateNetworkLinkStats(empty, makeRead(1000, 50, 10, 4, 5), t0,
                                  wall0, 8);
  const NetworkLinkStats reset = atm::updateNetworkLinkStats(
      base, makeRead(900, 55, 14, 4, 5), t0 + std::chrono::seconds(5), wall0,
      8);
  NetworkLinkStatRead denied;
  denied.read_ok = false;
  denied.access_denied = true;
  const NetworkLinkStats stale = atm::updateNetworkLinkStats(
      reset, denied, t0 + std::chrono::seconds(6), wall0, 8);

  const NetworkLinkStatSummary summary = atm::summarizeNetworkLinkStats(stale, 8);
  CHECK(summary.sample_count == 3);
  CHECK(summary.valid_sample_count == 2);
  CHECK(summary.stale_sample_count == 1);
  CHECK(summary.discontinuity_count == 1);
  CHECK(!summary.window_delta[kRxErrors].has_value());  // reset inside window
  CHECK(*summary.window_delta[kRxDropped] == 4);        // unchecked metric
  // Current counters are carried across the stale tick gap-free.
  CHECK(*summary.current[kRxErrors] == 900);
  CHECK(!summary.rates.rx_errors_per_second.has_value());
}

// -------------------------------------------------------------------------
// formatNetworkLinkStatRate
// -------------------------------------------------------------------------

void test_format_rate() {
  run("FormatRate");
  const std::optional<double> none;
  CHECK(atm::formatNetworkLinkStatRate(none) == "unavailable");
  CHECK(atm::formatNetworkLinkStatRate(0.0) == "0/s");
  CHECK(atm::formatNetworkLinkStatRate(123.456) == "123/s");
  CHECK(atm::formatNetworkLinkStatRate(12.34) == "12.3/s");
  CHECK(atm::formatNetworkLinkStatRate(1.0) == "1/s");
  CHECK(atm::formatNetworkLinkStatRate(0.5) == "0.5/s");
  CHECK(atm::formatNetworkLinkStatRate(0.05) == "0.05/s");
  CHECK(atm::formatNetworkLinkStatRate(1000000.0) == "1000000/s");
  CHECK(atm::formatNetworkLinkStatRate(-4.0) == "unavailable");
  CHECK(atm::formatNetworkLinkStatRate(
            std::numeric_limits<double>::quiet_NaN()) == "unavailable");
  CHECK(atm::formatNetworkLinkStatRate(
            std::numeric_limits<double>::infinity()) == "unavailable");
}

// -------------------------------------------------------------------------
// renderNetworkLinkStats
// -------------------------------------------------------------------------

void test_render_stats_block() {
  run("RenderStatsBlock");
  const Steady::time_point t0 = Steady::now();
  const System::time_point wall0 = System::now();

  NetworkLinkStats empty;
  empty.identity = "idx:2";
  const NetworkLinkStats base =
      atm::updateNetworkLinkStats(empty, makeRead(1000, 50, 10, 4, 5), t0,
                                  wall0, 8);
  const NetworkLinkStats next = atm::updateNetworkLinkStats(
      base, makeRead(1600, 50, 10, 4, 7), t0 + std::chrono::seconds(10), wall0,
      8);

  const std::string text = atm::renderNetworkLinkStats(next, 8);
  CHECK(text.find("State: valid") != std::string::npos);
  CHECK(text.find("Coverage: 2/8") != std::string::npos);
  CHECK(text.find("Discontinuities (counter resets): 0") != std::string::npos);
  CHECK(text.find("Cumulative counters") != std::string::npos);
  CHECK(text.find("rx_errors") != std::string::npos);
  CHECK(text.find("1600") != std::string::npos);
  CHECK(text.find("Current interval rates") != std::string::npos);
  CHECK(text.find("60/s") != std::string::npos);
  CHECK(text.find("drops (rx + tx)") != std::string::npos);
  CHECK(text.find("Retained-window deltas") != std::string::npos);
  CHECK(text.find("unavailable") != std::string::npos);  // rx_missed_errors
}

void test_render_stats_empty() {
  run("RenderStatsEmpty");
  NetworkLinkStats empty;
  empty.identity = "idx:2";
  const std::string text = atm::renderNetworkLinkStats(empty, 8);
  CHECK(text.find("State: unknown") != std::string::npos);
  CHECK(text.find("Coverage: 0/8") != std::string::npos);
  CHECK(text.find("unavailable") != std::string::npos);
}

// -------------------------------------------------------------------------
// renderNetworkLinkStatTrend
// -------------------------------------------------------------------------

void test_render_trend() {
  run("RenderTrend");
  // Build a rising rate over the retained ring: symlink-free, timeline-only.
  NetworkLinkStats record;
  record.identity = "idx:2";
  const Steady::time_point t0 = Steady::now();
  const System::time_point wall = System::now();
  for (int i = 0; i < 8; ++i) {
    // Counters advance by 50 per second -> 50 events/s every tick.
    const std::uint64_t rx = 1000u + 50u * static_cast<std::uint64_t>(i + 1);
    record = atm::updateNetworkLinkStats(record, makeRead(rx, 0, 0, 0, 0),
                                         t0 + std::chrono::seconds(i + 1), wall,
                                         16);
  }

  atm::NetworkLinkStatChartConfig config;
  config.data_width = 20;
  config.data_height = 5;
  const std::string text = atm::renderNetworkLinkStatTrend(
      record, NetworkLinkStatMetric::RxErrors, config);
  CHECK(text.find("rx_errors") != std::string::npos);
  CHECK(text.find("~") != std::string::npos);  // peak glyphs
  CHECK(text.find(".") != std::string::npos);  // flow glyphs
  CHECK(text.find("Now") != std::string::npos);
  CHECK(text.find("+---") != std::string::npos);
}

void test_render_trend_no_data() {
  run("RenderTrendNoData");
  NetworkLinkStats empty;
  empty.identity = "idx:2";
  atm::NetworkLinkStatChartConfig config;
  const std::string text = atm::renderNetworkLinkStatTrend(
      empty, NetworkLinkStatMetric::RxErrors, config);
  CHECK(text.find("(no data)") != std::string::npos);

  // A metric with no derived rate never plots anything.
  NetworkLinkStats record;
  record.identity = "idx:2";
  const std::string no_rate = atm::renderNetworkLinkStatTrend(
      record, NetworkLinkStatMetric::RxCompressed, config);
  CHECK(no_rate.find("(no data)") != std::string::npos);
}

// -------------------------------------------------------------------------
// NetworkLinkStatsMonitor
// -------------------------------------------------------------------------

void test_monitor_new_identity_updates() {
  run("MonitorNewIdentityUpdates");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-nls-test-monitor";
  std::filesystem::remove_all(root);
  const std::filesystem::path stats = makeStatisticsDir(root, "eth0");
  writeFile(stats / "rx_errors", "37\n");
  writeFile(stats / "tx_errors", "2\n");

  NetworkLinkStatsMonitor monitor(root, 4);
  monitor.update(
      makeSnapshot({makeInfo("eth0", 2, NetworkInterfaceType::Ethernet)}));
  const NetworkLinkStats *eth0 = monitor.tracked("idx:2");
  CHECK(eth0 != nullptr);
  CHECK(eth0->name == "eth0");
  CHECK(eth0->state == NetworkLinkStatState::Valid);
  CHECK(eth0->history.size() == 1);
  CHECK(*eth0->history.samples().back().counters[kRxErrors] == 37);

  std::filesystem::remove_all(root);
}

void test_monitor_disappearance_preserved() {
  run("MonitorDisappearancePreserved");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-nls-test-gone";
  std::filesystem::remove_all(root);
  makeStatisticsDir(root, "eth0");

  NetworkLinkStatsMonitor monitor(root, 4);
  monitor.update(
      makeSnapshot({makeInfo("eth0", 2, NetworkInterfaceType::Ethernet)}));
  CHECK(monitor.tracked("idx:2")->present);

  monitor.update(makeSnapshot({}));
  CHECK(monitor.tracked("idx:2") != nullptr);
  CHECK(!monitor.tracked("idx:2")->present);
  CHECK(monitor.tracked("idx:2")->history.size() == 1);

  std::filesystem::remove_all(root);
}

void test_monitor_reappears() {
  run("MonitorReappears");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-nls-test-reappear";
  std::filesystem::remove_all(root);
  makeStatisticsDir(root, "eth0");

  NetworkLinkStatsMonitor monitor(root, 4);
  monitor.update(
      makeSnapshot({makeInfo("eth0", 2, NetworkInterfaceType::Ethernet)}));
  monitor.update(makeSnapshot({}));
  CHECK(!monitor.tracked("idx:2")->present);
  monitor.update(
      makeSnapshot({makeInfo("eth0", 2, NetworkInterfaceType::Ethernet)}));
  CHECK(monitor.tracked("idx:2")->present);
  CHECK(monitor.tracked("idx:2")->history.size() == 2);

  std::filesystem::remove_all(root);
}

void test_monitor_rename_keeps_identity() {
  run("MonitorRenameKeepsIdentity");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-nls-test-rename";
  std::filesystem::remove_all(root);
  makeStatisticsDir(root, "eth0");
  makeStatisticsDir(root, "eth1");

  NetworkLinkStatsMonitor monitor(root, 4);
  monitor.update(
      makeSnapshot({makeInfo("eth0", 2, NetworkInterfaceType::Ethernet)}));
  monitor.update(
      makeSnapshot({makeInfo("eth1", 2, NetworkInterfaceType::Ethernet)}));
  CHECK(monitor.tracked("idx:2") != nullptr);
  CHECK(monitor.tracked("idx:2")->name == "eth1");
  CHECK(monitor.entries().size() == 1);  // same identity, not duplicated
  CHECK(monitor.tracked("idx:2")->history.size() == 2);

  std::filesystem::remove_all(root);
}

void test_monitor_recreation_starts_fresh() {
  run("MonitorRecreationStartsFresh");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-nls-test-recreate";
  std::filesystem::remove_all(root);
  makeStatisticsDir(root, "eth0");

  NetworkLinkStatsMonitor monitor(root, 4);
  monitor.update(
      makeSnapshot({makeInfo("eth0", 2, NetworkInterfaceType::Ethernet)}));
  CHECK(monitor.tracked("idx:2")->history.size() == 1);

  monitor.update(
      makeSnapshot({makeInfo("eth0", 7, NetworkInterfaceType::Ethernet)}));
  const NetworkLinkStats *old = monitor.tracked("idx:2");
  const NetworkLinkStats *fresh = monitor.tracked("idx:7");
  CHECK(old != nullptr && !old->present);
  CHECK(fresh != nullptr && fresh->present);
  CHECK(fresh->history.size() == 1);  // fresh record, no inherited history

  std::filesystem::remove_all(root);
}

void test_monitor_eviction_bound() {
  run("MonitorEvictionBound");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-nls-test-evict";
  std::filesystem::remove_all(root);

  NetworkLinkStatsMonitor monitor(root, 4);
  std::vector<NetworkInterfaceInfo> many;
  for (int i = 1; i <= 70; ++i) {
    makeStatisticsDir(root, "veth" + std::to_string(i));
    many.push_back(makeInfo("veth" + std::to_string(i), i,
                            NetworkInterfaceType::Virtual));
  }
  monitor.update(makeSnapshot(many));
  // All 70 are live: a live entry is never evicted, so all are retained.
  CHECK(monitor.entries().size() == 70);

  monitor.update(makeSnapshot({}));
  CHECK(monitor.entries().size() <=
        NetworkLinkStatsMonitor::kMaxTrackedLinkStatsInterfaces);
  for (const auto &kv : monitor.entries()) {
    CHECK(!kv.second.present);
  }

  std::filesystem::remove_all(root);
}

void test_monitor_pause_skips() {
  run("MonitorPauseSkips");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-nls-test-pause";
  std::filesystem::remove_all(root);
  makeStatisticsDir(root, "eth0");

  NetworkLinkStatsMonitor monitor(root, 4);
  monitor.setHistoryPaused(true);
  monitor.update(
      makeSnapshot({makeInfo("eth0", 2, NetworkInterfaceType::Ethernet)}));
  CHECK(monitor.entries().empty());  // reads and samples skipped entirely

  monitor.setHistoryPaused(false);
  monitor.update(
      makeSnapshot({makeInfo("eth0", 2, NetworkInterfaceType::Ethernet)}));
  CHECK(monitor.tracked("idx:2") != nullptr);
  CHECK(monitor.tracked("idx:2")->history.size() == 1);

  std::filesystem::remove_all(root);
}

void test_monitor_max_samples_applied() {
  run("MonitorMaxSamplesApplied");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-nls-test-maxsamples";
  std::filesystem::remove_all(root);
  makeStatisticsDir(root, "eth0");

  NetworkLinkStatsMonitor monitor(root, 3);
  monitor.setHistoryMaxSamples(3);
  for (int i = 0; i < 5; ++i) {
    monitor.update(
        makeSnapshot({makeInfo("eth0", 2, NetworkInterfaceType::Ethernet)}));
  }
  CHECK(monitor.tracked("idx:2")->history.size() <= 3);

  monitor.setHistoryMaxSamples(2);
  monitor.update(
      makeSnapshot({makeInfo("eth0", 2, NetworkInterfaceType::Ethernet)}));
  CHECK(monitor.tracked("idx:2")->history.size() <= 2);

  std::filesystem::remove_all(root);
}

void test_monitor_reset() {
  run("MonitorReset");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-nls-test-reset";
  std::filesystem::remove_all(root);
  makeStatisticsDir(root, "eth0");

  NetworkLinkStatsMonitor monitor(root, 4);
  monitor.update(
      makeSnapshot({makeInfo("eth0", 2, NetworkInterfaceType::Ethernet)}));
  CHECK(monitor.entries().size() == 1);
  monitor.reset();
  CHECK(monitor.entries().empty());
  CHECK(monitor.tracked("idx:2") == nullptr);

  std::filesystem::remove_all(root);
}

int main() {
  test_metric_name_table();
  test_state_names();

  test_read_full_statistics_tree();
  test_read_partial_counters();
  test_read_malformed_counters();
  test_read_large_counter();
  test_read_missing_statistics_dir();
  test_read_denied_statistics_dir();

  test_derive_rates();
  test_derive_missing_and_decreased();
  test_derive_invalid_windows();

  test_update_first_sample_baseline();
  test_update_rate_window();
  test_update_stale_tick_preserves();
  test_update_clean_absence_unavailable();
  test_update_readable_but_empty_unavailable();
  test_update_reset_discontinuity();
  test_update_long_gap_no_false_rate();
  test_update_ring_bounded();

  test_summarize_baseline();
  test_summarize_window();
  test_summarize_stale_and_reset();

  test_format_rate();

  test_render_stats_block();
  test_render_stats_empty();
  test_render_trend();
  test_render_trend_no_data();

  test_monitor_new_identity_updates();
  test_monitor_disappearance_preserved();
  test_monitor_reappears();
  test_monitor_rename_keeps_identity();
  test_monitor_recreation_starts_fresh();
  test_monitor_eviction_bound();
  test_monitor_pause_skips();
  test_monitor_max_samples_applied();
  test_monitor_reset();

  std::fprintf(stderr, "\n%zu checks, %d failures\n", g_checks + 0UL,
               g_failures);
  if (g_failures != 0) {
    std::fprintf(stderr, "RESULT: FAIL\n");
    return 1;
  }
  std::fprintf(stderr, "RESULT: PASS\n");
  return 0;
}