#pragma once

#include <sys/socket.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "network_monitor.hpp"
#include "resource_history.hpp"

namespace atm {

/// Default bound for the per-interface history ring buffers (samples).
inline constexpr std::size_t kDefaultInterfaceHistorySamples = 120;

/// Informational classification of a network interface, derived from its
/// ARPHRD link type (/sys/class/net/<iface>/type) and, for ambiguous values,
/// heuristics on the interface name. Purely informational: every discovered
/// interface is retained regardless of classification.
enum class NetworkInterfaceType {
  Ethernet,    // ARPHRD_ETHER — wired NICs, most bridges/bonds/veths
  Wifi,        // ARPHRD_IEEE80211 — wireless NICs
  Loopback,    // lo
  Tunnel,      // vxlan/gre/sit/tun/tap ...
  P2P,         // point-to-point links (PPP, ...)
  Bridge,      // software bridge
  Bond,        // link aggregation/bonding master
  InfiniBand,  // ARPHRD_INFINIBAND
  Virtual,     // synthetic: veth/dummy/docker/... (no physical link)
  Unknown,     // no strong signal — never hidden
};
[[nodiscard]] const char *networkInterfaceTypeName(NetworkInterfaceType type);

/// Structured error for the interface-details monitor as a whole.
enum class NetworkInterfaceError {
  None,
  SysfsUnavailable,    // /sys/class/net could not be listed
  GetifaddrsFailed,    // getifaddrs() failed — addresses unavailable
};
[[nodiscard]] const char *networkInterfaceErrorName(NetworkInterfaceError error);

/// One IP address assigned to an interface. Family is preserved (AF_INET /
/// AF_INET6); no DNS resolution is ever performed. Unavailable fields are
/// optional — missing prefix/netmask is not a zero.
struct NetworkAddressInfo {
  int family = AF_UNSPEC;         // AF_INET or AF_INET6
  std::string address;            // formatted with inet_ntop()
  std::optional<std::string> netmask;  // dotted-quad (v4) or hextet (v6)
  std::optional<int> prefix_length;    // /N derived from the netmask when possible
  std::optional<std::string> broadcast;  // v4 broadcast (or P2P destination)
};

/// Link metadata as read from /sys/class/net/<iface>/. Files that are missing,
/// unreadable, "unknown", "-1" or syntactically invalid are represented as
/// "no value" — never as zero.
struct NetworkInterfaceLink {
  std::optional<int> ifindex;        // ifindex, the primary stable identity
  std::optional<unsigned> link_type; // raw ARPHRD value ("type")
  std::optional<std::string> mac_address;  // "address"
  std::optional<int> mtu;                   // "mtu"
  std::optional<std::string> operstate;     // "operstate": up/down/unknown/...
  std::optional<int> carrier;               // 0 or 1 ("carrier")
  std::optional<int> speed_mbps;            // reported link speed in Mb/s
  std::optional<std::string> duplex;        // "full"/"half"
  std::optional<unsigned> flags;            // raw IFF_* bitmask ("flags")
  bool admin_up = false;                    // derived: (flags & IFF_UP) != 0
};

/// Wireless presence/status for an interface. Empty when the interface is not
/// wireless-capable (/sys/class/net/<iface>/wireless absent).
struct WirelessInfo {
  bool present = false;             // /sys/class/net/<iface>/wireless exists
  std::optional<int> link;          // link quality percentage (0-100)
  std::optional<int> level;         // signal level (dBm)
  std::optional<int> noise;         // noise level (dBm)
};

/// One discovered network interface plus its link metadata, addresses,
/// wireless status and (merged) traffic statistics.
struct NetworkInterfaceInfo {
  std::string name;                  // current kernel name, e.g. "enp3s0"
  NetworkInterfaceType type = NetworkInterfaceType::Unknown;
  NetworkInterfaceLink link;
  WirelessInfo wireless;
  std::vector<NetworkAddressInfo> addresses;

  // Traffic from the existing NetworkMonitor snapshot (same tick), merged by
  // the interface's current name. Optional so an interface absent from
  // /proc/net/dev is reported as unavailable, not as zero traffic.
  std::optional<NetworkInterfaceStats> traffic;

  std::chrono::system_clock::time_point refreshed_at;
  std::string error;                 // human-readable per-interface issue

  /// Stable identity: "idx:<ifindex>" when the kernel index is known (the
  /// preferred key — survives renames), otherwise "name:<name>". History and
  /// selection use this so a renamed interface keeps its series while a
  /// recreated interface (new index, same name) does not inherit an old one.
  [[nodiscard]] std::string identity() const;
};

/// Complete discovery result.
struct NetworkInterfaceSnapshot {
  std::vector<NetworkInterfaceInfo> interfaces;  // stable order
  bool sysfs_readable = true;   // /sys/class/net was listable
  bool addresses_readable = true;  // getifaddrs() succeeded
  NetworkInterfaceError error = NetworkInterfaceError::None;
  std::string error_detail;
  std::chrono::system_clock::time_point refreshed_at;
};

/// Bounded per-interface time-series for selected traffic metrics. One sample
/// per refresh (never more); ring buffers keyed by stable interface identity.
struct InterfaceHistory {
  std::string identity;
  ResourceHistory<TimedSample> rx_bytes_per_second{0};
  ResourceHistory<TimedSample> tx_bytes_per_second{0};
  ResourceHistory<TimedSample> rx_errors{0};
  ResourceHistory<TimedSample> tx_errors{0};
  ResourceHistory<TimedSample> rx_dropped{0};
  ResourceHistory<TimedSample> tx_dropped{0};
};

/// Network-interface details monitor.
///
/// Discovers interfaces by listing /sys/class/net, reads their link metadata
/// from sysfs, enumerates addresses with getifaddrs(), merges the traffic
/// counters/rates from the existing NetworkMonitor's snapshot (no second
/// /proc/net/dev parse, no second polling loop — read() is called once per
/// tick by the application's existing monitoring loop), and maintains bounded
/// per-interface history under the Step 14 ResourceHistory ring buffers.
class NetworkInterfaceMonitor {
 public:
  static constexpr std::size_t kMaxTrackedInterfaceSeries = 8;

  NetworkInterfaceMonitor() = default;
  ~NetworkInterfaceMonitor() = default;

  NetworkInterfaceMonitor(const NetworkInterfaceMonitor &) = delete;
  NetworkInterfaceMonitor &operator=(const NetworkInterfaceMonitor &) = delete;

  /// Re-reads interface metadata and addresses and merges `traffic` (the
  /// current NetworkMonitor snapshot). Returns the fresh snapshot (also in
  /// current()). Cheap: O(number of interfaces + number of addresses).
  [[nodiscard]] NetworkInterfaceSnapshot read(const NetworkSnapshot &traffic);

  [[nodiscard]] const NetworkInterfaceSnapshot &current() const { return current_; }

  /// Adds one history sample per tracked interface (one per refresh; no
  /// duplicates). Vanished interfaces are pruned. Public so tests can drive it
  /// hermetically; read() calls it automatically.
  void recordHistory(const NetworkInterfaceSnapshot &snapshot);

  [[nodiscard]] const InterfaceHistory *historyFor(
      const std::string &identity) const;

  [[nodiscard]] const std::unordered_map<std::string, InterfaceHistory> &
  histories() const {
    return histories_;
  }

  void setHistoryMaxSamples(std::size_t max_samples);
  [[nodiscard]] std::size_t historyMaxSamples() const { return max_samples_; }

  void setHistoryPaused(bool paused) { history_paused_ = paused; }
  [[nodiscard]] bool historyPaused() const { return history_paused_; }

  void clearHistory() { histories_.clear(); }

 private:
  NetworkInterfaceSnapshot current_;
  std::unordered_map<std::string, InterfaceHistory> histories_;
  std::size_t max_samples_ = kDefaultInterfaceHistorySamples;
  bool history_paused_ = false;

  void pruneHistory(const std::vector<std::string> &present_identities);
};

// --- Pure/testable parsing helpers (no global state, no syscalls) ----------

/// Classifies an interface from its raw ARPHRD link type.
[[nodiscard]] NetworkInterfaceType networkInterfaceTypeFromArphrd(unsigned arphrd);

/// Refines the ARPHRD classification with name heuristics for ambiguous
/// values (ARPHRD_ETHER covers real NICs, bridges, bonds and synthetic veths;
/// the name disambiguates). Unknown inputs fall back to `Unknown`, never drop.
[[nodiscard]] NetworkInterfaceType classifyInterface(
    const std::string &name, unsigned arphrd);

/// Parses a sysfs integer strictly (optional value). "-1" (the kernel's
/// "unknown speed" sentinel), "unknown", non-numeric and empty inputs yield
/// std::nullopt (unavailable, never zero).
[[nodiscard]] std::optional<int> parseSysfsInt(const std::string &text);

/// Parses a sysfs hexadecimal flag value ("0x1003") optionally as raw decimal.
[[nodiscard]] std::optional<unsigned> parseSysfsFlags(const std::string &text);

/// Formats a link speed in megabits per second ("1000 Mb/s", "2.5 Gb/s") or
/// "unavailable" when unknown/missing.
[[nodiscard]] std::string formatNetworkSpeed(const std::optional<int> &mbps);

/// Formats duplex ("full"/"half") or "-" when unavailable.
[[nodiscard]] std::string formatNetworkDuplex(const std::optional<std::string> &duplex);

/// Formats carrier (yes/no) or "-" when the interface exposes none (virtual).
[[nodiscard]] std::string formatNetworkCarrier(const std::optional<int> &carrier);

/// Human-readable names of the IFF_* flags set in `flags` ("UP RUNNING ...").
[[nodiscard]] std::string formatInterfaceFlagNames(unsigned flags);

/// Raw flag text for diagnostics, e.g. "0x1003" (empty when flags unavailable).
[[nodiscard]] std::string formatInterfaceFlagsRaw(const std::optional<unsigned> &flags);

/// Prefix length (/N) from a netmask sockaddr, or std::nullopt when it is not
/// a valid contiguous mask / not a netmask.
[[nodiscard]] std::optional<int> prefixLengthFromMask(const sockaddr *netmask,
                                                      socklen_t length);

/// Formats a sockaddr (AF_INET / AF_INET6) with inet_ntop(); no DNS lookups.
[[nodiscard]] std::optional<std::string> formatSockaddrAddress(
    const sockaddr *address, socklen_t length);

/// Appends `address` to `addresses` unless an identical (family,address) entry
/// already exists. Returns true when it was added.
[[nodiscard]] bool addAddressDeduplicated(
    std::vector<NetworkAddressInfo> &addresses,
    const NetworkAddressInfo &address);

}  // namespace atm