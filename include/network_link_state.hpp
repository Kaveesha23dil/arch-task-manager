#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "alert_manager.hpp"
#include "network_interface_details.hpp"
#include "resource_history.hpp"

namespace atm {

/// Kernel operational state of a network interface (the sysfs "operstate"
/// value). `Unavailable` is synthetic and reserved for the derived
/// availability: a temporary read failure means "not readable right now",
/// never "down".
enum class NetworkOperState {
  Unknown,         // "unknown" or an unrecognised value
  Up,              // "up" — the kernel considers the link layer operational
  Down,            // "down"
  Dormant,         // "dormant"
  LowerLayerDown,  // "lowerlayerdown" — a lower (driver/phy) layer is down
  Testing,         // "testing"
  NotPresent,      // "notpresent" — the device is not present on the bus
  Unavailable,     // synthetic: link information could not be read
};

/// Carrier presence of an interface (the sysfs "carrier" file).
enum class NetworkCarrierState {
  Unknown,   // no carrier value exposed by the driver
  NoCarrier, // carrier == 0
  Carrier,   // carrier == 1
};

/// Administrative state, derived from IFF_UP in the sysfs flags.
enum class NetworkAdminState {
  Unknown,   // flags not exposed
  AdminDown, // IFF_UP clear
  AdminUp,   // IFF_UP set
};

/// Derived, user-facing availability of one interface's link. Distinct from
/// the raw sub-states: it answers "is there a usable link right now?" while
/// remaining honest that operstate "up" does not imply internet connectivity.
enum class NetworkLinkAvailability {
  Unknown,     // present but no state has been reported yet
  Connected,   // operationally up (kernel "up", or carrier signals a link)
  Down,        // present and readable but not connected
  Unavailable, // present but the link information is temporarily unreadable
};

[[nodiscard]] const char *networkOperStateName(NetworkOperState state);
[[nodiscard]] const char *networkCarrierStateName(NetworkCarrierState state);
[[nodiscard]] const char *networkAdminStateName(NetworkAdminState state);
[[nodiscard]] const char *networkLinkAvailabilityName(
    NetworkLinkAvailability availability);

/// True for interface classifications that represent a physical link with a
/// real carrier (Ethernet, Wi-Fi, InfiniBand). Software/bridge/tunnel/virtual,
/// point-to-point and loopback devices are excluded so connectivity counts
/// reflect physical hardware only.
[[nodiscard]] bool isPhysicalNetworkLink(NetworkInterfaceType type);

// --- Pure, testable parsing/derivation helpers ---------------------------------

/// Maps a sysfs operstate string to the enum; missing/unrecognised values are
/// `Unknown` (never "down").
[[nodiscard]] NetworkOperState parseOperState(
    const std::optional<std::string> &operstate);

/// Maps the sysfs carrier integer (0/1) to the enum.
[[nodiscard]] NetworkCarrierState parseCarrierState(
    const std::optional<int> &carrier);

/// Maps the raw IFF_* flags to the derived administrative state.
[[nodiscard]] NetworkAdminState parseAdminState(
    const std::optional<unsigned> &flags, bool admin_up);

/// Derives the availability from the readable sub-states.
[[nodiscard]] NetworkLinkAvailability linkAvailability(
    bool link_known, NetworkOperState oper, NetworkCarrierState carrier,
    NetworkAdminState admin);

/// One link-lifecycle event, kept in a bounded history. Events are recorded
/// only on real state changes/appearances/disappearances, never on temporary
/// read failures.
enum class NetworkLinkEventType {
  StateChanged, // an operstate/carrier/admin value changed while present
  Appeared,     // a new stable identity was discovered
  Disappeared,  // the interface disappeared (removed)
  Reappeared,   // a previously-disappeared identity came back
};

[[nodiscard]] const char *networkLinkEventTypeName(NetworkLinkEventType type);

struct NetworkLinkStateEvent {
  NetworkLinkEventType type = NetworkLinkEventType::StateChanged;
  std::string identity;        // stable key ("idx:<ifindex>" / "name:<name>")
  std::string interface_name;  // current kernel name
  NetworkOperState previous_oper = NetworkOperState::Unknown;
  NetworkOperState new_oper = NetworkOperState::Unknown;
  NetworkCarrierState previous_carrier = NetworkCarrierState::Unknown;
  NetworkCarrierState new_carrier = NetworkCarrierState::Unknown;
  NetworkAdminState previous_admin = NetworkAdminState::Unknown;
  NetworkAdminState new_admin = NetworkAdminState::Unknown;
  bool is_recovery = false;  // availability was restored (recovery alert)
  std::string reason;        // human summary of the change
  std::uint64_t transition_count = 0;
  std::chrono::system_clock::time_point timestamp;
};

inline constexpr std::size_t kDefaultLinkStateMaxHistory = 100;
inline constexpr std::size_t kMaxTrackedLinkStateInterfaces = 64;

/// One interface's tracked link-lifecycle state, keyed by stable identity.
struct TrackedInterface {
  std::string identity;
  std::string name;
  NetworkOperState oper = NetworkOperState::Unknown;
  NetworkCarrierState carrier = NetworkCarrierState::Unknown;
  NetworkAdminState admin = NetworkAdminState::Unknown;
  bool has_valid_state = false;  // a readable state has been recorded
  bool link_known = false;       // metadata readable this tick
  bool present = true;           // false once the identity has disappeared
  std::chrono::steady_clock::time_point first_seen;
  std::chrono::steady_clock::time_point last_change_steady;
  std::chrono::system_clock::time_point last_change_system;
  std::uint64_t transition_count = 0;

  [[nodiscard]] NetworkLinkAvailability availability() const;
};

/// Aggregate link-state counts for the overview, physical interfaces only.
struct NetworkLinkStateCounts {
  std::size_t physical_total = 0;
  std::size_t connected = 0;
  std::size_t with_carrier = 0;
  std::size_t down = 0;
  std::size_t unavailable = 0;
};

/// Monitors network interface availability and link state.
///
/// Fed from the existing NetworkInterfaceMonitor snapshot exactly once per
/// tick by the application's monitoring loop (no second polling loop). Tracks
/// per-identity state (operstate/carrier/admin) across renames using the
/// stable identity key, keeps a bounded event history and forwards real state
/// changes to the central AlertManager and the optional desktop-notification
/// sink. A temporary read failure preserves the last valid state (marked
/// unavailable for display) and never fires an alert; an intentional removal
/// only clears the subject and records a history event, it never notifies.
class NetworkLinkStateMonitor {
 public:
  explicit NetworkLinkStateMonitor(
      AlertManager &alerts,
      std::size_t max_history = kDefaultLinkStateMaxHistory);

  NetworkLinkStateMonitor(const NetworkLinkStateMonitor &) = delete;
  NetworkLinkStateMonitor &operator=(const NetworkLinkStateMonitor &) = delete;

  /// Desktop-notification delivery callback; invoked for alert-worthy events.
  /// NotificationManager still applies the global settings and cooldown.
  using EventSink = void (*)(const AlertEvent &);
  void setEventSink(EventSink sink) { event_sink_ = sink; }

  /// Processes one discovery snapshot (produced by NetworkInterfaceMonitor).
  void update(const NetworkInterfaceSnapshot &snapshot);

  /// Bounded link-lifecycle history (oldest first).
  [[nodiscard]] const ResourceHistory<NetworkLinkStateEvent> &history() const {
    return history_;
  }

  /// Latest tracked state for an identity; nullptr when never seen.
  [[nodiscard]] const TrackedInterface *tracked(
      const std::string &identity) const;

  /// Aggregate counts for the current snapshot (recomputed each update()).
  [[nodiscard]] const NetworkLinkStateCounts &counts() const { return counts_; }

  void reset();

 private:
  AlertManager &alerts_;
  std::size_t max_history_;
  EventSink event_sink_ = nullptr;
  std::unordered_map<std::string, TrackedInterface> tracked_;
  ResourceHistory<NetworkLinkStateEvent> history_;
  NetworkLinkStateCounts counts_;

  void pushEvent(const NetworkLinkStateEvent &event);
  void recordAlert(const std::string &identity, AlertSeverity severity,
                   bool is_recovery, const std::string &message);
  void recount(const NetworkInterfaceSnapshot &snapshot);
};

}  // namespace atm