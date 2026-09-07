#include "systemd_manager.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <systemd/sd-bus.h>

namespace atm {

namespace {

constexpr const char *kSystemdDestination = "org.freedesktop.systemd1";
constexpr const char *kSystemdPath = "/org/freedesktop/systemd1";
constexpr const char *kManagerInterface = "org.freedesktop.systemd1.Manager";
constexpr const char *kUnitInterface = "org.freedesktop.systemd1.Unit";

/// Maps a D-Bus active state string to our ServiceState enum.
ServiceState parseServiceState(const char *state) {
  if (!state) return ServiceState::Unknown;
  if (std::strcmp(state, "active") == 0) return ServiceState::Active;
  if (std::strcmp(state, "inactive") == 0) return ServiceState::Inactive;
  if (std::strcmp(state, "failed") == 0) return ServiceState::Failed;
  if (std::strcmp(state, "activating") == 0) return ServiceState::Activating;
  if (std::strcmp(state, "deactivating") == 0)
    return ServiceState::Deactivating;
  if (std::strcmp(state, "reloading") == 0) return ServiceState::Reloading;
  return ServiceState::Unknown;
}

/// Maps a unit file state string to our UnitFileState enum.
UnitFileState parseUnitFileState(const char *state) {
  if (!state) return UnitFileState::Unknown;
  if (std::strcmp(state, "enabled") == 0) return UnitFileState::Enabled;
  if (std::strcmp(state, "disabled") == 0) return UnitFileState::Disabled;
  if (std::strcmp(state, "static") == 0) return UnitFileState::Static;
  if (std::strcmp(state, "masked") == 0) return UnitFileState::Masked;
  if (std::strcmp(state, "indirect") == 0) return UnitFileState::Indirect;
  if (std::strcmp(state, "generated") == 0) return UnitFileState::Generated;
  if (std::strcmp(state, "transient") == 0) return UnitFileState::Transient;
  if (std::strcmp(state, "bad") == 0) return UnitFileState::Bad;
  return UnitFileState::Unknown;
}

/// Classifies a D-Bus error from systemd into our service-operation statuses.
ServiceOperationStatus classifyBusError(const sd_bus_error &error) {
  if (!error.name) {
    return ServiceOperationStatus::BusError;
  }
  const std::string_view name(error.name);
  if (name.find("AccessDenied") != std::string_view::npos ||
      name.find("Unauthorized") != std::string_view::npos ||
      name.find("InteractiveAuthorizationRequired") !=
          std::string_view::npos) {
    return ServiceOperationStatus::PermissionDenied;
  }
  if (name == "org.freedesktop.systemd1.NoSuchUnit") {
    return ServiceOperationStatus::UnitNotFound;
  }
  // Some systemd polkit failures only put the details in the message text.
  if (error.message &&
      (std::strstr(error.message, "Access denied") != nullptr ||
       std::strstr(error.message, "Permission denied") != nullptr)) {
    return ServiceOperationStatus::PermissionDenied;
  }
  return ServiceOperationStatus::BusError;
}

/// Helper to read a uint32 property from a D-Bus unit object.
std::uint32_t readUnitPropertyUint32(sd_bus *bus,
                                     const std::string &unit_path,
                                     const char *property) {
  sd_bus_error error = SD_BUS_ERROR_NULL;
  sd_bus_message *msg = nullptr;
  std::uint32_t value = 0;

  int r = sd_bus_get_property(bus, kSystemdDestination, unit_path.c_str(),
                              kUnitInterface, property, &error, &msg, "u");
  if (r < 0) {
    sd_bus_error_free(&error);
    if (msg) sd_bus_message_unref(msg);
    return 0;
  }

  r = sd_bus_message_read(msg, "u", &value);
  sd_bus_message_unref(msg);
  if (r < 0) return 0;
  return value;
}

/// Reads the UnitFileState for a unit from its D-Bus object.
std::string readUnitFileStateFromBus(sd_bus *bus,
                                     const std::string &unit_path) {
  sd_bus_error error = SD_BUS_ERROR_NULL;
  sd_bus_message *msg = nullptr;

  int r = sd_bus_get_property(bus, kSystemdDestination, unit_path.c_str(),
                              kUnitInterface, "UnitFileState", &error, &msg,
                              "s");
  if (r < 0) {
    sd_bus_error_free(&error);
    if (msg) sd_bus_message_unref(msg);
    return {};
  }

  const char *value = nullptr;
  r = sd_bus_message_read(msg, "s", &value);
  std::string result;
  if (r >= 0 && value) {
    result = value;
  }
  sd_bus_message_unref(msg);
  return result;
}

}  // namespace

const char *serviceStateName(ServiceState state) {
  switch (state) {
    case ServiceState::Unknown:
      return "Unknown";
    case ServiceState::Active:
      return "active";
    case ServiceState::Inactive:
      return "inactive";
    case ServiceState::Failed:
      return "FAILED";
    case ServiceState::Activating:
      return "activating";
    case ServiceState::Deactivating:
      return "deactivating";
    case ServiceState::Reloading:
      return "reloading";
  }
  return "Unknown";
}

const char *unitFileStateName(UnitFileState state) {
  switch (state) {
    case UnitFileState::Enabled:
      return "enabled";
    case UnitFileState::Disabled:
      return "disabled";
    case UnitFileState::Static:
      return "static";
    case UnitFileState::Masked:
      return "masked";
    case UnitFileState::Indirect:
      return "indirect";
    case UnitFileState::Generated:
      return "generated";
    case UnitFileState::Transient:
      return "transient";
    case UnitFileState::Bad:
      return "bad";
    case UnitFileState::Unknown:
      return "unknown";
  }
  return "unknown";
}

const char *serviceSortName(ServiceSort sort) {
  switch (sort) {
    case ServiceSort::Name:
      return "Name";
    case ServiceSort::Status:
      return "Status";
    case ServiceSort::Enabled:
      return "Enabled";
    case ServiceSort::Description:
      return "Description";
  }
  return "Name";
}

// ---------------------------------------------------------------------------
// SystemdManager::Impl — private implementation holding the sd-bus state.
// ---------------------------------------------------------------------------

struct SystemdManager::Impl {
  sd_bus *bus = nullptr;
  bool available = false;
  std::vector<SystemdService> services;
  std::vector<std::string> cached_unit_paths;  // parallel to services
  bool discovered = false;

  Impl() = default;

  ~Impl() {
    if (bus) {
      sd_bus_unref(bus);
    }
  }

  Impl(const Impl &) = delete;
  Impl &operator=(const Impl &) = delete;

  /// Opens the system bus and verifies that systemd is reachable.
  bool connect() {
    if (bus) {
      sd_bus_unref(bus);
      bus = nullptr;
    }
    int r = sd_bus_open_system(&bus);
    if (r < 0) {
      available = false;
      return false;
    }
    available = true;
    return true;
  }

  /// Lists all loaded service units via the Manager's ListUnits method.
  /// ListUnits returns a(ssssssouso):
  ///   0:s  Id
  ///   1:s  Description
  ///   2:s  LoadState
  ///   3:s  ActiveState
  ///   4:s  SubState
  ///   5:s  Following
  ///   6:o  ObjectPath
  ///   7:u  JobId
  ///   8:s  JobType
  ///   9:o  JobObjectPath
  bool listUnits() {
    if (!bus) return false;

    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *msg = nullptr;

    int r = sd_bus_call_method(bus, kSystemdDestination, kSystemdPath,
                               kManagerInterface, "ListUnits", &error, &msg,
                               "");
    if (r < 0) {
      sd_bus_error_free(&error);
      if (msg) sd_bus_message_unref(msg);
      return false;
    }

    services.clear();
    cached_unit_paths.clear();

    r = sd_bus_message_enter_container(msg, SD_BUS_TYPE_ARRAY, "(ssssssouso)");
    if (r < 0) {
      sd_bus_message_unref(msg);
      return false;
    }

    while ((r = sd_bus_message_enter_container(msg, SD_BUS_TYPE_STRUCT,
                                               "ssssssouso")) > 0) {
      const char *id = nullptr;
      const char *description = nullptr;
      const char *load_state = nullptr;
      const char *active_state = nullptr;
      const char *sub_state = nullptr;
      const char *following = nullptr;
      const char *object_path = nullptr;
      std::uint32_t job_id = 0;
      const char *job_type = nullptr;
      const char *job_path = nullptr;

      r = sd_bus_message_read(msg, "ssssssouso", &id, &description, &load_state,
                              &active_state, &sub_state, &following,
                              &object_path, &job_id, &job_type, &job_path);
      if (r < 0) break;

      // Filter to service units only (name ends with ".service").
      const std::string name = id ? id : "";
      if (name.size() < 8 ||
          name.compare(name.size() - 8, 8, ".service") != 0) {
        sd_bus_message_exit_container(msg);
        continue;
      }

      SystemdService svc;
      svc.name = name;
      svc.description = description ? description : "";
      svc.load_state = load_state ? load_state : "";
      svc.active_state = active_state ? active_state : "";
      svc.sub_state = sub_state ? sub_state : "";
      svc.state = parseServiceState(active_state);
      svc.unit_file_path = object_path ? object_path : "";

      cached_unit_paths.push_back(object_path ? object_path : "");
      services.push_back(std::move(svc));

      sd_bus_message_exit_container(msg);
    }

    sd_bus_message_exit_container(msg);
    sd_bus_message_unref(msg);
    return true;
  }

  /// Enriches each cached service with its UnitFileState and MainPID.
  /// Description/LoadState/ActiveState/SubState already arrive current from
  /// ListUnits, so only the two extra per-object properties are fetched here.
  void enrichServices() {
    if (!bus) return;
    for (std::size_t i = 0; i < services.size(); ++i) {
      const std::string &path = cached_unit_paths[i];
      if (path.empty()) continue;

      const std::string file_state = readUnitFileStateFromBus(bus, path);
      services[i].file_state = parseUnitFileState(file_state.c_str());
      services[i].enabled =
          (services[i].file_state == UnitFileState::Enabled);
      services[i].static_unit =
          (services[i].file_state == UnitFileState::Static);

      services[i].main_pid =
          readUnitPropertyUint32(bus, path, "MainPID");
    }
  }

  /// Calls a Manager method that takes (ss): unit name + mode.
  ServiceOperationResult callManagerMethod(const char *method,
                                           const std::string &unit_name) {
    ServiceOperationResult result;
    if (!bus) {
      result.status = ServiceOperationStatus::Failed;
      result.error_message = "Not connected to systemd.";
      return result;
    }

    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *msg = nullptr;

    int r = sd_bus_call_method(bus, kSystemdDestination, kSystemdPath,
                               kManagerInterface, method, &error, &msg, "ss",
                               unit_name.c_str(), "replace");
    if (r < 0) {
      result.status = classifyBusError(error);
      if (result.status == ServiceOperationStatus::BusError ||
          result.status == ServiceOperationStatus::Failed) {
        result.status = ServiceOperationStatus::Failed;
      }
      result.error_message = error.message ? error.message : "Unknown error";
      sd_bus_error_free(&error);
      if (msg) sd_bus_message_unref(msg);
      return result;
    }

    if (msg) sd_bus_message_unref(msg);
    return result;
  }

  /// Enables unit files via EnableUnitFiles D-Bus method.
  ServiceOperationResult enableUnitFiles(const std::string &unit_name) {
    ServiceOperationResult result;
    if (!bus) {
      result.status = ServiceOperationStatus::Failed;
      result.error_message = "Not connected to systemd.";
      return result;
    }

    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *msg = nullptr;

    // EnableUnitFiles(asbb): files, runtime, force
    // We want persistent enable (runtime=false), no force.
    int r = sd_bus_call_method(bus, kSystemdDestination, kSystemdPath,
                               kManagerInterface, "EnableUnitFiles", &error,
                               &msg, "asbb", 1, unit_name.c_str(), false,
                               false);
    if (r < 0) {
      result.status = classifyBusError(error);
      if (result.status == ServiceOperationStatus::BusError) {
        result.status = ServiceOperationStatus::Failed;
      }
      result.error_message = error.message ? error.message : "Unknown error";
      sd_bus_error_free(&error);
      if (msg) sd_bus_message_unref(msg);
      return result;
    }

    if (msg) sd_bus_message_unref(msg);
    return result;
  }

  /// Disables unit files via DisableUnitFiles D-Bus method.
  ServiceOperationResult disableUnitFiles(const std::string &unit_name) {
    ServiceOperationResult result;
    if (!bus) {
      result.status = ServiceOperationStatus::Failed;
      result.error_message = "Not connected to systemd.";
      return result;
    }

    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *msg = nullptr;

    // DisableUnitFiles(asb): files, runtime
    int r = sd_bus_call_method(bus, kSystemdDestination, kSystemdPath,
                               kManagerInterface, "DisableUnitFiles", &error,
                               &msg, "asb", 1, unit_name.c_str(), false);
    if (r < 0) {
      result.status = classifyBusError(error);
      if (result.status == ServiceOperationStatus::BusError) {
        result.status = ServiceOperationStatus::Failed;
      }
      result.error_message = error.message ? error.message : "Unknown error";
      sd_bus_error_free(&error);
      if (msg) sd_bus_message_unref(msg);
      return result;
    }

    if (msg) sd_bus_message_unref(msg);
    return result;
  }
};

// ---------------------------------------------------------------------------
// SystemdManager public API — thin wrappers around Impl.
// ---------------------------------------------------------------------------

SystemdManager::SystemdManager() : impl_(new Impl) { impl_->connect(); }

SystemdManager::~SystemdManager() { delete impl_; }

bool SystemdManager::isAvailable() const { return impl_->available; }

void SystemdManager::discover() {
  if (!impl_->available) {
    impl_->connect();
    if (!impl_->available) return;
  }
  if (impl_->listUnits()) {
    impl_->enrichServices();
    impl_->discovered = true;
  }
}

SystemdSnapshot SystemdManager::read() {
  SystemdSnapshot snapshot;

  if (!impl_->available) {
    // Try reconnecting once per tick in case systemd became available.
    impl_->connect();
    if (!impl_->available) {
      snapshot.available = false;
      return snapshot;
    }
  }

  if (!impl_->discovered) {
    discover();
  }

  // Refresh dynamic state from D-Bus for each service.
  // Re-list units to pick up newly loaded or unloaded services.
  if (impl_->listUnits()) {
    impl_->enrichServices();
  }

  snapshot.services = impl_->services;
  snapshot.available = true;
  return snapshot;
}

ServiceOperationResult SystemdManager::startService(
    const std::string &name) {
  return impl_->callManagerMethod("StartUnit", name);
}

ServiceOperationResult SystemdManager::stopService(const std::string &name) {
  return impl_->callManagerMethod("StopUnit", name);
}

ServiceOperationResult SystemdManager::restartService(
    const std::string &name) {
  return impl_->callManagerMethod("RestartUnit", name);
}

ServiceOperationResult SystemdManager::reloadService(
    const std::string &name) {
  return impl_->callManagerMethod("ReloadUnit", name);
}

ServiceOperationResult SystemdManager::enableService(
    const std::string &name) {
  return impl_->enableUnitFiles(name);
}

ServiceOperationResult SystemdManager::disableService(
    const std::string &name) {
  return impl_->disableUnitFiles(name);
}

}  // namespace atm
