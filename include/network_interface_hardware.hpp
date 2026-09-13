#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "network_interface_details.hpp"
#include "network_link_state.hpp"  // isPhysicalNetworkLink (shared classification)

namespace atm {

/// Availability state of a hardware/driver field on one interface.
///
/// Distinguishes a fresh read (`Available`), a preserved value kept through a
/// temporary read failure (`Stale`), and a clean absence (`Unavailable` —
/// virtual device, no driver, nothing to report). `Unknown` means no probe has
/// been absorbed for the identity yet.
enum class NetworkHardwareState {
  Unknown,
  Available,
  Stale,
  Unavailable,
};
[[nodiscard]] const char *networkHardwareStateName(NetworkHardwareState state);

/// Physical/virtual classification of an interface with respect to hardware.
/// `Unknown` is used instead of guessing: presence of a MAC address or an
/// Ethernet ARPHRD type never implies physical hardware by itself.
enum class NetworkDeviceKind {
  Unknown,
  Physical,  // a real device on a bus (PCI/USB/platform/...) backs the link
  Virtual,   // software device (loopback, veth, bridge, bond, tunnel, ...)
};
[[nodiscard]] const char *networkDeviceKindName(NetworkDeviceKind kind);

/// One interface's hardware/driver metadata.
///
/// `device_*` describe the device directory the interface relates to (the
/// resolved `device` symlink), `driver_*` the currently bound kernel driver.
/// Fields are only ever filled from the kernel sysfs tree; a missing value is
/// represented by its explicit state (`Unavailable`/`Stale`) — never a guessed
/// zero, never a fake "physical".
struct NetworkInterfaceHardware {
  std::string identity;          // stable key ("idx:<ifindex>" / "name:<name>")
  std::string name;              // current kernel name
  bool present = true;           // false once the identity has disappeared
  bool device_related = false;   // <iface>/device resolved to a real directory

  NetworkDeviceKind device_kind = NetworkDeviceKind::Unknown;
  NetworkHardwareState device_state = NetworkHardwareState::Unknown;
  std::string device_path;  // resolved device directory (e.g. "/sys/devices/...")
  std::string device_bus;   // subsystem basename ("pci", "usb", "platform", ...)
  std::string device_id;    // raw <iface>/dev_id value ("0x0", ...)
  std::optional<int> name_assign_type;  // raw <iface>/name_assign_type

  std::string driver;  // basename of <iface>/device/driver, if bound
  NetworkHardwareState driver_state = NetworkHardwareState::Unknown;

  // Last successful wall-clock sysfs read; first-seen for eviction ordering.
  std::chrono::system_clock::time_point last_read{};
  bool sysfs_read_ok = false;  // last probe succeeded (internal monitor flag)
  std::chrono::steady_clock::time_point first_seen{};
  std::chrono::steady_clock::time_point last_probe{};
};

/// Classifies an interface as physical/virtual from the existing interface
/// classification plus the resolved `device` relationship. A `/sys/devices/
/// virtual/` device is unambiguous software; synthetic interface types
/// (loopback/bridge/bond/tunnel/virtual) are always virtual; a non-bus device
/// on an otherwise physical-looking type stays honest as `Unknown`.
[[nodiscard]] NetworkDeviceKind classifyNetworkDeviceKind(
    NetworkInterfaceType type, bool device_related,
    const std::string &device_path);

/// One raw probe of an interface's hardware/driver sysfs entries. `read_ok`
/// records whether the device symlink could be probed at all; `access_denied`
/// distinguishes a permission error (temporarily preserved as stale) from a
/// clean absence (ENOENT/loopback/virtual → unavailable). Pure data — never
/// throws, one failed probe never stops the others.
struct NetworkHardwareRead {
  bool read_ok = true;         // `<iface>/device` was resolvable or absent
  bool access_denied = false;  // a sysfs path existed but was not readable
  bool device_related = false;
  std::string device_path;
  std::string device_bus;
  std::string device_id;
  std::optional<int> name_assign_type;
  std::string driver;
  bool has_driver = false;
};

/// Probes `<root>/sys/class/net/<iface>/device`, its driver and the nearby
/// attribute files using native filesystem APIs (no subprocesses, no shell).
/// `root` defaults to "/" (the real sysfs tree) but may be an alternate root
/// for hermetic tests. Returns a `NetworkHardwareRead`; never throws.
[[nodiscard]] NetworkHardwareRead readNetworkInterfaceHardware(
    const std::filesystem::path &root, const std::string &iface);

/// Merges a fresh probe into the previously retained record for the same
/// stable identity. A temporary read failure (permission/removal) preserves the
/// last valid driver/device values and marks them stale; a clean absence turns
/// them unavailable. Pure and testable: no syscalls, no global state.
[[nodiscard]] NetworkInterfaceHardware updateNetworkInterfaceHardware(
    const NetworkInterfaceHardware &previous, const NetworkHardwareRead &read,
    const std::string &name, NetworkInterfaceType type,
    std::chrono::system_clock::time_point refreshed_at);

/// How long a cached hardware/driver record is reused before it is re-probed.
/// Driver/device metadata changes far less often than traffic counters, so a
/// fresh probe every N seconds is plenty; new/renamed identities and failed
/// probes are always re-probed on the next tick.
inline constexpr std::chrono::seconds kNetworkHardwareRereadInterval{10};

/// Monitors per-interface hardware and driver metadata.
///
/// Fed from the existing NetworkInterfaceMonitor snapshot exactly once per tick
/// by the application's monitoring loop (no second polling loop, nothing read
/// on the UI-render path). Static metadata is cached and only re-probed when an
/// interface appears, its name (identity) changes, the previous probe failed,
/// or the reread interval elapsed. A vanished interface is retained and marked
/// gone (never crashes, never mixes in an old identity's metadata).
class NetworkInterfaceHardwareMonitor {
 public:
  static constexpr std::size_t kMaxTrackedHardwareInterfaces = 64;

  explicit NetworkInterfaceHardwareMonitor(
      std::filesystem::path root = "/",
      std::chrono::milliseconds reread_interval = kNetworkHardwareRereadInterval);

  NetworkInterfaceHardwareMonitor(const NetworkInterfaceHardwareMonitor &) =
      delete;
  NetworkInterfaceHardwareMonitor &operator=(
      const NetworkInterfaceHardwareMonitor &) = delete;
  ~NetworkInterfaceHardwareMonitor() = default;

  /// Processes one discovery snapshot (produced by NetworkInterfaceMonitor).
  void update(const NetworkInterfaceSnapshot &snapshot);

  /// Latest metadata for an identity; nullptr when never seen.
  [[nodiscard]] const NetworkInterfaceHardware *tracked(
      const std::string &identity) const;

  /// All retained metadata (present and gone identities).
  [[nodiscard]] const std::unordered_map<std::string, NetworkInterfaceHardware>
      &entries() const {
    return tracked_;
  }

  void reset();

 private:
  std::filesystem::path root_;
  std::chrono::milliseconds reread_interval_;
  std::unordered_map<std::string, NetworkInterfaceHardware> tracked_;
  void evictOverflow();
};

}  // namespace atm