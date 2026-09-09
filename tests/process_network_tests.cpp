#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "process_network.hpp"
#include "process_scheduling.hpp"

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

int main() {
  using namespace atm;

  // --- TCP parsing (IPv4, /proc/net/tcp word layout). --------------------
  run("parseProcNetLine: ESTABLISHED IPv4 loopback");
  {
    std::string local;
    std::string remote;
    std::uint16_t lport = 0;
    std::uint16_t rport = 0;
    std::optional<TcpState> state;
    const char *line =
        "   0: 0100007F:8080 0100007F:D002 01 00000000:00000000 "
        "00:00000000 00000000 1000 00000000 12345";
    const auto inode = parseProcNetLine(line, false, local, remote, lport,
                                        rport, state, false);
    CHECK(inode.has_value());
    CHECK(*inode == 12345);
    CHECK(local == "127.0.0.1");
    CHECK(remote == "127.0.0.1");
    CHECK(lport == 0x8080);
    CHECK(rport == 0xD002);
    CHECK(state.has_value());
    CHECK(*state == TcpState::Established);
  }

  run("parseProcNetLine: LISTEN on all interfaces");
  {
    std::string local;
    std::string remote;
    std::uint16_t lport = 0;
    std::uint16_t rport = 0;
    std::optional<TcpState> state;
    const char *line =
        "   1: 00000000:0016 00000000:0000 0A 00000000:00000000 "
        "00:00000000 00000000     0 00000000 54321";
    const auto inode = parseProcNetLine(line, false, local, remote, lport,
                                        rport, state, false);
    CHECK(inode.has_value());
    CHECK(*inode == 54321);
    CHECK(local == "0.0.0.0");
    CHECK(lport == 22);
    CHECK(remote == "0.0.0.0");
    CHECK(rport == 0);
    CHECK(state.has_value());
    CHECK(*state == TcpState::Listen);
  }

  // --- TCP parsing: other state values. ----------------------------------
  run("parseProcNetLine: TIME_WAIT state");
  {
    std::string a, b;
    std::uint16_t lp = 0, rp = 0;
    std::optional<TcpState> st;
    const char *line =
        "   2: C0A80001:1BB3 C0A80002:0050 06 00000000:00000000 "
        "00:00000000 00000000 1000 00000000 999";
    const auto inode = parseProcNetLine(line, false, a, b, lp, rp, st, false);
    CHECK(inode.has_value());
    CHECK(st.has_value());
    CHECK(*st == TcpState::TimeWait);
    CHECK(a == "1.0.168.192");  // C0A80001: printed raw native-endian u32
  }

  // --- IPv6 parsing. -----------------------------------------------------
  run("parseProcNetLine: IPv6 loopback (tcp6 layout)");
  {
    std::string a, b;
    std::uint16_t lp = 0, rp = 0;
    std::optional<TcpState> st;
    const char *line =
        "   0: 00000000000000000000000001000000:1F90 "
        "00000000000000000000000000000000:0000 0A "
        "00000000:00000000 00:00000000 00000000 1000 00000000 777";
    const auto inode = parseProcNetLine(line, true, a, b, lp, rp, st, false);
    CHECK(inode.has_value());
    CHECK(*inode == 777);
    CHECK(a == "::1");
    CHECK(b == "::");
    CHECK(lp == 8080);
    CHECK(st.has_value());
    CHECK(*st == TcpState::Listen);
  }

  run("parseProcNetLine: IPv6 non-loopback");
  {
    std::string a, b;
    std::uint16_t lp = 0, rp = 0;
    std::optional<TcpState> st;
    const char *line =
        "   1: 00000000000000000000000000000000:1F90 "
        "00000000000000000000000000000000:0000 01 "
        "00000000:00000000 00:00000000 00000000 0 00000000 888";
    const auto inode = parseProcNetLine(line, true, a, b, lp, rp, st, false);
    CHECK(inode.has_value());
    CHECK(*inode == 888);
    CHECK(a == "::");
    CHECK(st.has_value());
    CHECK(*st == TcpState::Established);
  }

  // --- UDP parsing. ------------------------------------------------------
  run("parseProcNetLine: UDP (no TCP state)");
  {
    std::string a, b;
    std::uint16_t lp = 0, rp = 0;
    std::optional<TcpState> st;
    const char *line =
        "   0: 00000000:14E9 00000000:0000 07 00000000:00000000 "
        "00:00000000 00000000 0 00000000 555";
    const auto inode = parseProcNetLine(line, false, a, b, lp, rp, st, true);
    CHECK(inode.has_value());
    CHECK(*inode == 555);
    CHECK(a == "0.0.0.0");
    CHECK(lp == 5353);
    CHECK(!st.has_value());  // UDP carries no TCP state
  }

  // --- Unix domain socket parsing. ---------------------------------------
  run("parseProcNetUnixLine: bound socket with path");
  {
    int type = 0;
    int state = 0;
    std::string path;
    std::string address;
    const char *line =
        "ffff8f0000000000: 00000002 00000000 00010000 0001 01 27526 "
        "/run/example.sock";
    const auto inode = parseProcNetUnixLine(line, type, state, path, address);
    CHECK(inode.has_value());
    CHECK(*inode == 27526);
    CHECK(type == 1);  // SOCK_STREAM
    CHECK(state == 1);
    CHECK(path == "/run/example.sock");
  }

  run("parseProcNetUnixLine: unnamed abstract-style socket");
  {
    int type = 0;
    int state = 0;
    std::string path;
    std::string address;
    const char *line =
        "ffff8f0000000001: 00000002 00000000 00010000 0002 03 40000 @abstract";
    const auto inode = parseProcNetUnixLine(line, type, state, path, address);
    CHECK(inode.has_value());
    CHECK(*inode == 40000);
    CHECK(type == 2);  // SOCK_DGRAM
  }

  // --- Invalid / malformed data. -----------------------------------------
  run("parseProcNetLine: reject malformed address width");
  {
    std::string a, b;
    std::uint16_t lp = 0, rp = 0;
    std::optional<TcpState> st;
    const char *line =
        "   0: Z100007F:8080 0100007F:D002 01 00000000:00000000 "
        "00:00000000 00000000 1000 00000000 12345";
    CHECK(!parseProcNetLine(line, false, a, b, lp, rp, st, false).has_value());
  }

  run("parseProcNetLine: reject short line / missing inode");
  {
    std::string a, b;
    std::uint16_t lp = 0, rp = 0;
    std::optional<TcpState> st;
    CHECK(!parseProcNetLine("0: 0100007F:8080 0100007F:D002", false, a, b, lp,
                            rp, st, false)
               .has_value());
  }

  run("parseProcNetLine: reject invalid port");
  {
    std::string a, b;
    std::uint16_t lp = 0, rp = 0;
    std::optional<TcpState> st;
    const char *line =
        "   0: 0100007F:ZZZZ 0100007F:D002 01 00000000:00000000 "
        "00:00000000 00000000 1000 00000000 12345";
    CHECK(!parseProcNetLine(line, false, a, b, lp, rp, st, false).has_value());
  }

  run("parseProcNetLine: reject zero inode");
  {
    std::string a, b;
    std::uint16_t lp = 0, rp = 0;
    std::optional<TcpState> st;
    const char *line =
        "   0: 0100007F:8080 0100007F:D002 01 00000000:00000000 "
        "00:00000000 00000000 1000 00000000 0000";
    CHECK(!parseProcNetLine(line, false, a, b, lp, rp, st, false).has_value());
  }

  run("parseProcNetUnixLine: reject short line");
  {
    int type = 0, state = 0;
    std::string p, a;
    CHECK(!parseProcNetUnixLine("ffff8f0000000000: 00000002 00000000", type,
                                state, p, a)
               .has_value());
  }

  // --- Live integration: a real bound socket in this process. -----------
  run("manager: correlate a real socket inode to this process");
  {
    const int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(sock >= 0);
    // Bind to an ephemeral loopback port so it appears as a TCP LISTEN socket.
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    const int bound =
        ::bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0;
    const int listening =
        bound && ::listen(sock, 1) == 0 ? 1 : 0;
    if (!bound || !listening) {
      std::fprintf(stderr,
                   "SKIP: cannot bind/listen in test process (no "
                   "CAP_NET_ADMIN? loopback unavailable?)\n");
      if (sock >= 0) ::close(sock);
    } else {
      const auto self = ProcessIdentity::current(getpid());
      if (self) {
        const ProcessNetworkConnectionManager mgr;
        const ProcessNetworkConnectionsResult res = mgr.inspect(*self, 256);
        // At least the just-created LISTEN socket should be present.
        CHECK(res.success());
        bool found_listen = false;
        for (const auto &conn : res.connections) {
          if (conn.local_address == "127.0.0.1" &&
              conn.tcp_state == TcpState::Listen) {
            found_listen = true;
            break;
          }
        }
        CHECK(found_listen);
      } else {
        std::fprintf(stderr, "SKIP: cannot read own process identity\n");
      }
    }
    if (sock >= 0) ::close(sock);
  }

  run("manager: connection limit is enforced and reported as truncated");
  {
    // Open a few TCP listener sockets, then inspect with a deliberately small
    // limit. The manager must stop at the limit and set truncated.
    std::vector<int> socks;
    for (int i = 0; i < 4; ++i) {
      const int s = ::socket(AF_INET, SOCK_STREAM, 0);
      if (s < 0) break;
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      addr.sin_port = 0;
      if (::bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
          ::listen(s, 1) != 0) {
        ::close(s);
        break;
      }
      socks.push_back(s);
    }
    const auto self = ProcessIdentity::current(getpid());
    if (self) {
      const ProcessNetworkConnectionManager mgr;
      // Every process owns at least its stdout/stderr sockets, and we added
      // several more, so a limit of 1 is guaranteed to truncate.
      const ProcessNetworkConnectionsResult res = mgr.inspect(*self, 1);
      CHECK(res.success());
      CHECK(res.connections.size() == 1);
      CHECK(res.truncated);
    }
    for (int s : socks) ::close(s);
  }

  run("manager: PID reuse is detected");
  {
    const pid_t self_pid = getpid();
    const auto self = ProcessIdentity::current(self_pid);
    if (!self) {
      std::fprintf(stderr, "SKIP: cannot read own identity\n");
      return EXIT_FAILURE;
    }
    ProcessNetworkConnectionManager mgr;
    // A fabricated starttime tick that must differ from the real one, so the
    // manager must report ProcessReused instead of real connections.
    ProcessIdentity forged{self_pid, self->starttime_ticks + 1ULL};
    const ProcessNetworkConnectionsResult res = mgr.inspect(forged, 256);
    CHECK(res.status == NetworkConnectionsStatus::ProcessReused);
  }

  run("manager: invalid PID is rejected");
  {
    const ProcessNetworkConnectionManager mgr;
    const ProcessNetworkConnectionsResult res =
        mgr.inspect(ProcessIdentity{0, 1}, 256);
    CHECK(res.status == NetworkConnectionsStatus::InvalidPid);
    const ProcessNetworkConnectionsResult res_neg =
        mgr.inspect(ProcessIdentity{-5, 1}, 256);
    CHECK(res_neg.status == NetworkConnectionsStatus::InvalidPid);
  }

  run("manager: vanished process is reported, never crashes");
  {
    pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
      ::_exit(0);  // child exits immediately
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    // The process has exited; its identity is gone.
    const ProcessNetworkConnectionManager mgr;
    const ProcessNetworkConnectionsResult res =
        mgr.inspect(ProcessIdentity{child, 12345}, 256);
    // Either the identity could not be re-read (IdentityUnknown / gone) or the
    // fd dir no longer exists (ProcessNotFound). Either way: never a success
    // with connections, and never a crash.
    CHECK(res.status != NetworkConnectionsStatus::Success);
  }

  run("connectionProtocolName / tcpStateName are stable");
  CHECK(std::string(connectionProtocolName(ConnectionProtocol::Tcp4)) == "TCP");
  CHECK(std::string(connectionProtocolName(ConnectionProtocol::Udp6)) == "UDP");
  CHECK(std::string(connectionProtocolName(ConnectionProtocol::Unix)) == "Unix");
  CHECK(std::string(tcpStateName(TcpState::Listen)) == "LISTEN");
  CHECK(std::string(tcpStateName(TcpState::Established)) == "ESTABLISHED");

  std::fprintf(stderr, "\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}