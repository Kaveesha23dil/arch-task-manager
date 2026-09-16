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

#include "network_packet_stats.hpp"

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
using atm::NetworkPacketMtuChangeEvent;
using atm::NetworkPacketMtuState;
using atm::NetworkPacketMtuRead;
using atm::NetworkPacketMtuSample;
using atm::networkPacketMtuStateName;
using atm::NetworkPacketMtuStats;
using atm::NetworkPacketMtuSummary;
using atm::NetworkPacketMtuRates;
using atm::NetworkPacketMtuMonitor;
using atm::NetworkPacketSizeEstimate;

using Steady = std::chrono::steady_clock;
using System = std::chrono::system_clock;

namespace {

bool near(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) < eps;
}

void writeFile(const std::filesystem::path &path,
               const std::string &content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << content;
}

/// Builds `<root>/sys/class/net/<name>` (without the mtu file) and returns the
/// per-interface directory.
std::filesystem::path makeInterfaceDir(const std::filesystem::path &root,
                                       const std::string &name) {
  const std::filesystem::path dir =
      root / "sys" / "class" / "net" / name;
  std::filesystem::create_directories(dir);
  return dir;
}

NetworkInterfaceInfo makeInfo(const std::string &name, int ifindex) {
  NetworkInterfaceInfo info;
  info.name = name;
  info.type = NetworkInterfaceType::Ethernet;
  info.link.ifindex = ifindex;
  info.refreshed_at = System::now();
  return info;
}

NetworkInterfaceInfo makeInfoWithTraffic(const std::string &name, int ifindex,
                                         std::uint64_t rx_packets,
                                         std::uint64_t tx_packets,
                                         std::uint64_t rx_bytes,
                                         std::uint64_t tx_bytes) {
  NetworkInterfaceInfo info = makeInfo(name, ifindex);
  atm::NetworkInterfaceStats traffic;
  traffic.name = name;
  traffic.rx_packets = rx_packets;
  traffic.tx_packets = tx_packets;
  traffic.rx_bytes = rx_bytes;
  traffic.tx_bytes = tx_bytes;
  info.traffic = traffic;
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

/// A complete valid read (mtu + all four counters) expressed directly.
atm::NetworkPacketMtuRead makeRead(int mtu, std::uint64_t rx_packets,
                                   std::uint64_t tx_packets,
                                   std::uint64_t rx_bytes,
                                   std::uint64_t tx_bytes) {
  atm::NetworkPacketMtuRead read;
  read.mtu_read_ok = true;
  read.counters_available = true;
  read.mtu = mtu;
  read.rx_packets = rx_packets;
  read.tx_packets = tx_packets;
  read.rx_bytes = rx_bytes;
  read.tx_bytes = tx_bytes;
  return read;
}

}  // namespace

// -------------------------------------------------------------------------
// Size class / state names
// -------------------------------------------------------------------------

void test_size_class_names() {
  run("SizeClassNames");
  CHECK(std::string(atm::networkPacketMtuSizeClassName(std::nullopt)) ==
        "unknown");
  CHECK(std::string(atm::networkPacketMtuSizeClassName(1500)) == "standard");
  CHECK(std::string(atm::networkPacketMtuSizeClassName(1492)) ==
        "below_standard");
  CHECK(std::string(atm::networkPacketMtuSizeClassName(9000)) ==
        "above_standard");
  CHECK(std::string(atm::networkPacketMtuSizeClassName(0)) == "below_standard");
}

void test_state_names() {
  run("StateNames");
  CHECK(std::string(atm::networkPacketMtuStateName(
            NetworkPacketMtuState::Unknown)) == "unknown");
  CHECK(std::string(atm::networkPacketMtuStateName(
            NetworkPacketMtuState::Valid)) == "valid");
  CHECK(std::string(atm::networkPacketMtuStateName(
            NetworkPacketMtuState::Stale)) == "stale");
  CHECK(std::string(atm::networkPacketMtuStateName(
            NetworkPacketMtuState::Unavailable)) == "unavailable");
}

// -------------------------------------------------------------------------
// readNetworkInterfaceMtu: hermetically built sysfs trees
// -------------------------------------------------------------------------

void test_read_valid_mtu() {
  run("ReadValidMtu");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-npm-test-valid";
  std::filesystem::remove_all(root);
  const std::filesystem::path dir = makeInterfaceDir(root, "eth0");
  writeFile(dir / "mtu", "1500\n");

  const atm::NetworkPacketMtuRead read = atm::readNetworkInterfaceMtu(root, "eth0");
  CHECK(read.mtu_read_ok);
  CHECK(!read.mtu_access_denied);
  CHECK(read.mtu.has_value());
  CHECK(*read.mtu == 1500);
  CHECK(!read.counters_available);  // counters are merged from the snapshot
  std::filesystem::remove_all(root);
}

void test_read_malformed_mtu() {
  run("ReadMalformedMtu");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-npm-test-malformed";
  std::filesystem::remove_all(root);
  const std::filesystem::path dir = makeInterfaceDir(root, "eth0");
  writeFile(dir / "mtu", "lots\n");
  writeFile(makeInterfaceDir(root, "eth1") / "mtu", "1500.5\n");
  writeFile(makeInterfaceDir(root, "eth2") / "mtu", "-1\n");
  writeFile(makeInterfaceDir(root, "eth3") / "mtu", " 1500 \n");
  writeFile(makeInterfaceDir(root, "eth4") / "mtu", "2147483647\n");
  writeFile(makeInterfaceDir(root, "eth5") / "mtu", "2147483648\n");

  const auto read = atm::readNetworkInterfaceMtu(root, "eth0");
  CHECK(read.mtu_read_ok);
  CHECK(!read.mtu.has_value());
  const auto read1 = atm::readNetworkInterfaceMtu(root, "eth1");
  CHECK(read1.mtu_read_ok);
  CHECK(!read1.mtu.has_value());
  const auto read2 = atm::readNetworkInterfaceMtu(root, "eth2");
  CHECK(read2.mtu_read_ok);
  CHECK(!read2.mtu.has_value());  // "-1" is the kernel no-value sentinel
  const auto read3 = atm::readNetworkInterfaceMtu(root, "eth3");
  CHECK(*read3.mtu == 1500);  // whitespace trimmed
  const auto read4 = atm::readNetworkInterfaceMtu(root, "eth4");
  CHECK(*read4.mtu == 2147483647);
  const auto read5 = atm::readNetworkInterfaceMtu(root, "eth5");
  CHECK(read5.mtu_read_ok);
  CHECK(!read5.mtu.has_value());  // overflow of the int range
  std::filesystem::remove_all(root);
}

void test_read_missing_mtu_file() {
  run("ReadMissingMtuFile");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-npm-test-missing";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "sys" / "class" / "net");

  const atm::NetworkPacketMtuRead read =
      atm::readNetworkInterfaceMtu(root, "no-such-iface");
  CHECK(!read.mtu_read_ok);
  CHECK(!read.mtu_access_denied);
  CHECK(!read.mtu.has_value());
  std::filesystem::remove_all(root);
}

void test_read_empty_mtu_file() {
  run("ReadEmptyMtuFile");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-npm-test-empty";
  std::filesystem::remove_all(root);
  const std::filesystem::path dir = makeInterfaceDir(root, "eth0");
  writeFile(dir / "mtu", "");

  const atm::NetworkPacketMtuRead read = atm::readNetworkInterfaceMtu(root, "eth0");
  CHECK(read.mtu_read_ok);  // the file exists (clean "no value")
  CHECK(!read.mtu_access_denied);
  CHECK(!read.mtu.has_value());
  std::filesystem::remove_all(root);
}

void test_read_denied_mtu_file() {
  run("ReadDeniedMtuFile");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-npm-test-denied";
  std::filesystem::remove_all(root);
  const std::filesystem::path file =
      makeInterfaceDir(root, "eth0") / "mtu";
  writeFile(file, "1500\n");
  const bool not_root = ::getuid() != 0;
  if (not_root) {
    std::filesystem::permissions(
        file, std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace);
  }
  const atm::NetworkPacketMtuRead read = atm::readNetworkInterfaceMtu(root, "eth0");
  if (not_root) {
    CHECK(read.mtu_access_denied);
    CHECK(!read.mtu_read_ok);
    CHECK(!read.mtu.has_value());
  } else {
    CHECK(read.mtu_read_ok);  // root bypasses permission checks
    CHECK(*read.mtu == 1500);
  }
  if (not_root) {
    std::filesystem::permissions(file, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::add);
  }
  std::filesystem::remove_all(root);
}

// -------------------------------------------------------------------------
// deriveNetworkPacketMtuRates / deriveNetworkPacketSizeEstimate
// -------------------------------------------------------------------------

void test_derive_rates() {
  run("DeriveRates");
  atm::NetworkPacketMtuSample previous;
  previous.counters_available = true;
  previous.rx_packets = 1000;
  previous.tx_packets = 2000;
  previous.rx_bytes = 1600000;
  previous.tx_bytes = 3000000;
  atm::NetworkPacketMtuSample current;
  current.counters_available = true;
  current.rx_packets = 1120;
  current.tx_packets = 2100;
  current.rx_bytes = 1793600;
  current.tx_bytes = 3140000;

  const atm::NetworkPacketMtuRates rates =
      atm::deriveNetworkPacketMtuRates(previous, current, 2.0);
  CHECK(rates.rx_packets_per_second.has_value());
  CHECK(near(*rates.rx_packets_per_second, 60.0));  // 120 packets / 2s
  CHECK(rates.tx_packets_per_second.has_value());
  CHECK(near(*rates.tx_packets_per_second, 50.0));  // 100 packets / 2s
  CHECK(rates.combined_packets_per_second.has_value());
  CHECK(near(*rates.combined_packets_per_second, 110.0));
  CHECK(rates.rx_bytes_per_second.has_value());
  CHECK(near(*rates.rx_bytes_per_second, 96800.0));  // 193600 bytes / 2s
  CHECK(rates.tx_bytes_per_second.has_value());
  CHECK(near(*rates.tx_bytes_per_second, 70000.0));
  CHECK(rates.combined_bytes_per_second.has_value());
  CHECK(near(*rates.combined_bytes_per_second, 166800.0));
}

void test_derive_missing_and_decreased() {
  run("DeriveMissingAndDecreased");
  atm::NetworkPacketMtuSample previous;
  previous.counters_available = true;
  previous.rx_packets = 100;
  previous.rx_bytes = 150000;
  atm::NetworkPacketMtuSample current;
  current.counters_available = true;
  current.rx_packets = 80;  // decreased: driver reset
  current.rx_bytes = 120000;

  const atm::NetworkPacketMtuRates rates =
      atm::deriveNetworkPacketMtuRates(previous, current, 1.0);
  CHECK(!rates.rx_packets_per_second.has_value());
  CHECK(!rates.rx_bytes_per_second.has_value());
  CHECK(!rates.combined_packets_per_second.has_value());
  CHECK(!rates.combined_bytes_per_second.has_value());
}

void test_derive_invalid_windows() {
  run("DeriveInvalidWindows");
  atm::NetworkPacketMtuSample a;
  a.counters_available = true;
  a.rx_packets = 10;
  atm::NetworkPacketMtuSample b;
  b.counters_available = true;
  b.rx_packets = 20;

  atm::NetworkPacketMtuSample no_counters;
  no_counters.counters_available = false;
  no_counters.rx_packets = 50;
  const auto no_baseline = atm::deriveNetworkPacketMtuRates(no_counters, b, 1.0);
  CHECK(!no_baseline.rx_packets_per_second.has_value());
  const auto zero_seconds = atm::deriveNetworkPacketMtuRates(a, b, 0.0);
  CHECK(!zero_seconds.rx_packets_per_second.has_value());
  const auto negative = atm::deriveNetworkPacketMtuRates(a, b, -1.0);
  CHECK(!negative.rx_packets_per_second.has_value());
}

void test_derive_size_estimates() {
  run("DeriveSizeEstimates");
  atm::NetworkPacketMtuSample previous;
  previous.counters_available = true;
  previous.rx_packets = 1000;
  previous.tx_packets = 2000;
  previous.rx_bytes = 1500000;
  previous.tx_bytes = 3000000;
  atm::NetworkPacketMtuSample current;
  current.counters_available = true;
  current.rx_packets = 2000;
  current.tx_packets = 3000;
  current.rx_bytes = 3040000;
  current.tx_bytes = 4550000;

  const atm::NetworkPacketSizeEstimate estimate =
      atm::deriveNetworkPacketSizeEstimate(previous, current);
  CHECK(estimate.rx_bytes_per_frame.has_value());
  CHECK(near(*estimate.rx_bytes_per_frame, 1540.0));  // 1540000 / 1000
  CHECK(estimate.tx_bytes_per_frame.has_value());
  CHECK(near(*estimate.tx_bytes_per_frame, 1550.0));  // 1550000 / 1000
  CHECK(estimate.combined_bytes_per_frame.has_value());
  CHECK(near(*estimate.combined_bytes_per_frame, 1545.0));  // 3090000 / 2000

  // A window with bytes but no frames yields no estimate (not a fake ratio).
  atm::NetworkPacketMtuSample static_packets = previous;
  static_packets.rx_packets = 1000;  // no rx packet delta
  static_packets.tx_packets = 2000;  // no tx packet delta
  const atm::NetworkPacketSizeEstimate no_frame_delta =
      atm::deriveNetworkPacketSizeEstimate(previous, static_packets);
  CHECK(!no_frame_delta.rx_bytes_per_frame.has_value());
  CHECK(!no_frame_delta.tx_bytes_per_frame.has_value());
  CHECK(!no_frame_delta.combined_bytes_per_frame.has_value());

  // A decreased byte counter yields no estimate for that direction.
  atm::NetworkPacketMtuSample decreased_bytes = current;
  decreased_bytes.rx_bytes = 1000;
  const atm::NetworkPacketSizeEstimate reset =
      atm::deriveNetworkPacketSizeEstimate(previous, decreased_bytes);
  CHECK(!reset.rx_bytes_per_frame.has_value());
  CHECK(!reset.combined_bytes_per_frame.has_value());
  CHECK(reset.tx_bytes_per_frame.has_value());
}

// -------------------------------------------------------------------------
// updateNetworkPacketMtu
// -------------------------------------------------------------------------

void test_update_first_sample_baseline() {
  run("UpdateFirstSampleBaseline");
  atm::NetworkPacketMtuStats stats;
  stats.identity = "idx:2";
  const Steady::time_point t0 = Steady::now();
  const System::time_point w0 = System::now();
  stats = atm::updateNetworkPacketMtu(stats, makeRead(1500, 100, 200, 150000,
                                                      300000),
                                      t0, w0, 120);
  CHECK(stats.state == NetworkPacketMtuState::Valid);
  CHECK(stats.history.samples().size() == 1);
  const atm::NetworkPacketMtuSample &sample = stats.history.samples().back();
  CHECK(sample.valid);
  CHECK(sample.mtu_available);
  CHECK(sample.counters_available);
  CHECK(sample.mtu.has_value() && *sample.mtu == 1500);
  CHECK(!sample.mtu_changed);  // first reading establishes the baseline
  CHECK(!sample.previous_mtu.has_value());
  CHECK(!sample.rates.rx_packets_per_second.has_value());
  CHECK(!sample.estimate.rx_bytes_per_frame.has_value());
  CHECK(stats.mtu_change_count == 0);
  CHECK(stats.mtu_events.empty());
  CHECK(stats.last_update == w0);
}

void test_update_rate_window() {
  run("UpdateRateWindow");
  atm::NetworkPacketMtuStats stats;
  stats.identity = "idx:2";
  const Steady::time_point t0 = Steady::now();
  const System::time_point w0 = System::now();
  stats = atm::updateNetworkPacketMtu(stats, makeRead(1500, 100, 200, 150000,
                                                      300000),
                                      t0, w0, 120);
  stats = atm::updateNetworkPacketMtu(stats, makeRead(1500, 160, 250, 240000,
                                                      370000),
                                      t0 + std::chrono::seconds(2),
                                      w0 + std::chrono::seconds(2), 120);
  const atm::NetworkPacketMtuSample &sample = stats.history.samples().back();
  CHECK(sample.valid);
  CHECK(sample.rates.rx_packets_per_second.has_value());
  CHECK(near(*sample.rates.rx_packets_per_second, 30.0));  // 60 / 2s
  CHECK(sample.rates.tx_packets_per_second.has_value());
  CHECK(near(*sample.rates.tx_packets_per_second, 25.0));
  CHECK(sample.rates.rx_bytes_per_second.has_value());
  CHECK(near(*sample.rates.rx_bytes_per_second, 45000.0));
  CHECK(sample.rates.combined_packets_per_second.has_value());
  CHECK(near(*sample.rates.combined_packets_per_second, 55.0));
  CHECK(sample.estimate.combined_bytes_per_frame.has_value());
  CHECK(near(*sample.estimate.combined_bytes_per_frame,
             160000.0 / 110.0));
  CHECK(sample.counter_discontinuity_count == 0);
  CHECK(stats.discontinuity_count == 0);
}

void test_update_stale_tick_preserves() {
  run("UpdateStaleTickPreserves");
  atm::NetworkPacketMtuStats stats;
  stats.identity = "idx:2";
  const Steady::time_point t0 = Steady::now();
  const System::time_point w0 = System::now();
  stats = atm::updateNetworkPacketMtu(stats, makeRead(1500, 100, 200, 150000,
                                                      300000),
                                      t0, w0, 120);

  // A tick with neither an mtu reading nor fresh counters.
  atm::NetworkPacketMtuRead stale;
  stale.mtu_read_ok = false;  // the file vanished (temporary failure)
  stats = atm::updateNetworkPacketMtu(stats, stale,
                                      t0 + std::chrono::seconds(1),
                                      w0 + std::chrono::seconds(1), 120);
  CHECK(stats.state == NetworkPacketMtuState::Unavailable);
  const atm::NetworkPacketMtuSample &sample = stats.history.samples().back();
  CHECK(!sample.valid);
  CHECK(!sample.mtu_available);
  CHECK(!sample.counters_available);
  CHECK(sample.mtu.has_value() && *sample.mtu == 1500);  // preserved
  CHECK(sample.rx_packets.has_value() && *sample.rx_packets == 100);
  CHECK(sample.tx_bytes.has_value() && *sample.tx_bytes == 300000);
  CHECK(!sample.rates.rx_packets_per_second.has_value());  // no fresh rate
  CHECK(!sample.mtu_changed);
}

void test_update_access_denied_stale() {
  run("UpdateAccessDeniedStale");
  atm::NetworkPacketMtuStats stats;
  stats.identity = "idx:2";
  const Steady::time_point t0 = Steady::now();
  const System::time_point w0 = System::now();
  stats = atm::updateNetworkPacketMtu(stats, makeRead(1500, 100, 200, 150000,
                                                      300000),
                                      t0, w0, 120);
  atm::NetworkPacketMtuRead denied;
  denied.mtu_access_denied = true;
  denied.mtu_read_ok = false;
  denied.counters_available = false;
  stats = atm::updateNetworkPacketMtu(stats, denied,
                                      t0 + std::chrono::seconds(1),
                                      w0 + std::chrono::seconds(1), 120);
  CHECK(stats.state == NetworkPacketMtuState::Stale);
  CHECK(stats.history.samples().back().mtu.has_value());
  CHECK(*stats.history.samples().back().mtu == 1500);  // preserved
}

void test_update_mtu_change_detection() {
  run("UpdateMtuChangeDetection");
  atm::NetworkPacketMtuStats stats;
  stats.identity = "idx:2";
  const Steady::time_point t0 = Steady::now();
  const System::time_point w0 = System::now();
  stats = atm::updateNetworkPacketMtu(stats, makeRead(1500, 100, 200, 150000,
                                                      300000),
                                      t0, w0, 120);
  stats = atm::updateNetworkPacketMtu(stats, makeRead(9000, 160, 250, 240000,
                                                      370000),
                                      t0 + std::chrono::seconds(1),
                                      w0 + std::chrono::seconds(1), 120);
  CHECK(stats.mtu_change_count == 1);
  CHECK(stats.mtu_events.size() == 1);
  const NetworkPacketMtuChangeEvent &event = stats.mtu_events.front();
  CHECK(event.previous_mtu.has_value() && *event.previous_mtu == 1500);
  CHECK(event.new_mtu.has_value() && *event.new_mtu == 9000);
  CHECK(event.wall_clock == w0 + std::chrono::seconds(1));
  const atm::NetworkPacketMtuSample &changed = stats.history.samples().back();
  CHECK(changed.mtu_changed);
  CHECK(changed.previous_mtu.has_value() && *changed.previous_mtu == 1500);
  CHECK(changed.mtu.has_value() && *changed.mtu == 9000);

  // An identical value produces no second event.
  stats = atm::updateNetworkPacketMtu(stats, makeRead(9000, 200, 300, 300000,
                                                      500000),
                                      t0 + std::chrono::seconds(2),
                                      w0 + std::chrono::seconds(2), 120);
  CHECK(stats.mtu_change_count == 1);
  CHECK(stats.mtu_events.size() == 1);
}

void test_update_mtu_change_after_gap() {
  run("UpdateMtuChangeAfterGap");
  atm::NetworkPacketMtuStats stats;
  stats.identity = "idx:2";
  const Steady::time_point t0 = Steady::now();
  const System::time_point w0 = System::now();
  stats = atm::updateNetworkPacketMtu(stats, makeRead(1500, 100, 200, 150000,
                                                      300000),
                                      t0, w0, 120);
  atm::NetworkPacketMtuRead stale;
  stats = atm::updateNetworkPacketMtu(stats, stale,
                                      t0 + std::chrono::seconds(1),
                                      w0 + std::chrono::seconds(1), 120);
  // The change is detected against the preserved (still displayed) value.
  stats = atm::updateNetworkPacketMtu(stats, makeRead(1400, 160, 250, 240000,
                                                      370000),
                                      t0 + std::chrono::seconds(2),
                                      w0 + std::chrono::seconds(2), 120);
  CHECK(stats.mtu_change_count == 1);
  CHECK(stats.mtu_events.size() == 1);
  CHECK(*stats.mtu_events.front().previous_mtu == 1500);
}

void test_update_reset_discontinuity() {
  run("UpdateResetDiscontinuity");
  atm::NetworkPacketMtuStats stats;
  stats.identity = "idx:2";
  const Steady::time_point t0 = Steady::now();
  const System::time_point w0 = System::now();
  stats = atm::updateNetworkPacketMtu(stats, makeRead(1500, 100, 200, 150000,
                                                      300000),
                                      t0, w0, 120);
  // rx_packets decreased: one discontinuity, rates for rx unavailable.
  stats = atm::updateNetworkPacketMtu(stats, makeRead(1500, 90, 250, 240000,
                                                      370000),
                                      t0 + std::chrono::seconds(1),
                                      w0 + std::chrono::seconds(1), 120);
  CHECK(stats.discontinuity_count == 1);
  const atm::NetworkPacketMtuSample &sample = stats.history.samples().back();
  CHECK(sample.counter_discontinuity_count == 1);
  CHECK(!sample.rates.rx_packets_per_second.has_value());
  CHECK(sample.rates.tx_packets_per_second.has_value());
  CHECK(!sample.estimate.rx_bytes_per_frame.has_value());
  CHECK(sample.estimate.tx_bytes_per_frame.has_value());
  CHECK(!sample.mtu_changed);
}

void test_update_long_gap_honest_rate() {
  run("UpdateLongGapHonestRate");
  atm::NetworkPacketMtuStats stats;
  stats.identity = "idx:2";
  const Steady::time_point t0 = Steady::now();
  const System::time_point w0 = System::now();
  stats = atm::updateNetworkPacketMtu(stats, makeRead(1500, 100, 200, 150000,
                                                      300000),
                                      t0, w0, 120);
  // A 60-second quiet gap between two fresh counter readings: the rate is a
  // true average over the real elapsed time, never a per-tick fabrication.
  stats = atm::updateNetworkPacketMtu(stats, makeRead(1500, 160, 260, 210000,
                                                      360000),
                                      t0 + std::chrono::seconds(60),
                                      w0 + std::chrono::seconds(60), 120);
  const atm::NetworkPacketMtuSample &sample = stats.history.samples().back();
  CHECK(sample.rates.rx_packets_per_second.has_value());
  CHECK(near(*sample.rates.rx_packets_per_second, 1.0));   // 60 packets / 60s
  CHECK(near(*sample.rates.rx_bytes_per_second, 1000.0));  // 60 KB / 60s
  CHECK(near(*sample.rates.tx_packets_per_second, 1.0));
}

void test_update_ring_bounded() {
  run("UpdateRingBounded");
  atm::NetworkPacketMtuStats stats;
  stats.identity = "idx:2";
  const Steady::time_point t0 = Steady::now();
  const System::time_point w0 = System::now();
  for (int i = 0; i < 10; ++i) {
    stats = atm::updateNetworkPacketMtu(
        stats, makeRead(1500, 100 + i * 10, 200 + i * 10, 150000 + i * 1000,
                        300000 + i * 1000),
        t0 + std::chrono::seconds(i), w0 + std::chrono::seconds(i), 4);
  }
  CHECK(stats.history.samples().size() == 4);  // bounded under max_samples
  CHECK(stats.history.samples().front().timestamp ==
        t0 + std::chrono::seconds(6));
  CHECK(stats.history.samples().back().timestamp ==
        t0 + std::chrono::seconds(9));
}

void test_update_event_list_bounded() {
  run("UpdateEventListBounded");
  atm::NetworkPacketMtuStats stats;
  stats.identity = "idx:2";
  const Steady::time_point t0 = Steady::now();
  const System::time_point w0 = System::now();
  for (std::size_t i = 0; i < atm::kNetworkPacketMtuMaxEvents + 4; ++i) {
    stats = atm::updateNetworkPacketMtu(
        stats, makeRead(static_cast<int>(1500 + i), 100, 200, 150000, 300000),
        t0 + std::chrono::seconds(static_cast<long long>(i)),
        w0 + std::chrono::seconds(static_cast<long long>(i)), 120);
  }
  // The running counter is unbounded; only the retained timeline is capped.
  // The first of the 36 readings only establishes the baseline, so 35 changes.
  CHECK(stats.mtu_change_count == atm::kNetworkPacketMtuMaxEvents + 3);
  CHECK(stats.mtu_events.size() == atm::kNetworkPacketMtuMaxEvents);
}

// -------------------------------------------------------------------------
// summarizeNetworkPacketMtu
// -------------------------------------------------------------------------

void test_summarize_baseline() {
  run("SummarizeBaseline");
  atm::NetworkPacketMtuStats stats;
  stats.identity = "idx:2";
  stats.mtu_change_count = 3;
  stats.discontinuity_count = 2;
  const Steady::time_point t0 = Steady::now();
  const System::time_point w0 = System::now();
  stats = atm::updateNetworkPacketMtu(stats, makeRead(1500, 100, 200, 150000,
                                                      300000),
                                      t0, w0, 120);
  const atm::NetworkPacketMtuSummary summary =
      atm::summarizeNetworkPacketMtu(stats, 120);
  CHECK(summary.has_data);
  CHECK(summary.has_valid_data);
  CHECK(summary.has_mtu_data);
  CHECK(summary.has_counter_data);
  CHECK(summary.sample_count == 1);
  CHECK(summary.valid_sample_count == 1);
  CHECK(summary.stale_sample_count == 0);
  CHECK(summary.mtu_valid_count == 1);
  CHECK(summary.counter_valid_count == 1);
  CHECK(summary.mtu_change_count == 3);
  CHECK(summary.discontinuity_count == 2);
  CHECK(summary.current_mtu.has_value() && *summary.current_mtu == 1500);
  CHECK(!summary.previous_mtu.has_value());
  CHECK(summary.min_mtu.has_value() && *summary.min_mtu == 1500);
  CHECK(summary.max_mtu.has_value() && *summary.max_mtu == 1500);
  CHECK(summary.current_rx_packets.has_value() &&
        *summary.current_rx_packets == 100);
  CHECK(summary.current_tx_bytes.has_value() && *summary.current_tx_bytes == 300000);
  CHECK(!summary.window_rx_packets.has_value());  // needs two fresh samples
  CHECK(summary.last_update == w0);
}

void test_summarize_window() {
  run("SummarizeWindow");
  atm::NetworkPacketMtuStats stats;
  stats.identity = "idx:2";
  const Steady::time_point t0 = Steady::now();
  const System::time_point w0 = System::now();
  stats = atm::updateNetworkPacketMtu(stats, makeRead(9000, 100, 200, 150000,
                                                      300000),
                                      t0, w0, 120);
  stats = atm::updateNetworkPacketMtu(stats, makeRead(1500, 160, 250, 240000,
                                                      370000),
                                      t0 + std::chrono::seconds(1),
                                      w0 + std::chrono::seconds(1), 120);
  stats = atm::updateNetworkPacketMtu(stats, makeRead(1500, 210, 320, 320000,
                                                      470000),
                                      t0 + std::chrono::seconds(2),
                                      w0 + std::chrono::seconds(2), 120);
  const atm::NetworkPacketMtuSummary summary =
      atm::summarizeNetworkPacketMtu(stats, 120);
  CHECK(summary.sample_count == 3);
  CHECK(summary.valid_sample_count == 3);
  CHECK(summary.mtu_change_count == 1);
  CHECK(summary.current_mtu.has_value() && *summary.current_mtu == 1500);
  CHECK(summary.previous_mtu.has_value() && *summary.previous_mtu == 9000);
  CHECK(summary.min_mtu.has_value() && *summary.min_mtu == 1500);
  CHECK(summary.max_mtu.has_value() && *summary.max_mtu == 9000);
  // Window deltas are first vs last fresh-counter sample.
  CHECK(summary.window_rx_packets.has_value() && *summary.window_rx_packets == 110);
  CHECK(summary.window_tx_packets.has_value() && *summary.window_tx_packets == 120);
  CHECK(summary.window_rx_bytes.has_value() && *summary.window_rx_bytes == 170000);
  CHECK(summary.window_tx_bytes.has_value() && *summary.window_tx_bytes == 170000);
  CHECK(summary.window_estimate.rx_bytes_per_frame.has_value());
  CHECK(near(*summary.window_estimate.rx_bytes_per_frame, 170000.0 / 110.0));
  CHECK(summary.rates.rx_packets_per_second.has_value());
  CHECK(summary.estimate.combined_bytes_per_frame.has_value());
  CHECK(near(summary.coverage, 3.0 / 120.0));
}

void test_summarize_stale_and_unavailable() {
  run("SummarizeStaleAndUnavailable");
  atm::NetworkPacketMtuStats stats;
  stats.identity = "idx:2";
  const Steady::time_point t0 = Steady::now();
  const System::time_point w0 = System::now();
  stats = atm::updateNetworkPacketMtu(stats, makeRead(1500, 100, 200, 150000,
                                                      300000),
                                      t0, w0, 120);
  atm::NetworkPacketMtuRead stale;
  stats = atm::updateNetworkPacketMtu(stats, stale,
                                      t0 + std::chrono::seconds(1),
                                      w0 + std::chrono::seconds(1), 120);
  stats = atm::updateNetworkPacketMtu(stats, stale,
                                      t0 + std::chrono::seconds(2),
                                      w0 + std::chrono::seconds(2), 120);
  const atm::NetworkPacketMtuSummary summary =
      atm::summarizeNetworkPacketMtu(stats, 120);
  CHECK(summary.sample_count == 3);
  CHECK(summary.valid_sample_count == 1);
  CHECK(summary.stale_sample_count == 2);
  CHECK(summary.mtu_valid_count == 1);
  CHECK(summary.counter_valid_count == 1);
  CHECK(summary.has_valid_data);
  // The preserved MTU is still reported (gap-free display), never a zero.
  CHECK(summary.current_mtu.has_value() && *summary.current_mtu == 1500);
  CHECK(summary.current_rx_packets.has_value() && *summary.current_rx_packets == 100);
  CHECK(summary.last_update == w0);
}

void test_summarize_never_sampled() {
  run("SummarizeNeverSampled");
  atm::NetworkPacketMtuStats stats;
  stats.identity = "idx:2";
  const atm::NetworkPacketMtuSummary summary =
      atm::summarizeNetworkPacketMtu(stats, 120);
  CHECK(!summary.has_data);
  CHECK(!summary.has_valid_data);
  CHECK(!summary.has_mtu_data);
  CHECK(!summary.has_counter_data);
  CHECK(summary.sample_count == 0);
  CHECK(!summary.current_mtu.has_value());
  CHECK(!summary.min_mtu.has_value());
  CHECK(!summary.max_mtu.has_value());
  CHECK(!summary.rates.rx_packets_per_second.has_value());
}

void test_summarize_mtu_only() {
  run("SummarizeMtuOnly");
  atm::NetworkPacketMtuStats stats;
  stats.identity = "idx:2";
  const Steady::time_point t0 = Steady::now();
  const System::time_point w0 = System::now();
  atm::NetworkPacketMtuRead mtu_only;
  mtu_only.mtu_read_ok = true;
  mtu_only.mtu = 1492;
  stats = atm::updateNetworkPacketMtu(stats, mtu_only, t0, w0, 120);
  const atm::NetworkPacketMtuSummary summary =
      atm::summarizeNetworkPacketMtu(stats, 120);
  CHECK(summary.has_mtu_data);
  CHECK(!summary.has_counter_data);
  CHECK(summary.valid_sample_count == 1);  // fresh mtu counts as a valid tick
  CHECK(summary.current_mtu.has_value() && *summary.current_mtu == 1492);
  CHECK(!summary.current_rx_packets.has_value());
  CHECK(!summary.rates.rx_packets_per_second.has_value());
}

// -------------------------------------------------------------------------
// Formatting
// -------------------------------------------------------------------------

void test_format_rate() {
  run("FormatRate");
  CHECK(atm::formatNetworkPacketMtuRate(std::nullopt) == "unavailable");
  CHECK(atm::formatNetworkPacketMtuRate(-1.0) == "unavailable");
  CHECK(atm::formatNetworkPacketMtuRate(12.0) == "12/s");
  CHECK(atm::formatNetworkPacketMtuRate(12.34) == "12.3/s");
  CHECK(atm::formatNetworkPacketMtuRate(0.05) == "0.05/s");
  CHECK(atm::formatNetworkPacketMtuRate(120.0) == "120/s");
}

void test_format_throughput() {
  run("FormatThroughput");
  CHECK(atm::formatNetworkPacketMtuThroughput(std::nullopt) == "unavailable");
  CHECK(atm::formatNetworkPacketMtuThroughput(0.0) == "0 B/s");
  CHECK(atm::formatNetworkPacketMtuThroughput(512.0) == "512 B/s");
  CHECK(atm::formatNetworkPacketMtuThroughput(1500.0) == "1.5 KB/s");
  CHECK(atm::formatNetworkPacketMtuThroughput(2'500'000.0) == "2.5 MB/s");
  CHECK(atm::formatNetworkPacketMtuThroughput(1'000'000'000.0) == "1 GB/s");
}

void test_format_size_estimate() {
  run("FormatSizeEstimate");
  CHECK(atm::formatNetworkPacketSizeEstimate(std::nullopt) == "unavailable");
  CHECK(atm::formatNetworkPacketSizeEstimate(1514.0) == "1514 bytes/frame");
  CHECK(atm::formatNetworkPacketSizeEstimate(151.4) == "151.4 bytes/frame");
}

// -------------------------------------------------------------------------
// Render
// -------------------------------------------------------------------------

void test_render_block() {
  run("RenderBlock");
  atm::NetworkPacketMtuStats stats;
  stats.identity = "idx:2";
  const Steady::time_point t0 = Steady::now();
  const System::time_point w0 = System::now();
  stats = atm::updateNetworkPacketMtu(stats, makeRead(1500, 100, 200, 150000,
                                                      300000),
                                      t0, w0, 120);
  stats = atm::updateNetworkPacketMtu(stats, makeRead(9000, 160, 250, 240000,
                                                      370000),
                                      t0 + std::chrono::seconds(1),
                                      w0 + std::chrono::seconds(1), 120);
  const std::string large = atm::renderNetworkPacketMtu(stats, 120);
  CHECK(large.find("  Configured MTU: 9000 bytes") != std::string::npos);
  CHECK(large.find("(large configured MTU)") != std::string::npos);
  // A configured size is never asserted to be a verified jumbo capability.
  CHECK(large.find("not a verified jumbo-frame") != std::string::npos);
  CHECK(large.find("supports jumbo") == std::string::npos);
  CHECK(large.find("MTU changes recorded: 1") != std::string::npos);
  CHECK(large.find("  MTU changes from previous value: 9000 (was 1500)") !=
        std::string::npos);
  CHECK(large.find("Counter discontinuities (resets): 0") != std::string::npos);
  CHECK(large.find("rx packets") != std::string::npos);
  CHECK(large.find("tx bytes/s") != std::string::npos);
  CHECK(large.find("rx bytes/frame") != std::string::npos);
  CHECK(large.find("estimate only, no size distribution") != std::string::npos);

  // Returning to the standard Ethernet size is labelled honestly again.
  stats = atm::updateNetworkPacketMtu(stats, makeRead(1500, 220, 320, 320000,
                                                      470000),
                                      t0 + std::chrono::seconds(2),
                                      w0 + std::chrono::seconds(2), 120);
  const std::string standard = atm::renderNetworkPacketMtu(stats, 120);
  CHECK(standard.find("  Configured MTU: 1500 bytes (standard Ethernet MTU)") !=
        std::string::npos);
  CHECK(standard.find("MTU changes recorded: 2") != std::string::npos);
}

void test_render_block_unavailable() {
  run("RenderBlockUnavailable");
  atm::NetworkPacketMtuStats stats;
  stats.identity = "idx:2";
  const std::string text = atm::renderNetworkPacketMtu(stats, 120);
  CHECK(text.find("  State: unknown") != std::string::npos);
  CHECK(text.find("  Configured MTU: unavailable") != std::string::npos);
  CHECK(text.find("  Coverage: 0/120") != std::string::npos);
  CHECK(text.find("  MTU changes recorded: 0") != std::string::npos);
}

void test_render_changes() {
  run("RenderChanges");
  atm::NetworkPacketMtuStats stats;
  stats.identity = "idx:2";
  CHECK(atm::renderNetworkPacketMtuChanges(stats).find("no MTU changes recorded") !=
        std::string::npos);

  const Steady::time_point t0 = Steady::now();
  const System::time_point w0 = System::now();
  stats = atm::updateNetworkPacketMtu(stats, makeRead(1500, 100, 200, 150000,
                                                      300000),
                                      t0, w0, 120);
  stats = atm::updateNetworkPacketMtu(stats, makeRead(9000, 110, 200, 160000,
                                                      300000),
                                      t0 + std::chrono::seconds(1),
                                      w0 + std::chrono::seconds(1), 120);
  const std::string timeline = atm::renderNetworkPacketMtuChanges(stats);
  CHECK(timeline.find("MTU change timeline") != std::string::npos);
  CHECK(timeline.find("1500 -> 9000") != std::string::npos);
}

void test_render_trend() {
  run("RenderTrend");
  atm::NetworkPacketMtuStats stats;
  stats.identity = "idx:2";
  const Steady::time_point t0 = Steady::now();
  const System::time_point w0 = System::now();
  for (int i = 1; i <= 6; ++i) {
    stats = atm::updateNetworkPacketMtu(
        stats, makeRead(1500, 100 + i * 10, 200 + i * 10,
                        150000 + i * 1000, 300000 + i * 1000),
        t0 + std::chrono::seconds(i), w0 + std::chrono::seconds(i), 120);
  }
  atm::NetworkPacketMtuChartConfig config;
  config.data_width = 12;
  config.data_height = 5;
  const std::string rx =
      atm::renderNetworkPacketMtuTrend(stats,
                                       atm::NetworkPacketMtuTrendMetric::RxPacketRate,
                                       config);
  CHECK(rx.find("rx packets/s") != std::string::npos);
  CHECK(rx.find("|") != std::string::npos);
  CHECK(rx.find("Now") != std::string::npos);
  const std::string size =
      atm::renderNetworkPacketMtuTrend(stats,
                                       atm::NetworkPacketMtuTrendMetric::CombinedAvgFrameSize,
                                       config);
  CHECK(size.find("avg frame size") != std::string::npos);
}

void test_render_trend_no_data() {
  run("RenderTrendNoData");
  atm::NetworkPacketMtuStats stats;
  stats.identity = "idx:2";
  atm::NetworkPacketMtuChartConfig config;
  const std::string text =
      atm::renderNetworkPacketMtuTrend(stats,
                                       atm::NetworkPacketMtuTrendMetric::RxPacketRate,
                                       config);
  CHECK(text.find("(no data)") != std::string::npos);
}

// -------------------------------------------------------------------------
// Monitor
// -------------------------------------------------------------------------

void test_monitor_new_identity_updates() {
  run("MonitorNewIdentityUpdates");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-npm-monitor-new";
  std::filesystem::remove_all(root);
  writeFile(makeInterfaceDir(root, "eth0") / "mtu", "1500\n");
  writeFile(makeInterfaceDir(root, "wlan0") / "mtu", "1492\n");

  atm::NetworkPacketMtuMonitor monitor(root, 120);
  std::vector<NetworkInterfaceInfo> interfaces;
  interfaces.push_back(makeInfoWithTraffic("eth0", 2, 100, 200, 150000, 300000));
  interfaces.push_back(makeInfoWithTraffic("wlan0", 3, 50, 60, 80000, 90000));
  monitor.update(makeSnapshot(interfaces));

  const atm::NetworkPacketMtuStats *eth = monitor.tracked("idx:2");
  CHECK(eth != nullptr);
  CHECK(eth->name == "eth0");
  CHECK(eth->present);
  CHECK(eth->state == NetworkPacketMtuState::Valid);
  CHECK(!eth->history.samples().empty());
  CHECK(*eth->history.samples().back().mtu == 1500);
  CHECK(*eth->history.samples().back().rx_packets == 100);
  CHECK(*eth->history.samples().back().tx_bytes == 300000);
  const atm::NetworkPacketMtuStats *wlan = monitor.tracked("idx:3");
  CHECK(wlan != nullptr);
  CHECK(*wlan->history.samples().back().mtu == 1492);
  std::filesystem::remove_all(root);
}

void test_monitor_counters_absent() {
  run("MonitorCountersAbsent");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-npm-monitor-no-traffic";
  std::filesystem::remove_all(root);
  writeFile(makeInterfaceDir(root, "eth0") / "mtu", "1500\n");

  atm::NetworkPacketMtuMonitor monitor(root, 120);
  std::vector<NetworkInterfaceInfo> interfaces;
  interfaces.push_back(makeInfo("eth0", 2));  // no traffic -> /proc/net/dev absent
  monitor.update(makeSnapshot(interfaces));
  const atm::NetworkPacketMtuStats *eth = monitor.tracked("idx:2");
  CHECK(eth != nullptr);
  CHECK(eth->state == NetworkPacketMtuState::Valid);  // mtu fresh
  CHECK(eth->history.samples().back().mtu_available);
  CHECK(!eth->history.samples().back().counters_available);
  CHECK(!eth->history.samples().back().rx_packets.has_value());
  std::filesystem::remove_all(root);
}

void test_monitor_disappearance_preserved() {
  run("MonitorDisappearancePreserved");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-npm-monitor-gone";
  std::filesystem::remove_all(root);
  writeFile(makeInterfaceDir(root, "eth0") / "mtu", "1500\n");

  atm::NetworkPacketMtuMonitor monitor(root, 120);
  std::vector<NetworkInterfaceInfo> interfaces;
  interfaces.push_back(makeInfoWithTraffic("eth0", 2, 100, 200, 150000, 300000));
  monitor.update(makeSnapshot(interfaces));
  monitor.update(makeSnapshot({}));  // interface disappeared

  const atm::NetworkPacketMtuStats *eth = monitor.tracked("idx:2");
  CHECK(eth != nullptr);
  CHECK(!eth->present);
  CHECK(eth->name == "eth0");
  CHECK(*eth->history.samples().back().mtu == 1500);  // data retained
  std::filesystem::remove_all(root);
}

void test_monitor_reappears() {
  run("MonitorReappears");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-npm-monitor-reappear";
  std::filesystem::remove_all(root);
  writeFile(makeInterfaceDir(root, "eth0") / "mtu", "1500\n");

  atm::NetworkPacketMtuMonitor monitor(root, 120);
  std::vector<NetworkInterfaceInfo> interfaces;
  interfaces.push_back(makeInfoWithTraffic("eth0", 2, 100, 200, 150000, 300000));
  monitor.update(makeSnapshot(interfaces));
  monitor.update(makeSnapshot({}));
  CHECK(!monitor.tracked("idx:2")->present);
  monitor.update(makeSnapshot(interfaces));
  CHECK(monitor.tracked("idx:2")->present);
  CHECK(monitor.tracked("idx:2")->history.samples().size() == 2);
  std::filesystem::remove_all(root);
}

void test_monitor_rename_keeps_identity() {
  run("MonitorRenameKeepsIdentity");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-npm-monitor-rename";
  std::filesystem::remove_all(root);
  writeFile(makeInterfaceDir(root, "oldname") / "mtu", "1500\n");
  writeFile(makeInterfaceDir(root, "newname") / "mtu", "1500\n");

  atm::NetworkPacketMtuMonitor monitor(root, 120);
  std::vector<NetworkInterfaceInfo> interfaces;
  interfaces.push_back(makeInfoWithTraffic("oldname", 2, 100, 200, 150000, 300000));
  monitor.update(makeSnapshot(interfaces));
  interfaces.clear();
  interfaces.push_back(makeInfoWithTraffic("newname", 2, 160, 250, 240000, 370000));
  monitor.update(makeSnapshot(interfaces));

  const atm::NetworkPacketMtuStats *eth = monitor.tracked("idx:2");
  CHECK(eth != nullptr);
  CHECK(eth->name == "newname");
  CHECK(eth->history.samples().size() == 2);  // same identity, series continues
  CHECK(eth->history.samples().back().rates.rx_packets_per_second.has_value());
  std::filesystem::remove_all(root);
}

void test_monitor_recreation_starts_fresh() {
  run("MonitorRecreationStartsFresh");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-npm-monitor-recreate";
  std::filesystem::remove_all(root);

  atm::NetworkPacketMtuMonitor monitor(root, 120);
  std::vector<NetworkInterfaceInfo> interfaces;
  // The same name but a new ifindex is a new identity (recreated device), so no
  // change event and no inherited history.
  interfaces.push_back(makeInfoWithTraffic("eth0", 2, 100, 200, 150000, 300000));
  monitor.update(makeSnapshot(interfaces));
  interfaces.clear();
  interfaces.push_back(makeInfoWithTraffic("eth0", 44, 500, 600, 700000, 800000));
  monitor.update(makeSnapshot(interfaces));

  CHECK(monitor.tracked("idx:2") != nullptr);
  CHECK(monitor.tracked("idx:44") != nullptr);
  const atm::NetworkPacketMtuStats *recreated = monitor.tracked("idx:44");
  CHECK(recreated->history.samples().size() == 1);
  CHECK(recreated->mtu_change_count == 0);
  CHECK(*recreated->history.samples().back().rx_packets == 500);
  std::filesystem::remove_all(root);
}

void test_monitor_eviction_bound() {
  run("MonitorEvictionBound");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-npm-monitor-evict";
  std::filesystem::remove_all(root);

  atm::NetworkPacketMtuMonitor monitor(root, 120);
  // Exceed the tracked-interface bound with gone identities; only the eviction
  // candidate set (not-present) shrinks.
  for (std::size_t i = 0; i < atm::NetworkPacketMtuMonitor::kMaxTrackedPacketMtuInterfaces + 8; ++i) {
    std::vector<NetworkInterfaceInfo> interfaces;
    interfaces.push_back(makeInfoWithTraffic("eth" + std::to_string(i),
                                             static_cast<int>(i + 1), 1, 2, 3, 4));
    monitor.update(makeSnapshot(interfaces));
  }
  CHECK(monitor.entries().size() <=
        atm::NetworkPacketMtuMonitor::kMaxTrackedPacketMtuInterfaces);
  std::filesystem::remove_all(root);
}

void test_monitor_pause_skips() {
  run("MonitorPauseSkips");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-npm-monitor-pause";
  std::filesystem::remove_all(root);
  writeFile(makeInterfaceDir(root, "eth0") / "mtu", "1500\n");

  atm::NetworkPacketMtuMonitor monitor(root, 120);
  std::vector<NetworkInterfaceInfo> interfaces;
  interfaces.push_back(makeInfoWithTraffic("eth0", 2, 100, 200, 150000, 300000));
  monitor.setHistoryPaused(true);
  monitor.update(makeSnapshot(interfaces));
  CHECK(monitor.tracked("idx:2") == nullptr);  // nothing sampled while paused
  monitor.setHistoryPaused(false);
  monitor.update(makeSnapshot(interfaces));
  CHECK(monitor.tracked("idx:2") != nullptr);
  CHECK(monitor.tracked("idx:2")->history.samples().size() == 1);
  std::filesystem::remove_all(root);
}

void test_monitor_max_samples_applied() {
  run("MonitorMaxSamplesApplied");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-npm-monitor-max";
  std::filesystem::remove_all(root);
  writeFile(makeInterfaceDir(root, "eth0") / "mtu", "1500\n");

  atm::NetworkPacketMtuMonitor monitor(root, 3);
  for (int i = 0; i < 7; ++i) {
    std::vector<NetworkInterfaceInfo> interfaces;
    interfaces.push_back(makeInfoWithTraffic("eth0", 2, 100 + i * 10, 200 + i * 10,
                                             150000 + i * 1000, 300000 + i * 1000));
    monitor.update(makeSnapshot(interfaces));
  }
  CHECK(monitor.tracked("idx:2")->history.samples().size() == 3);
  CHECK(monitor.historyMaxSamples() == 3);
  std::filesystem::remove_all(root);
  std::vector<NetworkInterfaceInfo> interfaces;
  interfaces.push_back(makeInfoWithTraffic("eth0", 2, 1, 2, 3, 4));
  monitor.reset();
  monitor.update(makeSnapshot(interfaces));
  CHECK(monitor.tracked("idx:2")->history.samples().size() == 1);
  std::filesystem::remove_all(root);
}

void test_monitor_reset() {
  run("MonitorReset");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-npm-monitor-reset";
  std::filesystem::remove_all(root);
  writeFile(makeInterfaceDir(root, "eth0") / "mtu", "1500\n");

  atm::NetworkPacketMtuMonitor monitor(root, 120);
  std::vector<NetworkInterfaceInfo> interfaces;
  interfaces.push_back(makeInfoWithTraffic("eth0", 2, 100, 200, 150000, 300000));
  monitor.update(makeSnapshot(interfaces));
  CHECK(monitor.tracked("idx:2") != nullptr);
  monitor.reset();
  CHECK(monitor.tracked("idx:2") == nullptr);
  CHECK(monitor.entries().empty());
  std::filesystem::remove_all(root);
}

int main() {
  test_size_class_names();
  test_state_names();

  test_read_valid_mtu();
  test_read_malformed_mtu();
  test_read_missing_mtu_file();
  test_read_empty_mtu_file();
  test_read_denied_mtu_file();

  test_derive_rates();
  test_derive_missing_and_decreased();
  test_derive_invalid_windows();
  test_derive_size_estimates();

  test_update_first_sample_baseline();
  test_update_rate_window();
  test_update_stale_tick_preserves();
  test_update_access_denied_stale();
  test_update_mtu_change_detection();
  test_update_mtu_change_after_gap();
  test_update_reset_discontinuity();
  test_update_long_gap_honest_rate();
  test_update_ring_bounded();
  test_update_event_list_bounded();

  test_summarize_baseline();
  test_summarize_window();
  test_summarize_stale_and_unavailable();
  test_summarize_never_sampled();
  test_summarize_mtu_only();

  test_format_rate();
  test_format_throughput();
  test_format_size_estimate();

  test_render_block();
  test_render_block_unavailable();
  test_render_changes();
  test_render_trend();
  test_render_trend_no_data();

  test_monitor_new_identity_updates();
  test_monitor_counters_absent();
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