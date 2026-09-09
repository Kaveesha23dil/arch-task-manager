#include "process_network.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <sys/stat.h>

#include <cerrno>
#include <charconv>
#include <climits>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>
#include <algorithm>

namespace atm {

namespace {

std::optional<std::uint64_t> parseUInt(std::string_view view, int base) {
  if (view.empty()) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  const char *begin = view.data();
  const char *end = view.data() + view.size();
  const auto result = std::from_chars(begin, end, value, base);
  if (result.ec != std::errc{} || result.ptr != end) {
    return std::nullopt;
  }
  return value;
}

std::optional<std::string> readFile(std::string_view path) {
  std::ifstream in{std::string(path)};
  if (!in.is_open()) {
    return std::nullopt;
  }
  std::ostringstream contents;
  contents << in.rdbuf();
  if (in.bad()) {
    return std::nullopt;
  }
  return contents.str();
}

/// Decodes an IPv4 kernel hex address (8 hex chars) to a human-readable string
/// like "127.0.0.1". The kernel prints the __be32 value read as a native u32,
/// so a direct memcpy into in_addr.s_addr (which stores network byte order)
/// recovers the address correctly on any platform.
std::optional<std::string> decodeIpv4Hex(std::string_view hex) {
  if (hex.size() != 8) {
    return std::nullopt;
  }
  const std::optional<std::uint64_t> val = parseUInt(hex, 16);
  if (!val || *val > 0xFFFFFFFFULL) {
    return std::nullopt;
  }
  const auto v = static_cast<std::uint32_t>(*val);
  ::in_addr addr{};
  std::memcpy(&addr.s_addr, &v, sizeof(addr.s_addr));
  char buf[INET_ADDRSTRLEN] = {};
  if (::inet_ntop(AF_INET, &addr, buf, sizeof(buf)) == nullptr) {
    return std::nullopt;
  }
  return std::string(buf);
}

/// Decodes an IPv6 kernel hex address (32 hex chars = 4 x 8-char groups).
/// The kernel prints s6_addr32[0..3] as native-endian u32 values, so a direct
/// memcpy of each decoded group into s6_addr recovers the address on any
/// platform.
std::optional<std::string> decodeIpv6Hex(std::string_view hex) {
  if (hex.size() != 32) {
    return std::nullopt;
  }
  ::in6_addr addr{};
  for (int i = 0; i < 4; ++i) {
    const std::optional<std::uint64_t> val =
        parseUInt(hex.substr(static_cast<std::size_t>(i) * 8, 8), 16);
    if (!val || *val > 0xFFFFFFFFULL) {
      return std::nullopt;
    }
    const auto v = static_cast<std::uint32_t>(*val);
    std::memcpy(addr.s6_addr + i * 4, &v, sizeof(v));
  }
  char buf[INET6_ADDRSTRLEN] = {};
  if (::inet_ntop(AF_INET6, &addr, buf, sizeof(buf)) == nullptr) {
    return std::nullopt;
  }
  return std::string(buf);
}

/// Decodes an address+port pair like "0100007F:1F90" (IPv4) or
/// "0000000000000000000000000100007F:1F90" (IPv6) into a human-readable
/// address and numeric port. The kernel hex for both address and port uses
/// network byte order printed as a native u32.
std::optional<std::pair<std::string, std::uint16_t>>
decodeAddrPort(std::string_view token, bool ipv6) {
  const std::size_t colon = token.rfind(':');
  if (colon == std::string_view::npos || colon == 0 ||
      colon + 1 >= token.size()) {
    return std::nullopt;
  }
  const std::string_view addr_hex = token.substr(0, colon);
  const std::string_view port_hex = token.substr(colon + 1);
  const std::size_t expected = ipv6 ? 32U : 8U;
  if (addr_hex.size() != expected || port_hex.size() != 4) {
    return std::nullopt;
  }
  const std::optional<std::string> addr =
      ipv6 ? decodeIpv6Hex(addr_hex) : decodeIpv4Hex(addr_hex);
  if (!addr) {
    return std::nullopt;
  }
  const std::optional<std::uint64_t> port = parseUInt(port_hex, 16);
  if (!port || *port > 0xFFFF) {
    return std::nullopt;
  }
  return std::make_pair(*addr, static_cast<std::uint16_t>(*port));
}

std::vector<std::string_view> splitWhitespace(std::string_view line) {
  std::vector<std::string_view> fields;
  std::size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() &&
           std::isspace(static_cast<unsigned char>(line[i]))) {
      ++i;
    }
    if (i >= line.size()) {
      break;
    }
    std::size_t start = i;
    while (i < line.size() &&
           !std::isspace(static_cast<unsigned char>(line[i]))) {
      ++i;
    }
    fields.push_back(line.substr(start, i - start));
  }
  return fields;
}

const char *unixStateToName(int state, bool accept_flag) {
  if (accept_flag) {
    return "LISTENING";
  }
  switch (state) {
    case 0:
      return "FREE";
    case 1:
      return "UNCONNECTED";
    case 2:
      return "CONNECTING";
    case 3:
      return "CONNECTED";
    case 4:
      return "DISCONNECTING";
    default:
      return "UNKNOWN";
  }
}

const char *unixTypeName(int type) {
  switch (type) {
    case 1:
      return "STREAM";
    case 2:
      return "DGRAM";
    case 3:
      return "RAW";
    case 4:
      return "RDM";
    case 5:
      return "SEQPACKET";
    case 6:
      return "DCCP";
    default:
      return "?";
  }
}

}  // namespace

const char *connectionProtocolName(ConnectionProtocol protocol) {
  switch (protocol) {
    case ConnectionProtocol::Tcp4:
      return "TCP";
    case ConnectionProtocol::Tcp6:
      return "TCP";
    case ConnectionProtocol::Udp4:
      return "UDP";
    case ConnectionProtocol::Udp6:
      return "UDP";
    case ConnectionProtocol::Unix:
      return "Unix";
    case ConnectionProtocol::Unknown:
    default:
      return "Unknown";
  }
}

const char *tcpStateName(TcpState state) {
  switch (state) {
    case TcpState::Established:
      return "ESTABLISHED";
    case TcpState::SynSent:
      return "SYN_SENT";
    case TcpState::SynRecv:
      return "SYN_RECV";
    case TcpState::FinWait1:
      return "FIN_WAIT1";
    case TcpState::FinWait2:
      return "FIN_WAIT2";
    case TcpState::TimeWait:
      return "TIME_WAIT";
    case TcpState::Close:
      return "CLOSE";
    case TcpState::CloseWait:
      return "CLOSE_WAIT";
    case TcpState::LastAck:
      return "LAST_ACK";
    case TcpState::Listen:
      return "LISTEN";
    case TcpState::Closing:
      return "CLOSING";
    default:
      return "?";
  }
}

std::optional<std::uint64_t>
parseProcNetLine(std::string_view line, bool ipv6, std::string &out_local_addr,
                 std::string &out_remote_addr, std::uint16_t &out_local_port,
                 std::uint16_t &out_remote_port,
                 std::optional<TcpState> &out_tcp_state, bool udp) {
  if (!line.empty() && line.back() == '\r') {
    line.remove_suffix(1);
  }
  if (line.empty()) {
    return std::nullopt;
  }
  // Skip header lines.
  if (line[0] == 's' || line[0] == ' ') {
    if (line.find("sl ") == 0 || line.find("  sl ") == 0) {
      return std::nullopt;
    }
  }

  const std::vector<std::string_view> f = splitWhitespace(line);
  if (f.size() < 10) {
    return std::nullopt;
  }

  // /proc/net/tcp layout (fields by index):
  //   [0] "0:"  (sl)
  //   [1] "0100007F:1F90"  (local_address:port)
  //   [2] "00000000:0000"  (rem_address:port)
  //   [3] "0A"  (st)
  //   [4] "00000000:00000000"  (tx_queue:rx_queue)
  //   [5] "00:00000000"  (tr:tm->when)
  //   [6] "00000000"  (retrnsmt)
  //   [7] "0"  (uid)
  //   [8] "0"  (timeout)
  //   [9] "12345"  (inode — decimal)
  // The inode is always field index 9.

  const std::optional<std::uint64_t> inode = parseUInt(f[9], 10);
  if (!inode || *inode == 0) {
    return std::nullopt;
  }

  const auto local = decodeAddrPort(f[1], ipv6);
  const auto remote = decodeAddrPort(f[2], ipv6);
  if (!local || !remote) {
    return std::nullopt;
  }
  out_local_addr = local->first;
  out_local_port = local->second;
  out_remote_addr = remote->first;
  out_remote_port = remote->second;

  // TCP state from field [3] as a hex byte (01..0B). Map the raw kernel value
  // to the named TcpState enum.
  out_tcp_state = std::nullopt;
  if (!udp) {
    const std::optional<std::uint64_t> st_val = parseUInt(f[3], 16);
    static const TcpState states[] = {
        /* 00 */ TcpState::Close,          // (unused / default)
        /* 01 */ TcpState::Established,
        /* 02 */ TcpState::SynSent,
        /* 03 */ TcpState::SynRecv,
        /* 04 */ TcpState::FinWait1,
        /* 05 */ TcpState::FinWait2,
        /* 06 */ TcpState::TimeWait,
        /* 07 */ TcpState::Close,
        /* 08 */ TcpState::CloseWait,
        /* 09 */ TcpState::LastAck,
        /* 0A */ TcpState::Listen,
        /* 0B */ TcpState::Closing,
    };
    if (st_val && *st_val < sizeof(states) / sizeof(states[0])) {
      out_tcp_state = states[static_cast<std::size_t>(*st_val)];
    }
  }

  return *inode;
}

std::optional<std::uint64_t>
parseProcNetUnixLine(std::string_view line, int &out_type, int &out_state,
                     std::string &out_path, std::string &out_address) {
  if (!line.empty() && line.back() == '\r') {
    line.remove_suffix(1);
  }
  if (line.empty()) {
    return std::nullopt;
  }
  // Skip header line.
  if (line[0] == 's' || line[0] == ' ') {
    if (line.find("Num") != std::string_view::npos) {
      return std::nullopt;
    }
  }

  const std::vector<std::string_view> f = splitWhitespace(line);
  // /proc/net/unix layout (fields by index):
  //   [0] "ffff9f0c6a5f4000:"  (Num — address:len, we ignore the value)
  //   [1] "00000002"  (RefCount)
  //   [2] "00000000"  (Protocol)
  //   [3] "00010000"  (Flags — 0x00010000 = SO_ACCEPTCONN / listening)
  //   [4] "0001"  (Type — SOCK_STREAM=1, SOCK_DGRAM=2)
  //   [5] "01"  (St — state)
  //   [6] "27526"  (Inode — decimal)
  //   [7] "/run/example.sock"  (Path — optional)
  if (f.size() < 7) {
    return std::nullopt;
  }

  const std::optional<std::uint64_t> inode = parseUInt(f[6], 10);
  if (!inode || *inode == 0) {
    return std::nullopt;
  }

  const std::optional<std::uint64_t> type_val = parseUInt(f[4], 16);
  if (!type_val) {
    return std::nullopt;
  }
  const std::optional<std::uint64_t> state_val = parseUInt(f[5], 16);
  if (!state_val) {
    return std::nullopt;
  }
  const std::optional<std::uint64_t> flags_val = parseUInt(f[3], 16);
  const bool accept_flag =
      flags_val.has_value() && (*flags_val & 0x00010000) != 0;

  out_type = static_cast<int>(*type_val);
  out_state = static_cast<int>(*state_val);

  out_path.clear();
  if (f.size() >= 8) {
    out_path = std::string(f[7]);
  }
  // Build the display address. An "@" prefix marks an abstract socket; an
  // empty path means an unnamed socket.
  if (out_path.empty()) {
    out_address = "-";
  } else {
    out_address = out_path;
  }
  // Append state as a suffix for the display string.
  out_address += " (";
  out_address += unixStateToName(out_state, accept_flag);
  out_address += ") ";
  out_address += unixTypeName(out_type);

  return *inode;
}

namespace {

/// Enumerates the process's socket file descriptors, returning (fd, inode)
/// pairs. Non-socket descriptors (regular files, pipes, ttys, etc.) are
/// ignored. Returns std::nullopt on EACCES/EPERM (permission denied) or
/// ENOENT/ESRCH (process gone).
std::optional<std::vector<std::pair<int, std::uint64_t>>>
collectSocketFds(::pid_t pid, int &out_errno) {
  out_errno = 0;
  const std::string fd_dir = "/proc/" + std::to_string(pid) + "/fd";
  DIR *dir = ::opendir(fd_dir.c_str());
  if (dir == nullptr) {
    out_errno = errno;
    return std::nullopt;
  }

  std::vector<std::pair<int, std::uint64_t>> sockets;
  char link_target[64];
  struct ::dirent *entry = nullptr;
  while ((entry = ::readdir(dir)) != nullptr) {
    const char *name = entry->d_name;
    if (name[0] == '.' &&
        (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
      continue;
    }
    // Skip non-numeric fd names (defensive).
    if (name[0] < '0' || name[0] > '9') {
      continue;
    }
    const std::optional<std::uint64_t> fd_parsed = parseUInt(name, 10);
    if (!fd_parsed ||
        *fd_parsed > static_cast<std::uint64_t>(INT_MAX)) {
      continue;
    }
    const int fd = static_cast<int>(*fd_parsed);
    const std::string link_path = fd_dir + "/" + name;
    const ssize_t len =
        ::readlink(link_path.c_str(), link_target, sizeof(link_target) - 1);
    if (len <= 0) {
      continue;  // fd may have vanished between readdir and readlink; skip.
    }
    link_target[len] = '\0';
    // Socket targets have the form "socket:[123456]".
    if (len > 8 && std::string_view(link_target).substr(0, 8) == "socket:[" &&
        link_target[len - 1] == ']') {
      const std::string_view inode_str(link_target + 8,
                                       static_cast<std::size_t>(len - 9));
      const std::optional<std::uint64_t> inode = parseUInt(inode_str, 10);
      if (inode && *inode != 0) {
        sockets.emplace_back(fd, *inode);
      }
    }
  }
  ::closedir(dir);
  // Deterministic order by fd for consistent display.
  std::sort(sockets.begin(), sockets.end(),
            [](const auto &a, const auto &b) { return a.first < b.first; });
  return sockets;
}

struct ProcNetRecord {
  ConnectionProtocol protocol;
  std::string local_addr;
  std::string remote_addr;
  std::uint16_t local_port = 0;
  std::uint16_t remote_port = 0;
  std::optional<TcpState> tcp_state;
  bool udp = false;
  // For /proc/net/unix: encoded display info.
  std::string unix_display;
  bool is_unix = false;
};

std::unordered_map<std::uint64_t, ProcNetRecord>
parseInetTable(const std::string &contents, ConnectionProtocol protocol,
               bool ipv6, bool udp) {
  std::unordered_map<std::uint64_t, ProcNetRecord> records;
  std::istringstream stream(contents);
  std::string line;
  while (std::getline(stream, line)) {
    std::string local_addr;
    std::string remote_addr;
    std::uint16_t local_port = 0;
    std::uint16_t remote_port = 0;
    std::optional<TcpState> tcp_state;
    if (const std::optional<std::uint64_t> inode =
            parseProcNetLine(line, ipv6, local_addr, remote_addr, local_port,
                             remote_port, tcp_state, udp)) {
      ProcNetRecord rec;
      rec.protocol = protocol;
      rec.local_addr = std::move(local_addr);
      rec.remote_addr = std::move(remote_addr);
      rec.local_port = local_port;
      rec.remote_port = remote_port;
      rec.tcp_state = tcp_state;
      rec.udp = udp;
      records.emplace(*inode, std::move(rec));
    }
  }
  return records;
}

std::unordered_map<std::uint64_t, ProcNetRecord>
parseUnixTable(const std::string &contents) {
  std::unordered_map<std::uint64_t, ProcNetRecord> records;
  std::istringstream stream(contents);
  std::string line;
  while (std::getline(stream, line)) {
    int type = 0;
    int state = 0;
    std::string path;
    std::string address;
    if (const std::optional<std::uint64_t> inode =
            parseProcNetUnixLine(line, type, state, path, address)) {
      ProcNetRecord rec;
      rec.protocol = ConnectionProtocol::Unix;
      rec.is_unix = true;
      rec.unix_display = std::move(address);
      records.emplace(*inode, std::move(rec));
    }
  }
  return records;
}

}  // namespace

ProcessNetworkConnectionsResult ProcessNetworkConnectionManager::inspect(
    const ProcessIdentity &identity, std::size_t max_connections) const {
  if (identity.pid <= 0) {
    return {NetworkConnectionsStatus::InvalidPid, 0, {}, false};
  }

  // Identity gate (before the read).
  std::optional<ProcessIdentity> current = ProcessIdentity::current(identity.pid);
  if (!current) {
    return {NetworkConnectionsStatus::IdentityUnknown, 0, {}, false};
  }
  if (current->starttime_ticks != identity.starttime_ticks) {
    return {NetworkConnectionsStatus::ProcessReused, 0, {}, false};
  }

  int fd_errno = 0;
  const std::optional<std::vector<std::pair<int, std::uint64_t>>> sockets =
      collectSocketFds(identity.pid, fd_errno);
  if (!sockets) {
    if (fd_errno == EACCES || fd_errno == EPERM) {
      return {NetworkConnectionsStatus::PermissionDenied, fd_errno, {}, false};
    }
    if (fd_errno == ENOENT || fd_errno == ESRCH) {
      return {NetworkConnectionsStatus::ProcessNotFound, fd_errno, {}, false};
    }
    return {NetworkConnectionsStatus::ReadError, fd_errno, {}, false};
  }

  // Parse the relevant /proc/net/* tables and build an inode → record map.
  std::unordered_map<std::uint64_t, ProcNetRecord> index;

  auto merge = [&index](const std::string &path, ConnectionProtocol protocol,
                        bool ipv6, bool udp) {
    const std::optional<std::string> contents = readFile(path);
    if (!contents) {
      return;
    }
    std::unordered_map<std::uint64_t, ProcNetRecord> parsed =
        parseInetTable(*contents, protocol, ipv6, udp);
    for (auto &kv : parsed) {
      index.emplace(kv.first, std::move(kv.second));
    }
  };
  merge("/proc/net/tcp", ConnectionProtocol::Tcp4, false, false);
  merge("/proc/net/tcp6", ConnectionProtocol::Tcp6, true, false);
  merge("/proc/net/udp", ConnectionProtocol::Udp4, false, true);
  merge("/proc/net/udp6", ConnectionProtocol::Udp6, true, true);

  // Unix domain sockets.
  if (const std::optional<std::string> unix_contents = readFile("/proc/net/unix");
      unix_contents) {
    std::unordered_map<std::uint64_t, ProcNetRecord> unix_records =
        parseUnixTable(*unix_contents);
    for (auto &kv : unix_records) {
      index.emplace(kv.first, std::move(kv.second));
    }
  }

  ProcessNetworkConnectionsResult result;
  result.status = NetworkConnectionsStatus::Success;

  // Correlate each socket fd + inode with its protocol table entry. Maintain a
  // deterministic display order (fd ascending).
  for (const auto &[fd, inode_number] : *sockets) {
    if (result.connections.size() >= max_connections) {
      result.truncated = true;
      break;
    }
    const auto it = index.find(inode_number);
    if (it == index.end()) {
      // A socket inode not found in any protocol table: represent it as
      // unknown without fabricating address/port information.
      ProcessNetworkConnection conn;
      conn.fd = fd;
      conn.inode = inode_number;
      conn.protocol = ConnectionProtocol::Unknown;
      result.connections.push_back(std::move(conn));
      continue;
    }
    const ProcNetRecord &record = it->second;
    ProcessNetworkConnection conn;
    conn.fd = fd;
    conn.inode = inode_number;
    conn.protocol = record.protocol;
    conn.udp = record.udp;
    conn.local_port = record.local_port;
    conn.remote_port = record.remote_port;
    if (record.is_unix) {
      conn.local_address = record.unix_display;
    } else {
      conn.local_address = record.local_addr;
      conn.remote_address = record.remote_addr;
    }
    conn.tcp_state = record.tcp_state;

    switch (record.protocol) {
      case ConnectionProtocol::Tcp4:
      case ConnectionProtocol::Tcp6:
        ++result.tcp_count;
        if (conn.tcp_state == TcpState::Listen) {
          ++result.listening_count;
        } else if (conn.tcp_state == TcpState::Established) {
          ++result.established_count;
        }
        break;
      case ConnectionProtocol::Udp4:
      case ConnectionProtocol::Udp6:
        ++result.udp_count;
        break;
      case ConnectionProtocol::Unix:
        ++result.unix_count;
        break;
      default:
        break;
    }
    result.connections.push_back(std::move(conn));
  }

  // Identity gate (after the read): discard a stale result if the PID was
  // reused while we were inspecting.
  current = ProcessIdentity::current(identity.pid);
  if (!current) {
    return {NetworkConnectionsStatus::IdentityUnknown, 0, {}, false};
  }
  if (current->starttime_ticks != identity.starttime_ticks) {
    return {NetworkConnectionsStatus::ProcessReused, 0, {}, false};
  }
  return result;
}

}  // namespace atm