#include "network_interface_details.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace atm {

namespace {

using std::uint64_t;

/// Strips leading/trailing ASCII whitespace (including a CR).
std::string trimWhitespace(const std::string &text) {
  std::size_t begin = 0;
  while (begin < text.size() &&
         (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r')) {
    ++begin;
  }
  std::size_t end = text.size();
  while (end > begin &&
         (text[end - 1] == ' ' || text[end - 1] == '\t' ||
          text[end - 1] == '\r')) {
    --end;
  }
  return text.substr(begin, end - begin);
}

/// Reads one /sys/class/net/<iface>/<field> file, trimmed. A missing, empty or
/// unreadable file yields std::nullopt (expected for virtual/loopback
/// interfaces — never an application failure).
std::optional<std::string> readSysfsText(const std::string &iface,
                                         const char *field) {
  std::ifstream file("/sys/class/net/" + iface + "/" + field);
  std::string text;
  if (!(file >> text)) {
    return std::nullopt;
  }
  text = trimWhitespace(text);
  if (text.empty()) {
    return std::nullopt;
  }
  return text;
}

/// Strict decimal parse used by all sysfs numeric fields. "-1", "unknown",
/// non-numeric and empty inputs become "unavailable", never zero.
bool parseDecimalInt(const std::string &text, int &value) {
  if (text.empty()) {
    return false;
  }
  long parsed = 0;
  std::size_t index = 0;
  bool negative = false;
  if (text[0] == '-') {
    negative = true;
    if (text.size() == 1) {
      return false;
    }
    index = 1;
  }
  for (; index < text.size(); ++index) {
    const char c = text[index];
    if (c < '0' || c > '9') {
      return false;
    }
    parsed = parsed * 10 + (c - '0');
    if (parsed > 2147483647) {
      return false;  // outside int range — treat as unavailable
    }
  }
  if (negative) {
    parsed = -parsed;
  }
  value = static_cast<int>(parsed);
  return true;
}

/// Strict unsigned decimal parse (used for the ARPHRD "type" and "ifindex",
/// which are never negative).
bool parseDecimalU32(const std::string &text, unsigned &value) {
  if (text.empty()) {
    return false;
  }
  uint64_t parsed = 0;
  constexpr uint64_t kMax = std::numeric_limits<unsigned>::max();
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    const uint64_t digit = static_cast<uint64_t>(c - '0');
    if (parsed > (kMax - digit) / 10) {
      return false;
    }
    parsed = parsed * 10 + digit;
  }
  value = static_cast<unsigned>(parsed);
  return true;
}

/// True when `text` starts with the (null-terminated) prefix `p`.
bool startsWith(const std::string &text, const char *p) {
  return text.rfind(p, 0) == 0;
}

/// Lists the entries of /sys/class/net. `error_detail` is set when the
/// directory is missing/unreadable so the caller can report a structured
/// error rather than pretending the machine has no interfaces.
std::vector<std::string> listNetworkInterfaces(std::string &error_detail) {
  DIR *dir = ::opendir("/sys/class/net");
  if (dir == nullptr) {
    error_detail = std::strerror(errno);
    return {};
  }
  std::vector<std::string> names;
  while (const dirent *entry = ::readdir(dir)) {
    const std::string name = entry->d_name;
    if (name == "." || name == "..") {
      continue;
    }
    names.push_back(name);
  }
  ::closedir(dir);
  std::sort(names.begin(), names.end());
  return names;
}

/// Reads a wireless attribute file, tolerating absence and malformed values.
std::optional<int> readWirelessInt(const std::string &iface, const char *field) {
  const std::optional<std::string> text = readSysfsText(iface, field);
  if (!text.has_value()) {
    return std::nullopt;
  }
  int value = 0;
  if (!parseDecimalInt(*text, value)) {
    return std::nullopt;
  }
  return value;
}

/// True when an interface is loopback: names "lo" or the IFF_LOOPBACK flag.
bool isLoopback(const NetworkInterfaceInfo &info) {
  return info.name == "lo" ||
         (info.link.flags.has_value() &&
          (*info.link.flags & IFF_LOOPBACK) != 0) ||
         info.type == NetworkInterfaceType::Loopback;
}

/// Counts the number of leading one-bits in `bytes`, verifying the remainder
/// is all zeros (a contiguous netmask). Non-contiguous masks are not prefixes.
std::optional<int> contiguousPrefixLength(const unsigned char *bytes,
                                          std::size_t count) {
  int ones = 0;
  bool seen_zero = false;
  for (std::size_t i = 0; i < count; ++i) {
    for (int bit = 7; bit >= 0; --bit) {
      const bool one = (bytes[i] & (1u << bit)) != 0;
      if (one) {
        if (seen_zero) {
          return std::nullopt;  // 1 after a 0 — not a contiguous mask
        }
        ++ones;
      } else {
        seen_zero = true;
      }
    }
  }
  return ones;
}

void mergeTrafficMap(const NetworkSnapshot &traffic,
                     std::unordered_map<std::string, NetworkInterfaceStats>
                         &by_name) {
  by_name.reserve(traffic.interfaces.size());
  for (const NetworkInterfaceStats &stats : traffic.interfaces) {
    by_name.emplace(stats.name, stats);
  }
}

}  // namespace

const char *networkInterfaceTypeName(NetworkInterfaceType type) {
  switch (type) {
    case NetworkInterfaceType::Ethernet:   return "Ethernet";
    case NetworkInterfaceType::Wifi:       return "Wifi";
    case NetworkInterfaceType::Loopback:   return "Loopback";
    case NetworkInterfaceType::Tunnel:     return "Tunnel";
    case NetworkInterfaceType::P2P:        return "P2P";
    case NetworkInterfaceType::Bridge:     return "Bridge";
    case NetworkInterfaceType::Bond:       return "Bond";
    case NetworkInterfaceType::InfiniBand: return "InfiniBand";
    case NetworkInterfaceType::Virtual:    return "Virtual";
    case NetworkInterfaceType::Unknown:    return "Unknown";
  }
  return "Unknown";
}

const char *networkInterfaceErrorName(NetworkInterfaceError error) {
  switch (error) {
    case NetworkInterfaceError::None:             return "None";
    case NetworkInterfaceError::SysfsUnavailable: return "SysfsUnavailable";
    case NetworkInterfaceError::GetifaddrsFailed: return "GetifaddrsFailed";
  }
  return "None";
}

std::string NetworkInterfaceInfo::identity() const {
  if (link.ifindex.has_value()) {
    return "idx:" + std::to_string(*link.ifindex);
  }
  return "name:" + name;
}

NetworkInterfaceType networkInterfaceTypeFromArphrd(unsigned arphrd) {
  switch (arphrd) {
    case ARPHRD_ETHER:
      return NetworkInterfaceType::Ethernet;
    case ARPHRD_IEEE80211:
      return NetworkInterfaceType::Wifi;
    case ARPHRD_LOOPBACK:
      return NetworkInterfaceType::Loopback;
    case ARPHRD_TUNNEL:
    case ARPHRD_TUNNEL6:
    case ARPHRD_SIT:
      return NetworkInterfaceType::Tunnel;
    case ARPHRD_PPP:
      return NetworkInterfaceType::P2P;
    case ARPHRD_INFINIBAND:
      return NetworkInterfaceType::InfiniBand;
    case ARPHRD_NONE:
      return NetworkInterfaceType::Virtual;
    default:
      return NetworkInterfaceType::Unknown;
  }
}

NetworkInterfaceType classifyInterface(const std::string &name,
                                       unsigned arphrd) {
  const NetworkInterfaceType by_arphrd = networkInterfaceTypeFromArphrd(arphrd);
  if (name == "lo") {
    return NetworkInterfaceType::Loopback;
  }
  // Unambiguous ARPHRD values win; only ARPHRD_ETHER (and Unknown) need the
  // name-based refinement because real NICs, bridges, bonds and synthetic
  // veths all report the same Ethernet ARPHRD value.
  if (by_arphrd != NetworkInterfaceType::Ethernet &&
      by_arphrd != NetworkInterfaceType::Unknown) {
    return by_arphrd;
  }

  static constexpr std::array kTunnel{
      "vxlan", "gre", "gretap", "erspan", "sit", "tun", "tap",
      "tunl",  "gif", "wg",     "teql",   "ipip", "ip6tnl"};
  for (const char *prefix : kTunnel) {
    if (startsWith(name, prefix)) {
      return NetworkInterfaceType::Tunnel;
    }
  }
  static constexpr std::array kVirtual{
      "veth", "dummy", "docker", "virbr", "br-", "vnet", "vrf",
      "vcan", "ifb",    "podman", "vbox",  "vtap"};
  for (const char *prefix : kVirtual) {
    if (startsWith(name, prefix)) {
      return NetworkInterfaceType::Virtual;
    }
  }
  if (startsWith(name, "wlan") || startsWith(name, "wlp") ||
      startsWith(name, "wlo") || startsWith(name, "wwan")) {
    return NetworkInterfaceType::Wifi;
  }
  if (startsWith(name, "bond")) {
    return NetworkInterfaceType::Bond;
  }
  if (name.size() > 2 && startsWith(name, "br") && name[2] >= '0' &&
      name[2] <= '9') {
    return NetworkInterfaceType::Bridge;
  }
  return by_arphrd;  // Ethernet for ARPHRD_ETHER, Unknown otherwise
}

std::optional<int> parseSysfsInt(const std::string &text) {
  int value = 0;
  if (!parseDecimalInt(text, value)) {
    return std::nullopt;
  }
  if (value < 0) {
    return std::nullopt;  // "-1" is the kernel's "no value" sentinel
  }
  return value;
}

std::optional<unsigned> parseSysfsFlags(const std::string &text) {
  if (text.empty()) {
    return std::nullopt;
  }
  std::size_t index = 0;
  const bool hex = text.size() > 2 && text[0] == '0' &&
                   (text[1] == 'x' || text[1] == 'X');
  const unsigned base = hex ? 16u : 10u;
  if (hex) {
    index = 2;
  }
  uint64_t value = 0;
  bool any = false;
  constexpr uint64_t kMax = std::numeric_limits<unsigned>::max();
  for (; index < text.size(); ++index) {
    const char c = text[index];
    unsigned digit = 0;
    if (c >= '0' && c <= '9') {
      digit = static_cast<unsigned>(c - '0');
    } else if (hex && c >= 'a' && c <= 'f') {
      digit = static_cast<unsigned>(c - 'a' + 10);
    } else if (hex && c >= 'A' && c <= 'F') {
      digit = static_cast<unsigned>(c - 'A' + 10);
    } else {
      return std::nullopt;
    }
    any = true;
    if (value > (kMax - digit) / base) {
      return std::nullopt;  // overflow
    }
    value = value * base + digit;
  }
  if (!any) {
    return std::nullopt;
  }
  return static_cast<unsigned>(value);
}

std::string formatNetworkSpeed(const std::optional<int> &mbps) {
  if (!mbps.has_value() || *mbps <= 0) {
    return "unavailable";  // -1/0/file-missing all mean "no reported speed"
  }
  const int value = *mbps;
  if (value < 1000) {
    return std::to_string(value) + " Mb/s";
  }
  std::ostringstream out;
  out << std::fixed << std::setprecision(1)
      << (static_cast<double>(value) / 1000.0);
  std::string speed = out.str();
  if (speed.size() >= 2 && speed.rfind(".0") == speed.size() - 2) {
    speed.resize(speed.size() - 2);
  }
  return speed + " Gb/s";
}

std::string formatNetworkDuplex(const std::optional<std::string> &duplex) {
  if (!duplex.has_value() || duplex->empty()) {
    return "-";
  }
  return *duplex;
}

std::string formatNetworkCarrier(const std::optional<int> &carrier) {
  if (!carrier.has_value()) {
    return "-";  // virtual interfaces often expose no carrier
  }
  return *carrier == 0 ? "no" : "yes";
}

std::string formatInterfaceFlagNames(unsigned flags) {
  struct FlagEntry {
    unsigned bit;
    const char *name;
  };
  const FlagEntry entries[] = {
      {IFF_UP, "UP"},
      {IFF_BROADCAST, "BROADCAST"},
      {IFF_DEBUG, "DEBUG"},
      {IFF_LOOPBACK, "LOOPBACK"},
      {IFF_POINTOPOINT, "POINTOPOINT"},
      {IFF_RUNNING, "RUNNING"},
      {IFF_NOARP, "NOARP"},
      {IFF_PROMISC, "PROMISC"},
      {IFF_ALLMULTI, "ALLMULTI"},
      {IFF_MASTER, "MASTER"},
      {IFF_SLAVE, "SLAVE"},
      {IFF_MULTICAST, "MULTICAST"},
      {IFF_DYNAMIC, "DYNAMIC"},
#ifdef IFF_LOWER_UP
      {IFF_LOWER_UP, "LOWER_UP"},
#endif
#ifdef IFF_DORMANT
      {IFF_DORMANT, "DORMANT"},
#endif
#ifdef IFF_ECHO
      {IFF_ECHO, "ECHO"},
#endif
  };
  std::string out;
  for (const FlagEntry &entry : entries) {
    if ((flags & entry.bit) != 0) {
      if (!out.empty()) {
        out += ' ';
      }
      out += entry.name;
    }
  }
  return out;
}

std::string formatInterfaceFlagsRaw(const std::optional<unsigned> &flags) {
  if (!flags.has_value()) {
    return "";
  }
  std::ostringstream out;
  out << std::hex << std::showbase << *flags;
  return out.str();
}

std::optional<int> prefixLengthFromMask(const sockaddr *netmask,
                                        socklen_t length) {
  if (netmask == nullptr) {
    return std::nullopt;
  }
  if (netmask->sa_family == AF_INET && length >= sizeof(sockaddr_in)) {
    const auto *mask = reinterpret_cast<const sockaddr_in *>(netmask);
    const auto *bytes =
        reinterpret_cast<const unsigned char *>(&mask->sin_addr);
    return contiguousPrefixLength(bytes, 4);
  }
  if (netmask->sa_family == AF_INET6 && length >= sizeof(sockaddr_in6)) {
    const auto *mask = reinterpret_cast<const sockaddr_in6 *>(netmask);
    const auto *bytes =
        reinterpret_cast<const unsigned char *>(&mask->sin6_addr);
    return contiguousPrefixLength(bytes, 16);
  }
  return std::nullopt;
}

std::optional<std::string> formatSockaddrAddress(const sockaddr *address,
                                                 socklen_t length) {
  if (address == nullptr) {
    return std::nullopt;
  }
  char buffer[INET6_ADDRSTRLEN] = {};
  if (address->sa_family == AF_INET && length >= sizeof(sockaddr_in)) {
    const auto *src = reinterpret_cast<const sockaddr_in *>(address);
    if (::inet_ntop(AF_INET, &src->sin_addr, buffer, sizeof(buffer)) ==
        nullptr) {
      return std::nullopt;
    }
    return std::string(buffer);
  }
  if (address->sa_family == AF_INET6 && length >= sizeof(sockaddr_in6)) {
    const auto *src = reinterpret_cast<const sockaddr_in6 *>(address);
    if (::inet_ntop(AF_INET6, &src->sin6_addr, buffer, sizeof(buffer)) ==
        nullptr) {
      return std::nullopt;
    }
    return std::string(buffer);
  }
  return std::nullopt;  // unsupported family — not an error
}

bool addAddressDeduplicated(std::vector<NetworkAddressInfo> &addresses,
                            const NetworkAddressInfo &address) {
  for (const NetworkAddressInfo &existing : addresses) {
    if (existing.family == address.family &&
        existing.address == address.address) {
      return false;  // duplicate (family,address) — already present
    }
  }
  addresses.push_back(address);
  return true;
}

NetworkInterfaceLink readLink(const std::string &iface) {
  NetworkInterfaceLink link;
  if (const std::optional<std::string> text = readSysfsText(iface, "ifindex")) {
    int value = 0;
    if (parseDecimalInt(*text, value) && value > 0) {
      link.ifindex = value;
    }
  }
  if (const std::optional<std::string> text = readSysfsText(iface, "type")) {
    unsigned value = 0;
    if (parseDecimalU32(*text, value)) {
      link.link_type = value;
    }
  }
  if (const std::optional<std::string> text = readSysfsText(iface, "address")) {
    link.mac_address = *text;
  }
  if (const std::optional<std::string> text = readSysfsText(iface, "mtu")) {
    int value = 0;
    if (parseDecimalInt(*text, value) && value >= 0) {
      link.mtu = value;
    }
  }
  link.operstate = readSysfsText(iface, "operstate");  // may be "unknown"
  if (const std::optional<std::string> text = readSysfsText(iface, "carrier")) {
    int value = 0;
    if (parseDecimalInt(*text, value) && (value == 0 || value == 1)) {
      link.carrier = value;
    }
  }
  if (const std::optional<std::string> text = readSysfsText(iface, "speed")) {
    int value = 0;
    if (parseDecimalInt(*text, value) && value > 0) {
      link.speed_mbps = value;  // 0/-1 = no reported speed, stays unavailable
    }
  }
  link.duplex = readSysfsText(iface, "duplex");
  if (const std::optional<std::string> text = readSysfsText(iface, "flags")) {
    link.flags = parseSysfsFlags(*text);
  }
  if (link.flags.has_value()) {
    link.admin_up = (*link.flags & IFF_UP) != 0;
  }
  return link;
}

WirelessInfo readWirelessInfo(const std::string &iface) {
  WirelessInfo wireless;
  if (::access(("/sys/class/net/" + iface + "/wireless").c_str(), F_OK) != 0) {
    return wireless;  // not wireless-capable
  }
  wireless.present = true;
  wireless.link = readWirelessInt(iface, "wireless/link");
  wireless.level = readWirelessInt(iface, "wireless/level");
  wireless.noise = readWirelessInt(iface, "wireless/noise");
  return wireless;
}

std::unordered_map<std::string, std::vector<NetworkAddressInfo>>
collectAddresses(bool &ok) {
  std::unordered_map<std::string, std::vector<NetworkAddressInfo>> by_name;
  struct ifaddrs *ifaddr = nullptr;
  if (::getifaddrs(&ifaddr) != 0) {
    ok = false;
    return by_name;
  }
  ok = true;
  for (struct ifaddrs *ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
    if (ifa->ifa_name == nullptr || ifa->ifa_name[0] == '\0' ||
        ifa->ifa_addr == nullptr) {
      continue;  // no name or no address — nothing to record
    }
    const int family = ifa->ifa_addr->sa_family;
    if (family != AF_INET && family != AF_INET6) {
      continue;  // unsupported family — skipped, never fatal
    }
    const socklen_t length =
        family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);

    NetworkAddressInfo address;
    address.family = family;
    const std::optional<std::string> formatted =
        formatSockaddrAddress(ifa->ifa_addr, length);
    if (!formatted.has_value()) {
      continue;  // invalid address — inet_ntop failed
    }
    address.address = *formatted;

    if (ifa->ifa_netmask != nullptr) {
      address.netmask =
          formatSockaddrAddress(ifa->ifa_netmask, length);
      address.prefix_length = prefixLengthFromMask(ifa->ifa_netmask, length);
    }
    if (family == AF_INET) {
      if ((ifa->ifa_flags & IFF_BROADCAST) != 0 &&
          ifa->ifa_ifu.ifu_broadaddr != nullptr) {
        address.broadcast =
            formatSockaddrAddress(ifa->ifa_ifu.ifu_broadaddr, length);
      } else if ((ifa->ifa_flags & IFF_POINTOPOINT) != 0 &&
                 ifa->ifa_ifu.ifu_dstaddr != nullptr) {
        address.broadcast =
            formatSockaddrAddress(ifa->ifa_ifu.ifu_dstaddr, length);
      }
    }

    std::vector<NetworkAddressInfo> &list = by_name[ifa->ifa_name];
    if (!address.address.empty()) {
      (void)addAddressDeduplicated(list, address);
    }
  }
  ::freeifaddrs(ifaddr);
  return by_name;
}

NetworkInterfaceSnapshot NetworkInterfaceMonitor::read(
    const NetworkSnapshot &traffic) {
  NetworkInterfaceSnapshot snapshot;
  snapshot.refreshed_at = std::chrono::system_clock::now();

  std::string dir_error;
  std::vector<std::string> names = listNetworkInterfaces(dir_error);
  if (names.empty() && !dir_error.empty()) {
    snapshot.sysfs_readable = false;
    snapshot.error = NetworkInterfaceError::SysfsUnavailable;
    snapshot.error_detail = "cannot list /sys/class/net: " + dir_error;
    current_ = snapshot;
    return snapshot;  // no interface can be described
  }

  bool addresses_ok = false;
  const std::unordered_map<std::string, std::vector<NetworkAddressInfo>>
      addresses = collectAddresses(addresses_ok);
  snapshot.addresses_readable = addresses_ok;
  if (!addresses_ok) {
    snapshot.error = NetworkInterfaceError::GetifaddrsFailed;
    snapshot.error_detail =
        "getifaddrs() failed: " + std::string(std::strerror(errno));
  }

  std::unordered_map<std::string, NetworkInterfaceStats> traffic_by_name;
  mergeTrafficMap(traffic, traffic_by_name);

  snapshot.interfaces.reserve(names.size());
  for (const std::string &name : names) {
    NetworkInterfaceInfo info;
    info.name = name;
    info.refreshed_at = snapshot.refreshed_at;
    info.link = readLink(name);
    info.type = classifyInterface(
        name, info.link.link_type.value_or(0));
    info.wireless = readWirelessInfo(name);

    const auto found = addresses.find(name);
    if (found != addresses.end()) {
      info.addresses = found->second;
    }
    const auto traffic_hit = traffic_by_name.find(name);
    if (traffic_hit != traffic_by_name.end()) {
      info.traffic = traffic_hit->second;
    }
    if (!addresses_ok) {
      info.error = "addresses unavailable: " + snapshot.error_detail;
    }
    snapshot.interfaces.push_back(std::move(info));
  }

  std::sort(snapshot.interfaces.begin(), snapshot.interfaces.end(),
            [](const NetworkInterfaceInfo &a, const NetworkInterfaceInfo &b) {
              const bool a_loop = isLoopback(a);
              const bool b_loop = isLoopback(b);
              if (a_loop != b_loop) {
                return !a_loop;  // non-loopback first, like the traffic table
              }
              const int a_idx = a.link.ifindex.value_or(-1);
              const int b_idx = b.link.ifindex.value_or(-1);
              if (a_idx != b_idx) {
                return a_idx < b_idx;
              }
              return a.name < b.name;
            });

  current_ = snapshot;
  recordHistory(current_);
  return snapshot;
}

void NetworkInterfaceMonitor::recordHistory(
    const NetworkInterfaceSnapshot &snapshot) {
  if (history_paused_) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  std::vector<std::string> present_identities;
  present_identities.reserve(snapshot.interfaces.size());

  for (const NetworkInterfaceInfo &info : snapshot.interfaces) {
    const std::string identity = info.identity();
    present_identities.push_back(identity);
    if (!info.traffic.has_value()) {
      continue;  // no traffic table entry — nothing to plot this refresh
    }

    auto it = histories_.find(identity);
    if (it == histories_.end()) {
      if (histories_.size() >= kMaxTrackedInterfaceSeries) {
        continue;  // bound the number of series on hosts with many interfaces
      }
      InterfaceHistory history;
      history.identity = identity;
      history.rx_bytes_per_second =
          ResourceHistory<TimedSample>(max_samples_);
      history.tx_bytes_per_second =
          ResourceHistory<TimedSample>(max_samples_);
      history.rx_errors = ResourceHistory<TimedSample>(max_samples_);
      history.tx_errors = ResourceHistory<TimedSample>(max_samples_);
      history.rx_dropped = ResourceHistory<TimedSample>(max_samples_);
      history.tx_dropped = ResourceHistory<TimedSample>(max_samples_);
      it = histories_.emplace(identity, std::move(history)).first;
    }

    // One sample per metric per refresh; a renamed interface keeps its series
    // (same ifindex identity) while a recreated one starts fresh.
    it->second.rx_bytes_per_second.addSample(
        TimedSample{now, static_cast<double>(
                             info.traffic->rx_bytes_per_second)});
    it->second.tx_bytes_per_second.addSample(
        TimedSample{now, static_cast<double>(
                             info.traffic->tx_bytes_per_second)});
    it->second.rx_errors.addSample(
        TimedSample{now, static_cast<double>(info.traffic->rx_errors)});
    it->second.tx_errors.addSample(
        TimedSample{now, static_cast<double>(info.traffic->tx_errors)});
    it->second.rx_dropped.addSample(
        TimedSample{now, static_cast<double>(info.traffic->rx_dropped)});
    it->second.tx_dropped.addSample(
        TimedSample{now, static_cast<double>(info.traffic->tx_dropped)});
  }

  pruneHistory(present_identities);
}

void NetworkInterfaceMonitor::pruneHistory(
    const std::vector<std::string> &present_identities) {
  std::vector<std::string> vanished;
  for (const auto &[identity, history] : histories_) {
    (void)history;
    if (std::find(present_identities.begin(), present_identities.end(),
                  identity) == present_identities.end()) {
      vanished.push_back(identity);
    }
  }
  for (const std::string &identity : vanished) {
    histories_.erase(identity);
  }
}

const InterfaceHistory *NetworkInterfaceMonitor::historyFor(
    const std::string &identity) const {
  const auto it = histories_.find(identity);
  return it == histories_.end() ? nullptr : &it->second;
}

void NetworkInterfaceMonitor::setHistoryMaxSamples(std::size_t max_samples) {
  if (max_samples == 0) {
    max_samples = 1;  // never allow an unbounded/invalid ring
  }
  if (max_samples == max_samples_) {
    return;
  }
  max_samples_ = max_samples;
  for (auto &[identity, history] : histories_) {
    (void)identity;
    history.rx_bytes_per_second =
        ResourceHistory<TimedSample>(max_samples_);
    history.tx_bytes_per_second =
        ResourceHistory<TimedSample>(max_samples_);
    history.rx_errors = ResourceHistory<TimedSample>(max_samples_);
    history.tx_errors = ResourceHistory<TimedSample>(max_samples_);
    history.rx_dropped = ResourceHistory<TimedSample>(max_samples_);
    history.tx_dropped = ResourceHistory<TimedSample>(max_samples_);
  }
}

}  // namespace atm