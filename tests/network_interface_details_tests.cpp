#include <arpa/inet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <sys/socket.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "network_interface_details.hpp"

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
void run(const char *name) {
  std::fprintf(stderr, "TEST %s\n", name);
}
}  // namespace

#define CHECK(expr) ::expect((expr), #expr, __FILE__, __LINE__)

// -------------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------------

using atm::NetworkAddressInfo;
using atm::NetworkInterfaceInfo;
using atm::NetworkInterfaceSnapshot;
using atm::NetworkInterfaceStats;

/// Builds a synthetic interface for history/identity tests.
NetworkInterfaceInfo makeIfInfo(const std::string &name, int ifindex,
                                bool has_traffic, std::uint64_t rx_bps = 0,
                                std::uint64_t tx_bps = 0,
                                std::uint64_t rx_errors = 0,
                                std::uint64_t tx_dropped = 0) {
  NetworkInterfaceInfo info;
  info.name = name;
  info.link.ifindex = ifindex;
  if (has_traffic) {
    NetworkInterfaceStats stats;
    stats.name = name;
    stats.rx_bytes_per_second = rx_bps;
    stats.tx_bytes_per_second = tx_bps;
    stats.rx_errors = rx_errors;
    stats.tx_dropped = tx_dropped;
    info.traffic = stats;
  }
  return info;
}

NetworkInterfaceSnapshot makeSnapshot(std::vector<NetworkInterfaceInfo> v) {
  NetworkInterfaceSnapshot snapshot;
  snapshot.interfaces = std::move(v);
  snapshot.refreshed_at = std::chrono::system_clock::now();
  return snapshot;
}

// -------------------------------------------------------------------------
// Interface classification ("discovery" semantics)
// -------------------------------------------------------------------------

void test_classification_from_arphrd() {
  run("ARPHRD type classification");
  CHECK(atm::networkInterfaceTypeFromArphrd(ARPHRD_ETHER) ==
        atm::NetworkInterfaceType::Ethernet);
  CHECK(atm::networkInterfaceTypeFromArphrd(ARPHRD_IEEE80211) ==
        atm::NetworkInterfaceType::Wifi);
  CHECK(atm::networkInterfaceTypeFromArphrd(ARPHRD_LOOPBACK) ==
        atm::NetworkInterfaceType::Loopback);
  CHECK(atm::networkInterfaceTypeFromArphrd(ARPHRD_PPP) ==
        atm::NetworkInterfaceType::P2P);
  CHECK(atm::networkInterfaceTypeFromArphrd(ARPHRD_NONE) ==
        atm::NetworkInterfaceType::Virtual);
  CHECK(atm::networkInterfaceTypeFromArphrd(ARPHRD_INFINIBAND) ==
        atm::NetworkInterfaceType::InfiniBand);
  CHECK(atm::networkInterfaceTypeFromArphrd(9999) ==
        atm::NetworkInterfaceType::Unknown);
}

void test_classification_with_names() {
  run("name-based refinement (Ethernet ARPHRD)");
  CHECK(atm::classifyInterface("lo", ARPHRD_ETHER) ==
        atm::NetworkInterfaceType::Loopback);
  CHECK(atm::classifyInterface("enp3s0", ARPHRD_ETHER) ==
        atm::NetworkInterfaceType::Ethernet);
  CHECK(atm::classifyInterface("eth0", ARPHRD_ETHER) ==
        atm::NetworkInterfaceType::Ethernet);
  CHECK(atm::classifyInterface("wlp3s0", ARPHRD_ETHER) ==
        atm::NetworkInterfaceType::Wifi);
  CHECK(atm::classifyInterface("wlan5", ARPHRD_ETHER) ==
        atm::NetworkInterfaceType::Wifi);
  CHECK(atm::classifyInterface("wwan0", ARPHRD_PPP) ==
        atm::NetworkInterfaceType::P2P);
  CHECK(atm::classifyInterface("veth9a2c", ARPHRD_ETHER) ==
        atm::NetworkInterfaceType::Virtual);
  CHECK(atm::classifyInterface("docker0", ARPHRD_ETHER) ==
        atm::NetworkInterfaceType::Virtual);
  CHECK(atm::classifyInterface("virbr0", ARPHRD_ETHER) ==
        atm::NetworkInterfaceType::Virtual);
  CHECK(atm::classifyInterface("br-1a2b3c", ARPHRD_ETHER) ==
        atm::NetworkInterfaceType::Virtual);
  CHECK(atm::classifyInterface("vxlan42", ARPHRD_ETHER) ==
        atm::NetworkInterfaceType::Tunnel);
  CHECK(atm::classifyInterface("wg0", ARPHRD_ETHER) ==
        atm::NetworkInterfaceType::Tunnel);
  CHECK(atm::classifyInterface("tun0", ARPHRD_ETHER) ==
        atm::NetworkInterfaceType::Tunnel);
  CHECK(atm::classifyInterface("tap1", ARPHRD_ETHER) ==
        atm::NetworkInterfaceType::Tunnel);
  CHECK(atm::classifyInterface("bond0", ARPHRD_ETHER) ==
        atm::NetworkInterfaceType::Bond);
  CHECK(atm::classifyInterface("br0", ARPHRD_ETHER) ==
        atm::NetworkInterfaceType::Bridge);
  CHECK(atm::classifyInterface("wlan0", ARPHRD_IEEE80211) ==
        atm::NetworkInterfaceType::Wifi);
  CHECK(atm::classifyInterface("lo", ARPHRD_LOOPBACK) ==
        atm::NetworkInterfaceType::Loopback);
}

// -------------------------------------------------------------------------
// Sysfs value parsing / formatting
// -------------------------------------------------------------------------

void test_parse_sysfs_int() {
  run("sysfs integer parsing");
  CHECK(atm::parseSysfsInt("1000").has_value() && *atm::parseSysfsInt("1000") == 1000);
  CHECK(atm::parseSysfsInt("0").has_value() && *atm::parseSysfsInt("0") == 0);
  CHECK(!atm::parseSysfsInt("unknown").has_value());
  CHECK(!atm::parseSysfsInt("").has_value());
  CHECK(!atm::parseSysfsInt("not-a-number").has_value());
  CHECK(!atm::parseSysfsInt("12x").has_value());
  CHECK(!atm::parseSysfsInt("99999999999999999999").has_value());  // overflow
  CHECK(!atm::parseSysfsInt("-1").has_value());  // kernel "no value" sentinel
  CHECK(atm::parseSysfsInt("1").has_value() && *atm::parseSysfsInt("1") == 1);
  CHECK(!atm::parseSysfsInt("unknown").has_value());
  CHECK(!atm::parseSysfsInt("").has_value());
  CHECK(!atm::parseSysfsInt("not-a-number").has_value());
  CHECK(!atm::parseSysfsInt("12x").has_value());
  CHECK(!atm::parseSysfsInt("99999999999999999999").has_value());  // overflow
}

void test_parse_sysfs_flags() {
  run("sysfs flags parsing");
  CHECK(atm::parseSysfsFlags("0x1003").has_value() &&
        *atm::parseSysfsFlags("0x1003") == 0x1003U);
  CHECK(atm::parseSysfsFlags("0X10003").has_value() &&
        *atm::parseSysfsFlags("0X10003") == 0x10003U);
  CHECK(atm::parseSysfsFlags("1003").has_value() &&
        *atm::parseSysfsFlags("1003") == 1003U);  // decimal accepted too
  CHECK(atm::parseSysfsFlags("0xffffffff").has_value() &&
        *atm::parseSysfsFlags("0xffffffff") == 0xffffffffU);
  CHECK(!atm::parseSysfsFlags("0x").has_value());
  CHECK(!atm::parseSysfsFlags("").has_value());
  CHECK(!atm::parseSysfsFlags("zzz").has_value());
  CHECK(!atm::parseSysfsFlags("0x100000000").has_value());  // overflow
}

void test_format_speed() {
  run("link speed formatting");
  CHECK(atm::formatNetworkSpeed(std::nullopt) == "unavailable");
  CHECK(atm::formatNetworkSpeed(0) == "unavailable");
  CHECK(atm::formatNetworkSpeed(-1) == "unavailable");
  CHECK(atm::formatNetworkSpeed(100) == "100 Mb/s");
  CHECK(atm::formatNetworkSpeed(1000) == "1 Gb/s");
  CHECK(atm::formatNetworkSpeed(2500) == "2.5 Gb/s");
  CHECK(atm::formatNetworkSpeed(10000) == "10 Gb/s");
}

void test_format_duplex_carrier_flags() {
  run("duplex/carrier/flags formatting");
  CHECK(atm::formatNetworkDuplex("full") == "full");
  CHECK(atm::formatNetworkDuplex("half") == "half");
  CHECK(atm::formatNetworkDuplex(std::nullopt) == "-");
  CHECK(atm::formatNetworkCarrier(1) == "yes");
  CHECK(atm::formatNetworkCarrier(0) == "no");
  CHECK(atm::formatNetworkCarrier(std::nullopt) == "-");
  CHECK(atm::formatInterfaceFlagNames(IFF_UP) == "UP");
  CHECK(atm::formatInterfaceFlagNames(IFF_UP | IFF_RUNNING | IFF_MULTICAST) ==
        "UP RUNNING MULTICAST");
  CHECK(atm::formatInterfaceFlagsRaw(
            std::optional<unsigned>(0x1003U)) == "0x1003");
  CHECK(atm::formatInterfaceFlagsRaw(std::nullopt) == "");
}

// -------------------------------------------------------------------------
// Address formatting / prefix derivation (no DNS, no syscalls)
// -------------------------------------------------------------------------

void test_format_addresses() {
  run("sockaddr address formatting");
  {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    CHECK(::inet_pton(AF_INET, "192.168.1.5", &addr.sin_addr) == 1);
    const auto text = atm::formatSockaddrAddress(
        reinterpret_cast<const sockaddr *>(&addr), sizeof(addr));
    CHECK(text.has_value() && *text == "192.168.1.5");
  }
  {
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    CHECK(::inet_pton(AF_INET6, "fe80::abcd:1", &addr.sin6_addr) == 1);
    const auto text = atm::formatSockaddrAddress(
        reinterpret_cast<const sockaddr *>(&addr), sizeof(addr));
    CHECK(text.has_value() && *text == "fe80::abcd:1");
  }
  {
    // Unsupported family → no address, but never a failure.
    const sockaddr raw{};
    CHECK(!atm::formatSockaddrAddress(&raw, sizeof(raw)).has_value());
  }
  CHECK(!atm::formatSockaddrAddress(nullptr, sizeof(sockaddr_in)).has_value());
}

void test_prefix_from_mask() {
  run("prefix length from netmask");
  {
    sockaddr_in mask{};
    mask.sin_family = AF_INET;
    CHECK(::inet_pton(AF_INET, "255.255.255.0", &mask.sin_addr) == 1);
    const auto prefix = atm::prefixLengthFromMask(
        reinterpret_cast<const sockaddr *>(&mask), sizeof(mask));
    CHECK(prefix.has_value() && *prefix == 24);
  }
  {
    sockaddr_in mask{};
    mask.sin_family = AF_INET;
    CHECK(::inet_pton(AF_INET, "255.255.255.255", &mask.sin_addr) == 1);
    const auto prefix = atm::prefixLengthFromMask(
        reinterpret_cast<const sockaddr *>(&mask), sizeof(mask));
    CHECK(prefix.has_value() && *prefix == 32);
  }
  {
    sockaddr_in mask{};
    mask.sin_family = AF_INET;
    CHECK(::inet_pton(AF_INET, "0.0.0.0", &mask.sin_addr) == 1);
    const auto prefix = atm::prefixLengthFromMask(
        reinterpret_cast<const sockaddr *>(&mask), sizeof(mask));
    CHECK(prefix.has_value() && *prefix == 0);
  }
  {
    sockaddr_in mask{};
    mask.sin_family = AF_INET;
    CHECK(::inet_pton(AF_INET, "255.0.255.0", &mask.sin_addr) == 1);
    // Non-contiguous mask has no /N prefix — reported as a plain netmask.
    const auto prefix = atm::prefixLengthFromMask(
        reinterpret_cast<const sockaddr *>(&mask), sizeof(mask));
    CHECK(!prefix.has_value());
  }
  {
    sockaddr_in6 mask{};
    mask.sin6_family = AF_INET6;
    CHECK(::inet_pton(AF_INET6, "ffff:ffff:ffff:ffff::", &mask.sin6_addr) == 1);
    const auto prefix = atm::prefixLengthFromMask(
        reinterpret_cast<const sockaddr *>(&mask), sizeof(mask));
    CHECK(prefix.has_value() && *prefix == 64);
  }
  {
    sockaddr_in6 mask{};
    mask.sin6_family = AF_INET6;
    CHECK(::inet_pton(AF_INET6, "ffff::ffff", &mask.sin6_addr) == 1);
    const auto prefix = atm::prefixLengthFromMask(
        reinterpret_cast<const sockaddr *>(&mask), sizeof(mask));
    CHECK(!prefix.has_value());
  }
  CHECK(!atm::prefixLengthFromMask(nullptr, sizeof(sockaddr_in)).has_value());
  {
    // Too-short socklen is rejected rather than over-read.
    sockaddr_in mask{};
    mask.sin_family = AF_INET;
    CHECK(::inet_pton(AF_INET, "255.255.255.0", &mask.sin_addr) == 1);
    CHECK(!atm::prefixLengthFromMask(
              reinterpret_cast<const sockaddr *>(&mask), sizeof(sa_family_t))
              .has_value());
  }
}

void test_address_deduplication() {
  run("duplicate address suppression");
  std::vector<NetworkAddressInfo> addresses;
  NetworkAddressInfo a;
  a.family = AF_INET;
  a.address = "192.168.1.5";
  NetworkAddressInfo b = a;
  NetworkAddressInfo c = a;
  c.family = AF_INET6;
  c.address = "fe80::1";
  CHECK(atm::addAddressDeduplicated(addresses, a));
  CHECK(!atm::addAddressDeduplicated(addresses, b));  // exact duplicate
  CHECK(atm::addAddressDeduplicated(addresses, c));   // different family
  CHECK(addresses.size() == 2);
}

// -------------------------------------------------------------------------
// Stable identity
// -------------------------------------------------------------------------

void test_identity() {
  run("stable interface identity");
  NetworkInterfaceInfo by_index = makeIfInfo("enp3s0", 2, false);
  CHECK(by_index.identity() == "idx:2");
  // A rename keeps the index → same identity (history is preserved).
  NetworkInterfaceInfo renamed = makeIfInfo("enx000a", 2, false);
  CHECK(renamed.identity() == by_index.identity());
  // A recreated interface reuses the name but gets a new index → fresh.
  NetworkInterfaceInfo recreated = makeIfInfo("enp3s0", 3, false);
  CHECK(recreated.identity() != by_index.identity());
  // No index (unusual) → name-based fallback identity.
  NetworkInterfaceInfo no_index = makeIfInfo("eth0", -1, false);
  no_index.link.ifindex.reset();
  CHECK(no_index.identity() == "name:eth0");
}

void test_identity_virtual_appearance_disappearance() {
  run("virtual interface appearance/disappearance stays distinct");
  NetworkInterfaceInfo veth1 = makeIfInfo("veth0", 42, false);  // first veth
  NetworkInterfaceInfo veth2 = makeIfInfo("veth0", 43, false);  // recreated
  CHECK(veth1.identity() != veth2.identity());
}

// -------------------------------------------------------------------------
// History
// -------------------------------------------------------------------------

void test_history_one_sample_per_refresh() {
  run("one sample per refresh, no duplicates");
  atm::NetworkInterfaceMonitor monitor;
  const auto snapshot = makeSnapshot({makeIfInfo("enp3s0", 2, true, 100, 200)});
  monitor.recordHistory(snapshot);
  const atm::InterfaceHistory *h = monitor.historyFor("idx:2");
  CHECK(h != nullptr);
  CHECK(h->rx_bytes_per_second.size() == 1);
  CHECK(h->tx_bytes_per_second.size() == 1);
  CHECK(h->rx_errors.size() == 1);
  CHECK(h->tx_dropped.size() == 1);
  monitor.recordHistory(snapshot);
  h = monitor.historyFor("idx:2");
  CHECK(h->rx_bytes_per_second.size() == 2);  // exactly one more sample
}

void test_history_appearing_disappearing() {
  run("interface appear/disappear is tracked and pruned");
  atm::NetworkInterfaceMonitor monitor;
  monitor.recordHistory(makeSnapshot({makeIfInfo("eth0", 2, true)}));
  CHECK(monitor.histories().size() == 1);
  monitor.recordHistory(makeSnapshot(
      {makeIfInfo("eth0", 2, true), makeIfInfo("veth0", 42, true)}));
  CHECK(monitor.histories().size() == 2);
  monitor.recordHistory(makeSnapshot({makeIfInfo("eth0", 2, true)}));
  CHECK(monitor.histories().size() == 1);
  CHECK(monitor.historyFor("idx:2") != nullptr);
  CHECK(monitor.historyFor("idx:42") == nullptr);  // vanished series removed
}

void test_history_rename_preserves_series() {
  run("rename keeps the same series via ifindex identity");
  atm::NetworkInterfaceMonitor monitor;
  monitor.recordHistory(makeSnapshot({makeIfInfo("enp3s0", 7, true, 55)}));
  const atm::InterfaceHistory *before = monitor.historyFor("idx:7");
  CHECK(before != nullptr);
  CHECK(before->rx_bytes_per_second.size() == 1);
  // Same ifindex, new name (predictable rename) → same identity.
  monitor.recordHistory(makeSnapshot({makeIfInfo("enx001122", 7, true, 99)}));
  CHECK(monitor.histories().size() == 1);
  const atm::InterfaceHistory *after = monitor.historyFor("idx:7");
  CHECK(after == before);
  CHECK(after->rx_bytes_per_second.size() == 2);
  CHECK(after->rx_bytes_per_second.samples().back().value == 99.0);
}

void test_history_recreation_starts_fresh() {
  run("recreated interface does not inherit an old series");
  atm::NetworkInterfaceMonitor monitor;
  monitor.recordHistory(makeSnapshot({makeIfInfo("eth0", 2, true, 111)}));
  CHECK(monitor.historyFor("idx:2") != nullptr);
  monitor.recordHistory(makeSnapshot({makeIfInfo("eth0", 3, true, 222)}));
  // The recreated "eth0" has a new ifindex, so the old idx:2 series belongs
  // to an interface that vanished and is pruned; a name-keyed history would
  // have wrongly carried idx:2's samples over.
  CHECK(monitor.histories().size() == 1);
  CHECK(monitor.historyFor("idx:2") == nullptr);
  const atm::InterfaceHistory *h = monitor.historyFor("idx:3");
  CHECK(h != nullptr);
  CHECK(h->rx_bytes_per_second.size() == 1);
  CHECK(h->rx_bytes_per_second.samples().back().value == 222.0);
}

void test_history_bounded() {
  run("history is bounded");
  atm::NetworkInterfaceMonitor monitor;
  monitor.setHistoryMaxSamples(5);
  const auto snapshot = makeSnapshot({makeIfInfo("enp3s0", 2, true, 1)});
  for (int i = 0; i < 20; ++i) {
    monitor.recordHistory(snapshot);
  }
  const atm::InterfaceHistory *h = monitor.historyFor("idx:2");
  CHECK(h != nullptr);
  CHECK(h->rx_bytes_per_second.size() == 5);
  CHECK(h->rx_errors.size() == 5);
  CHECK(monitor.historyMaxSamples() == 5);
}

void test_history_disabled_and_resumed() {
  run("paused history records nothing");
  atm::NetworkInterfaceMonitor monitor;
  monitor.setHistoryPaused(true);
  const auto snapshot = makeSnapshot({makeIfInfo("eth0", 2, true, 1)});
  monitor.recordHistory(snapshot);
  monitor.recordHistory(snapshot);
  CHECK(monitor.histories().empty());
  monitor.setHistoryPaused(false);
  monitor.recordHistory(snapshot);
  CHECK(monitor.historyFor("idx:2")->rx_bytes_per_second.size() == 1);
}

void test_history_no_traffic_not_tracked() {
  run("interfaces without a traffic entry contribute no series");
  atm::NetworkInterfaceMonitor monitor;
  monitor.recordHistory(makeSnapshot({makeIfInfo("eth0", 2, false)}));
  CHECK(monitor.histories().empty());
}

void test_history_series_limit() {
  run("excessive interface sets are bounded");
  atm::NetworkInterfaceMonitor monitor;
  std::vector<NetworkInterfaceInfo> infos;
  for (int i = 1; i <= 20; ++i) {
    infos.push_back(makeIfInfo("if" + std::to_string(i), i, true));
  }
  monitor.recordHistory(makeSnapshot(std::move(infos)));
  CHECK(monitor.histories().size() ==
        atm::NetworkInterfaceMonitor::kMaxTrackedInterfaceSeries);
}

void test_history_records_cumulative_counters() {
  run("error/drop counters sampled as cumulative values");
  atm::NetworkInterfaceMonitor monitor;
  monitor.recordHistory(
      makeSnapshot({makeIfInfo("eth0", 2, true, 10, 20, /*rx_errors=*/7,
                               /*tx_dropped=*/3)}));
  const atm::InterfaceHistory *h = monitor.historyFor("idx:2");
  CHECK(h->rx_errors.samples().back().value == 7.0);
  CHECK(h->tx_dropped.samples().back().value == 3.0);
  // Counter reset (kernel re-seed): the new lower value is sampled normally;
  // no negative delta, no crash, series continues.
  monitor.recordHistory(
      makeSnapshot({makeIfInfo("eth0", 2, true, 0, 0, /*rx_errors=*/1,
                               /*tx_dropped=*/0)}));
  CHECK(h->rx_errors.samples().size() == 2);
  CHECK(h->rx_errors.samples().back().value == 1.0);
  CHECK(h->rx_dropped.samples().size() == 2);
}

void test_history_respects_max_samples_setting() {
  run("setHistoryMaxSamples rebuilds series at the new bound");
  atm::NetworkInterfaceMonitor monitor;
  const auto snapshot = makeSnapshot({makeIfInfo("eth0", 2, true, 1)});
  monitor.recordHistory(snapshot);
  CHECK(monitor.historyFor("idx:2")->rx_bytes_per_second.size() == 1);
  monitor.setHistoryMaxSamples(3);
  CHECK(monitor.historyFor("idx:2")->rx_bytes_per_second.size() == 0);
  monitor.recordHistory(snapshot);
  monitor.recordHistory(snapshot);
  monitor.recordHistory(snapshot);
  monitor.recordHistory(snapshot);
  CHECK(monitor.historyFor("idx:2")->rx_bytes_per_second.size() == 3);
}

int main() {
  // Classification / discovery semantics
  test_classification_from_arphrd();
  test_classification_with_names();

  // Sysfs parsing & formatting
  test_parse_sysfs_int();
  test_parse_sysfs_flags();
  test_format_speed();
  test_format_duplex_carrier_flags();

  // Address handling
  test_format_addresses();
  test_prefix_from_mask();
  test_address_deduplication();

  // Stable identity
  test_identity();
  test_identity_virtual_appearance_disappearance();

  // History
  test_history_one_sample_per_refresh();
  test_history_appearing_disappearing();
  test_history_rename_preserves_series();
  test_history_recreation_starts_fresh();
  test_history_bounded();
  test_history_disabled_and_resumed();
  test_history_no_traffic_not_tracked();
  test_history_series_limit();
  test_history_records_cumulative_counters();
  test_history_respects_max_samples_setting();

  std::fprintf(stderr, "\n%d checks, %d failures\n", g_checks, g_failures);
  if (g_failures != 0) {
    std::fprintf(stderr, "RESULT: FAIL\n");
    return 1;
  }
  std::fprintf(stderr, "RESULT: PASS\n");
  return 0;
}