#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "process_scheduling.hpp"

namespace atm {

/// Upper bound on the number of connection records collected for one process in
/// a single inspection. A heavily-socketed process is reported as truncated
/// instead of causing unbounded allocations.
constexpr std::size_t kDefaultMaxConnections = 1024;

/// Outcome of reading a process's network connections.
enum class NetworkConnectionsStatus {
  Success,          // connections were read (possibly zero, possibly truncated)
  InvalidPid,       // pid <= 0, rejected before any system access
  ProcessNotFound,  // /proc/<pid> is absent (the process has exited / ESRCH)
  IdentityUnknown,  // the process identity could not be re-read before applying
  ProcessReused,    // the PID was reused by a different process since selection
  PermissionDenied, // EACCES / EPERM while opening /proc/<pid>/fd
  ReadError,        // any other failure reading the needed /proc sources
};

/// The transport protocol / socket family of one connection.
enum class ConnectionProtocol {
  Tcp4,
  Tcp6,
  Udp4,
  Udp6,
  Unix,
  Unknown,  // a socket whose inode matched no /proc/net/* table
};

/// Human-readable, stable labels for the protocols in ConnectionProtocol.
[[nodiscard]] const char *connectionProtocolName(ConnectionProtocol protocol);

/// TCP socket state, decoded from the Linux /proc/net/tcp st field.
enum class TcpState {
  Established,
  SynSent,
  SynRecv,
  FinWait1,
  FinWait2,
  TimeWait,
  Close,
  CloseWait,
  LastAck,
  Listen,
  Closing,
};

/// Human-readable, stable labels for the TcpState values.
[[nodiscard]] const char *tcpStateName(TcpState state);

/// One network connection belonging to the inspected process. Everything here
/// is read-only metadata derived from /proc/<pid>/fd and /proc/net/* — a socket
/// is never modified, closed, sent to, or intercepted.
struct ProcessNetworkConnection {
  int fd = 0;                      // the file descriptor in the process
  ConnectionProtocol protocol = ConnectionProtocol::Unknown;
  std::string local_address;       // numeric IPv4/IPv6 or socket path; "" when none
  std::string remote_address;      // numeric IPv4/IPv6; "" when not applicable
  std::uint16_t local_port = 0;    // 0 when not applicable (e.g. Unix socket)
  std::uint16_t remote_port = 0;   // 0 when not applicable
  std::optional<TcpState> tcp_state;  // set only for TCP sockets
  std::uint64_t inode = 0;         // the socket inode from socket:[inode]
  bool udp = false;                // true for UDP sockets (unconnected / dummy)
};

/// Result of one connection inspection, including lightweight summary counters.
struct ProcessNetworkConnectionsResult {
  NetworkConnectionsStatus status = NetworkConnectionsStatus::ReadError;
  int errno_value = 0;  // original errno for PermissionDenied / ReadError
  std::vector<ProcessNetworkConnection> connections;
  bool truncated = false;  // true when kDefaultMaxConnections was reached

  [[nodiscard]] bool success() const {
    return status == NetworkConnectionsStatus::Success;
  }

  // Summary counters. A TCP socket that is LISTEN gets counted in
  // listening_count, not established_count; UDP sockets are never counted as
  // established or listening (they are unconnected by nature).
  std::size_t tcp_count = 0;
  std::size_t udp_count = 0;
  std::size_t unix_count = 0;
  std::size_t listening_count = 0;
  std::size_t established_count = 0;
};

/**
 * Parses a single /proc/net/tcp, /proc/net/tcp6, /proc/net/udp or
 * /proc/net/udp6 data line into an inode-to-connection index entry. The kernel
 * represents addresses as 8-digit hex words (4 for IPv4, 8 for IPv6) and the
 * port as 4 hex digits. Malformed lines yield std::nullopt instead of crashing.
 * `ipv6` selects the tcp6/udp6 word layout.
 */
[[nodiscard]] std::optional<std::uint64_t>
parseProcNetLine(std::string_view line, bool ipv6, std::string &out_local_addr,
                 std::string &out_remote_addr, std::uint16_t &out_local_port,
                 std::uint16_t &out_remote_port,
                 std::optional<TcpState> &out_tcp_state, bool udp);

/// Parses a /proc/net/unix data line, returning the (type, state, pathname,
/// inode) tuple on success.
[[nodiscard]] std::optional<std::uint64_t>
parseProcNetUnixLine(std::string_view line, int &out_type, int &out_state,
                     std::string &out_path, std::string &out_address);

/**
 * Read-only network connection inspector for one selected process.
 *
 * It enumerates /proc/<pid>/fd, keeps only socket: descriptors, extracts their
 * inode numbers, and correlates them against the matching /proc/net/ tables to
 * build structured, read-only connection records. Only the selected process's
 * descriptors are enumerated and only the relevant /proc/net/ tables are read
 * once per inspection. No external command (ss, netstat, lsof ...) is used and
 * no socket is ever modified.
 *
 * Every inspection is gated by the supplied ProcessIdentity (PID + kernel
 * start-time tick) both immediately before and immediately after the read, and
 * the result is discarded when the running process is not the one the caller
 * selected — so stale results can never be shown under a newer selection of the
 * same PID.
 */
class ProcessNetworkConnectionManager {
 public:
  ProcessNetworkConnectionManager() = default;
  ~ProcessNetworkConnectionManager() = default;

  // Stateless wrapper object; copy/move are harmless.
  ProcessNetworkConnectionManager(const ProcessNetworkConnectionManager &) =
      default;
  ProcessNetworkConnectionManager &operator=(
      const ProcessNetworkConnectionManager &) = default;

  [[nodiscard]] ProcessNetworkConnectionsResult
  inspect(const ProcessIdentity &identity,
          std::size_t max_connections = kDefaultMaxConnections) const;
};

}  // namespace atm