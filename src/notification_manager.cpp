#include "notification_manager.hpp"

#include <systemd/sd-bus.h>

#include <chrono>
#include <string>
#include <utility>

namespace atm {

namespace {

constexpr const char* kService = "org.freedesktop.Notifications";
constexpr const char* kPath = "/org/freedesktop/Notifications";
constexpr const char* kInterface = "org.freedesktop.Notifications";
constexpr const char* kAppName = "Arch Task Manager";

/// Maps an alert severity to a D-Bus notifications urgency hint:
/// Normal -> 0 (low), Warning -> 1 (normal), Critical -> 2 (critical).
std::uint8_t urgencyFor(AlertSeverity severity) {
  switch (severity) {
    case AlertSeverity::Normal:   return 0;
    case AlertSeverity::Warning:  return 1;
    case AlertSeverity::Critical: return 2;
  }
  return 1;
}

}  // namespace

NotificationManager::NotificationManager() {
  connect();
}

NotificationManager::~NotificationManager() {
  if (bus_ != nullptr) {
    sd_bus_flush_close_unref(static_cast<sd_bus*>(bus_));
    bus_ = nullptr;
  }
  connected_ = false;
}

void NotificationManager::connect() {
  if (bus_ != nullptr) {
    sd_bus_flush_close_unref(static_cast<sd_bus*>(bus_));
    bus_ = nullptr;
  }
  sd_bus* bus = nullptr;
  const int r = sd_bus_default_user(&bus);
  if (r < 0 || bus == nullptr) {
    connected_ = false;
    return;
  }
  bus_ = bus;
  // Lazy default: the connection is only fully established on first use, so
  // do not report failure here. connected_ means "we have a bus handle".
  connected_ = true;
}

bool NotificationManager::shouldNotify(const AlertEvent& event) const {
  if (!settings_.enabled) {
    return false;
  }
  if (event.is_recovery) {
    return settings_.notify_on_recovery;
  }
  switch (event.severity) {
    case AlertSeverity::Critical: return settings_.notify_on_critical;
    case AlertSeverity::Warning:  return settings_.notify_on_warning;
    case AlertSeverity::Normal:   return false;
  }
  return false;
}

bool NotificationManager::inCooldown(const AlertEvent& event) const {
  const std::string key = alertTypeName(event.type) + std::string("|") + event.source;
  const auto it = last_sent_.find(key);
  if (it == last_sent_.end()) {
    return false;
  }
  return std::chrono::steady_clock::now() - it->second < settings_.cooldown;
}

bool NotificationManager::deliver(const AlertEvent& event) {
  if (bus_ == nullptr) {
    return false;
  }
  sd_bus* bus = static_cast<sd_bus*>(bus_);

  const std::string key = alertTypeName(event.type) + std::string("|") + event.source;
  const std::uint32_t replace_id =
      ids_.count(key) != 0 ? ids_[key] : 0u;

  const std::string title = [&]() {
    if (event.is_recovery) {
      return std::string("Recovered: ") + alertTypeName(event.type);
    }
    return std::string(alertSeverityName(event.severity)) + " \u2014 " +
           alertTypeName(event.type);
  }();

  const std::string body = event.source + ": " + event.message;

  sd_bus_message* msg = nullptr;
  int r = sd_bus_message_new_method_call(bus, &msg, kService, kPath,
                                         kInterface, "Notify");
  if (r < 0) {
    return false;
  }

  // Build the Notify call:
  //   Notify(s app_name, u replaces_id, s app_icon, s summary, s body,
  //          as actions, a{sv} hints, i expire_timeout) -> u id
  const bool valid =
      sd_bus_message_append(msg, "susss", kAppName, replace_id, "", title.c_str(),
                            body.c_str()) >= 0 &&
      // Empty actions array.
      sd_bus_message_open_container(msg, 'a', "s") >= 0 &&
      sd_bus_message_close_container(msg) >= 0 &&
      // Hints: urgency key -> u byte.
      sd_bus_message_open_container(msg, 'a', "{sv}") >= 0 &&
      sd_bus_message_open_container(msg, 'e', "sv") >= 0 &&
      sd_bus_message_append(msg, "s", "urgency") >= 0 &&
      sd_bus_message_open_container(msg, 'v', "y") >= 0 &&
      sd_bus_message_append(msg, "y", urgencyFor(event.severity)) >= 0 &&
      sd_bus_message_close_container(msg) >= 0 &&
      sd_bus_message_close_container(msg) >= 0 &&
      sd_bus_message_close_container(msg) >= 0 &&
      sd_bus_message_append(msg, "i", static_cast<int>(settings_.timeout_ms)) >= 0;

  if (!valid) {
    sd_bus_message_unref(msg);
    return false;
  }

  sd_bus_error error = SD_BUS_ERROR_NULL;
  sd_bus_message* reply = nullptr;
  r = sd_bus_call(bus, msg, 0, &error, &reply);
  sd_bus_message_unref(msg);

  if (r < 0) {
    // Service unavailable or call failed. Keep the reply pointer null-safe.
    if (reply != nullptr) {
      sd_bus_message_unref(reply);
    }
    sd_bus_error_free(&error);
    // Drop the (possibly dead) handle so a later notify() can reconnect.
    sd_bus_flush_close_unref(bus);
    bus_ = nullptr;
    connected_ = false;
    return false;
  }

  std::uint32_t id = 0;
  sd_bus_message_read(reply, "u", &id);
  ids_[key] = id;
  sd_bus_message_unref(reply);
  sd_bus_error_free(&error);
  return true;
}

void NotificationManager::notify(const AlertEvent& event) {
  if (!shouldNotify(event)) {
    return;
  }
  if (inCooldown(event)) {
    return;
  }

  const bool ok = deliver(event);
  if (ok) {
    const std::string key =
        alertTypeName(event.type) + std::string("|") + event.source;
    last_sent_[key] = std::chrono::steady_clock::now();
  }
}

}  // namespace atm
