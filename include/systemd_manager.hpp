#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace atm {

/// High-level runtime state of a systemd service unit.
enum class ServiceState {
  Unknown,
  Active,
  Inactive,
  Failed,
  Activating,
  Deactivating,
  Reloading,
};

/// Human-readable name of a ServiceState.
[[nodiscard]] const char *serviceStateName(ServiceState state);

/// Boot-time enablement of a unit.
enum class UnitFileState {
  Enabled,
  Disabled,
  Static,
  Masked,
  Indirect,
  Generated,
  Transient,
  Bad,
  Unknown,
};

/// Human-readable name of a UnitFileState.
[[nodiscard]] const char *unitFileStateName(UnitFileState state);

/// Outcome of a management operation (start/stop/restart/enable/disable).
enum class ServiceOperationStatus {
  Success,
  PermissionDenied,
  UnitNotFound,
  Failed,
  BusError,
};

/// Result of a service management action.
struct ServiceOperationResult {
  ServiceOperationStatus status = ServiceOperationStatus::Success;
  std::string error_message;

  [[nodiscard]] bool success() const {
    return status == ServiceOperationStatus::Success;
  }
};

/// One systemd service unit discovered via the D-Bus API.
struct SystemdService {
  std::string name;           // e.g. "sshd.service"
  std::string description;    // e.g. "OpenSSH server daemon"
  std::string load_state;     // "loaded", "not-found", "error", ...
  std::string active_state;   // "active", "inactive", "failed", ...
  std::string sub_state;      // "running", "dead", "waiting", ...
  ServiceState state = ServiceState::Unknown;
  UnitFileState file_state = UnitFileState::Unknown;
  bool enabled = false;       // derived from file_state
  bool static_unit = false;
  std::uint32_t main_pid = 0;
  std::string unit_file_path; // e.g. "/usr/lib/systemd/system/sshd.service"
};

/// A snapshot of all discovered systemd service units.
struct SystemdSnapshot {
  std::vector<SystemdService> services;
  bool available = false;  // true when the D-Bus connection to systemd succeeded
};

/// Sorting order for the service list.
enum class ServiceSort {
  Name,
  Status,
  Enabled,
  Description,
};

/// Human-readable name of a ServiceSort variant.
[[nodiscard]] const char *serviceSortName(ServiceSort sort);

/**
 * Systemd service manager using the native sd-bus / libsystemd D-Bus API.
 *
 * This module connects to the system D-Bus, queries the
 * org.freedesktop.systemd1 Manager interface for loaded service units, and
 * caches relatively static information (name, description, unit file path)
 * while refreshing dynamic information (active state, sub state, main PID)
 * on every read. Management operations (start, stop, restart, enable,
 * disable) use the corresponding D-Bus methods directly — no shell commands
 * or systemctl invocations.
 *
 * If the D-Bus connection to systemd cannot be established, the module
 * degrades gracefully: `read()` returns an empty snapshot with
 * `available = false` and management operations return appropriate errors.
 * The rest of the Task Manager continues to function normally.
 *
 * The refresh cadence is owned by the application's main loop — this class
 * never starts its own thread.
 */
class SystemdManager {
 public:
  SystemdManager();
  ~SystemdManager();

  // SystemdManager holds D-Bus connection state; copying/moving would
  // duplicate the connection.
  SystemdManager(const SystemdManager &) = delete;
  SystemdManager &operator=(const SystemdManager &) = delete;

  /// Returns true when the D-Bus connection to systemd was successfully
  /// established and services are available.
  [[nodiscard]] bool isAvailable() const;

  /// (Re)discovers all loaded service units from systemd. Called once at
  /// startup. Subsequent calls to read() also re-discover when the service
  /// set changes.
  void discover();

  /// Refreshes dynamic state (active/sub state, PID) of all known services
  /// and returns a snapshot. Cached identity data is not re-read unless
  /// the service set has changed.
  [[nodiscard]] SystemdSnapshot read();

  /// Start a service unit. Requires confirmation from the caller.
  [[nodiscard]] ServiceOperationResult startService(const std::string &name);

  /// Stop a service unit. Requires confirmation from the caller.
  [[nodiscard]] ServiceOperationResult stopService(const std::string &name);

  /// Restart a service unit. Requires confirmation from the caller.
  [[nodiscard]] ServiceOperationResult restartService(const std::string &name);

  /// Reload a service unit (sends SIGUSR1 or similar to the main process).
  [[nodiscard]] ServiceOperationResult reloadService(const std::string &name);

  /// Enable a service unit for automatic startup at boot.
  [[nodiscard]] ServiceOperationResult enableService(const std::string &name);

  /// Disable a service unit so it does not start automatically at boot.
  [[nodiscard]] ServiceOperationResult disableService(const std::string &name);

 private:
  struct Impl;
  Impl *impl_;
};

}  // namespace atm
