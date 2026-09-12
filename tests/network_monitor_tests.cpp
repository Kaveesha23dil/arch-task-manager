#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "network_monitor.hpp"

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

// -------------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------------

using atm::NetworkInterfaceStats;

/// Builds the /proc/net/dev body for one interface. `values` carries up to 16
/// counter fields; the well-known dummy tail is zeroed so a short array stresses
/// the offset mapping without losing clarity.
std::string lineFor(const std::string &name,
                    const std::vector<unsigned long long> &values) {
  std::string line = "  " + name + ": ";
  for (int i = 0; i < 16; ++i) {
    const std::size_t value = (i < static_cast<int>(values.size()))
                                  ? static_cast<std::size_t>(values[i])
                                  : 0;
    line += std::to_string(value);
    line += (i < 15 ? " " : "");
  }
  return line;
}

std::string twoHeaderLines() {
  return "Inter-|   Receive                                                "
         "Transmit\n"
         " face |bytes    packets errs drop fifo frame compressed multicast|"
         "bytes    packets errs drop fifo colls carrier compressed\n";
}

// -------------------------------------------------------------------------
// Parsing
// -------------------------------------------------------------------------

void test_parses_two_interfaces() {
  run("two normal interfaces parse with correct field offsets");
  std::istringstream input(
      twoHeaderLines() +
      "  eth0: 111 222 333 444 555 666 777 888 999 1111 2222 3333 4444 5555 "
      "6666 7777\n"
      "    lo: 100 57 0 0 0 0 0 0 200 57 0 0 0 0 0 0\n");
  const std::vector<NetworkInterfaceStats> parsed =
      atm::parseNetworkStatsText(input);
  CHECK(parsed.size() == 2);
  CHECK(parsed[0].name == "eth0");
  CHECK(parsed[0].rx_bytes == 111);
  CHECK(parsed[0].rx_packets == 222);
  CHECK(parsed[0].rx_errors == 333);
  CHECK(parsed[0].rx_dropped == 444);
  CHECK(parsed[0].tx_bytes == 999);
  CHECK(parsed[0].tx_packets == 1111);
  CHECK(parsed[0].tx_errors == 2222);
  CHECK(parsed[0].tx_dropped == 3333);
  CHECK(parsed[1].name == "lo");
  CHECK(parsed[1].rx_bytes == 100);
  CHECK(parsed[1].tx_bytes == 200);
}

void test_field_offset_mapping_stress() {
  run("only the eight real counters are consumed at the right offsets");
  // Values chosen so a (wrong) shift by one field would break every check.
  std::istringstream input(
      twoHeaderLines() +
      lineFor("enp3s0", {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u}) + "\n");
  const std::vector<NetworkInterfaceStats> parsed =
      atm::parseNetworkStatsText(input);
  CHECK(parsed.size() == 1);
  CHECK(parsed[0].rx_bytes == 1);
  CHECK(parsed[0].rx_packets == 2);
  CHECK(parsed[0].rx_errors == 3);
  CHECK(parsed[0].rx_dropped == 4);
  CHECK(parsed[0].tx_bytes == 9);
  CHECK(parsed[0].tx_packets == 10);
  CHECK(parsed[0].tx_errors == 0);
  CHECK(parsed[0].tx_dropped == 0);
  CHECK(parsed[0].rx_bytes_per_second == 0);
  CHECK(parsed[0].tx_bytes_per_second == 0);
  CHECK(parsed[0].state.empty());
  CHECK(!parsed[0].loopback);
}

void test_names_without_padding() {
  run("interface names are trimmed regardless of column padding");
  std::istringstream input(twoHeaderLines() + "wlan0: 1 2 3 4 5 6 7 8 9 10 11 "
                                              "12 13 14 15 16\n");
  const std::vector<NetworkInterfaceStats> parsed =
      atm::parseNetworkStatsText(input);
  CHECK(parsed.size() == 1);
  CHECK(parsed[0].name == "wlan0");
  CHECK(parsed[0].rx_bytes == 1);
}

void test_unusual_names() {
  run("dashed, dotted and heavily prefixed names are retained");
  std::istringstream input(
      twoHeaderLines() +
      "veth-0789ab: 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16\n"
      "tap0.100: 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16\n"
      "br-lan: 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16\n");
  const std::vector<NetworkInterfaceStats> parsed =
      atm::parseNetworkStatsText(input);
  CHECK(parsed.size() == 3);
  CHECK(parsed[0].name == "veth-0789ab");
  CHECK(parsed[1].name == "tap0.100");
  CHECK(parsed[2].name == "br-lan");
}

void test_large_uint64() {
  run("values at the top of the uint64 range are parsed exactly");
  std::istringstream input(
      twoHeaderLines() +
      "eth0: 18446744073709551615 18446744073709551614 100 200 0 0 0 0 "
      "18446744073709551615 5 6 7 0 0 0 0\n");
  const std::vector<NetworkInterfaceStats> parsed =
      atm::parseNetworkStatsText(input);
  CHECK(parsed.size() == 1);
  CHECK(parsed[0].rx_bytes == 18446744073709551615ULL);
  CHECK(parsed[0].rx_packets == 18446744073709551614ULL);
  CHECK(parsed[0].tx_bytes == 18446744073709551615ULL);
  CHECK(parsed[0].rx_errors == 100);
  CHECK(parsed[0].tx_dropped == 7);
}

void test_malformed_line_is_skipped() {
  run("malformed and truncated lines are skipped, valid neighbors kept");
  std::istringstream input(
      twoHeaderLines() +
      "eth0: 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16\n"
      "broken because it has no colon digits on the right\n"
      "bad0: 1 2 three 4 5 6 7 8 9 10 11 12 13 14 15\n"
      "short0: 1 2 3 4 5 6 7 8\n"
      "eth1: 16 15 14 13 12 11 10 9 8 7 6 5 4 3 2 1\n");
  const std::vector<NetworkInterfaceStats> parsed =
      atm::parseNetworkStatsText(input);
  CHECK(parsed.size() == 2);
  CHECK(parsed[0].name == "eth0");
  CHECK(parsed[1].name == "eth1");
  CHECK(parsed[1].rx_bytes == 16);
}

void test_empty_name_is_skipped() {
  run("line with an empty interface name is skipped");
  std::istringstream input(twoHeaderLines() +
                           ": 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16\n"
                           "eth0: 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16\n");
  const std::vector<NetworkInterfaceStats> parsed =
      atm::parseNetworkStatsText(input);
  CHECK(parsed.size() == 1);
  CHECK(parsed[0].name == "eth0");
}

void test_extra_fields_are_ignored() {
  run("a 17th column (future kernel formats) is ignored, not a failure");
  std::istringstream input(twoHeaderLines() +
                           "eth0: 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 99\n");
  const std::vector<NetworkInterfaceStats> parsed =
      atm::parseNetworkStatsText(input);
  CHECK(parsed.size() == 1);
  CHECK(parsed[0].rx_bytes == 1);
  CHECK(parsed[0].tx_dropped == 12);
  CHECK(parsed[0].tx_bytes == 9);
}

void test_headers_only_or_empty() {
  run("empty input and header-only input yield no interfaces");
  std::istringstream empty("");
  CHECK(atm::parseNetworkStatsText(empty).empty());
  std::istringstream headers(twoHeaderLines());
  CHECK(atm::parseNetworkStatsText(headers).empty());
}

void test_sysfs_fields_are_not_touched() {
  run("state/loopback are filled by readNetworkStats, not the text parser");
  std::istringstream input(
      twoHeaderLines() + "  lo: 100 57 0 0 0 0 0 0 200 57 0 0 0 0 0 0\n");
  const std::vector<NetworkInterfaceStats> parsed =
      atm::parseNetworkStatsText(input);
  CHECK(parsed.size() == 1);
  CHECK(parsed[0].name == "lo");
  CHECK(!parsed[0].loopback);  // left untouched here; sysfs sets it
  CHECK(parsed[0].state.empty());
}

int main() {
  test_parses_two_interfaces();
  test_field_offset_mapping_stress();
  test_names_without_padding();
  test_unusual_names();
  test_large_uint64();
  test_malformed_line_is_skipped();
  test_empty_name_is_skipped();
  test_extra_fields_are_ignored();
  test_headers_only_or_empty();
  test_sysfs_fields_are_not_touched();

  std::fprintf(stderr, "\n%zu checks, %d failures\n", g_checks + 0UL,
               g_failures);
  if (g_failures != 0) {
    std::fprintf(stderr, "RESULT: FAIL\n");
    return 1;
  }
  std::fprintf(stderr, "RESULT: PASS\n");
  return 0;
}