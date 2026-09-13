#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "network_interface_details.hpp"
#include "network_link_state.hpp"

namespace atm {

/// Strongly-typed duplex mode of a negotiated link, parsed from the sysfs
/// "duplex" file. `Unknown` covers "unknown", missing files and unrecognised
/// values — never a fabricated "half".
enum class NetworkDuplexMode { Unknown, Half, Full };

/// Availability state of an interface's reported link speed.
enum class NetworkSpeedState {
  Unknown,     // no sample has been absorbed for this identity yet
  Valid,       // a fresh speed was reported while the link is active
  Stale,       // last-known value preserved, but the link is not active now
  Unavailable, // no usable speed (virtual links, never reported a value)
};

/// Availability state of an interface's reported duplex mode.
enum class NetworkDuplexState {
  Unknown,     // no sample has been absorbed for this identity yet
  Valid,       // a fresh duplex was reported while the link is active
  Stale,       // last-known value preserved, but the link is not active now
  Unavailable, // no usable duplex (virtual links, never reported a value)
};

[[nodiscard]] const char *networkDuplexModeName(NetworkDuplexMode mode);
[[nodiscard]] const char *networkSpeedStateName(NetworkSpeedState state);
[[nodiscard]] const char *networkDuplexStateName(NetworkDuplexState state);

/// Parses the raw sysfs duplex value. "full" maps to Full, "half" to Half and
/// anything else (missing, "unknown", empty, syntactically invalid) to Unknown.
[[nodiscard]] NetworkDuplexMode parseNetworkDuplex(
    const std::optional<std::string> &duplex);

/// One interface's link speed/duplex metrics for one read. Only physical links
/// (Ethernet/Wi-Fi/InfiniBand) carry a meaningful negotiated speed and duplex;
/// virtual, tunnel, bond and loopback interfaces are always reported
/// unavailable (never a stale firmware value). A temporary read failure
/// preserves the last successfully reported value and marks it stale.
struct NetworkLinkMetrics {
  std::string identity;          // stable key ("idx:<ifindex>" / "name:<name>")
  std::string name;              // current kernel name
  bool present = true;           // false once the identity has disappeared
  bool physical = false;         // isPhysicalNetworkLink(type)
  bool link_active = false;      // availability == Connected on this read

  NetworkSpeedState speed_state = NetworkSpeedState::Unknown;
  std::optional<int> speed_mbps;  // last successfully reported value, if any

  NetworkDuplexState duplex_state = NetworkDuplexState::Unknown;
  NetworkDuplexMode duplex = NetworkDuplexMode::Unknown;

  // Last wall-clock read that produced a fresh speed or duplex value.
  std::chrono::system_clock::time_point last_update{};
  std::chrono::steady_clock::time_point first_seen{};
};

/// Derives the metrics for one interface from its raw discovery info plus the
/// previously retained metrics. Pure and testable: no syscalls, no global state.
[[nodiscard]] NetworkLinkMetrics updateNetworkLinkMetrics(
    const NetworkLinkMetrics &previous, const NetworkInterfaceInfo &info,
    bool link_active, std::chrono::system_clock::time_point refreshed_at);

/// Monitors per-interface link speed and duplex mode.
///
/// Fed from the existing NetworkInterfaceMonitor snapshot exactly once per
/// tick by the application's monitoring loop (no second polling loop). Tracks
/// per-identity state across renames using the stable identity key, preserves
/// the last valid value on temporary read failures (marking it stale),
/// resets the state for a recreated interface (new ifindex) and retains a
/// bounded set of gone identities so a temporary removal is not misreported.
class NetworkLinkMetricsMonitor {
 public:
  static constexpr std::size_t kMaxTrackedLinkMetricsInterfaces = 64;

  NetworkLinkMetricsMonitor() = default;
  ~NetworkLinkMetricsMonitor() = default;

  NetworkLinkMetricsMonitor(const NetworkLinkMetricsMonitor &) = delete;
  NetworkLinkMetricsMonitor &operator=(const NetworkLinkMetricsMonitor &) =
      delete;

  /// Processes one discovery snapshot (produced by NetworkInterfaceMonitor).
  void update(const NetworkInterfaceSnapshot &snapshot);

  /// Latest metrics for an identity; nullptr when never seen.
  [[nodiscard]] const NetworkLinkMetrics *tracked(
      const std::string &identity) const;

  /// All retained metrics (present and gone identities).
  [[nodiscard]] const std::unordered_map<std::string, NetworkLinkMetrics> &
  entries() const {
    return tracked_;
  }

  void reset();

 private:
  std::unordered_map<std::string, NetworkLinkMetrics> tracked_;
  void evictOverflow();
};

}  // namespace atm