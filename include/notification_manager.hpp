#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "alert_manager.hpp"

namespace atm {

/// In-memory configuration for desktop notifications. No persistent storage.
struct NotificationSettings {
  bool enabled = false;          // master switch; off by default
  bool notify_on_warning = false;  // push Warning notifications
  bool notify_on_critical = true;  // push Critical notifications (default on)
  bool notify_on_recovery = false; // push recovery (return to Normal) events
  std::chrono::seconds cooldown{60};  // min time between notifications per source
  unsigned timeout_ms = 10000;        // how long the notification stays visible
};

/// Delivers native Linux desktop notifications for alert events over D-Bus.
///
/// Uses the org.freedesktop.Notifications interface via sd-bus (libsystemd);
/// no external helpers are spawned. The manager holds no alert-detection or
/// state-transition logic of its own: it only renders and sends the events it
/// is handed. It applies a per-source cooldown and replaces a source's prior
/// notification by id to avoid spamming an already-visible popup. If the D-Bus
/// notification service is unavailable the call is a no-op and never throws, so
/// in-app alerts continue to work.
class NotificationManager {
 public:
  NotificationManager();
  ~NotificationManager();

  NotificationManager(const NotificationManager&) = delete;
  NotificationManager& operator=(const NotificationManager&) = delete;

  /// Re-opens the D-Bus connection (e.g. after a failed connect). Non-fatal.
  void connect();

  /// Access to the mutable notification settings.
  [[nodiscard]] NotificationSettings& settings() { return settings_; }
  [[nodiscard]] const NotificationSettings& settings() const { return settings_; }

  /// Sends a desktop notification for an alert event, subject to settings and
  /// per-source cooldown. Safe to call when D-Bus is unavailable.
  void notify(const AlertEvent& event);

  /// Returns true if the last notify() delivery attempted D-Bus successfully.
  [[nodiscard]] bool available() const { return connected_; }

 private:
  NotificationSettings settings_;
  bool connected_ = false;
  void* bus_ = nullptr;  // sd_bus*

  std::unordered_map<std::string, std::chrono::steady_clock::time_point>
      last_sent_;                                   // per-source cooldown timestamps
  std::unordered_map<std::string, std::uint32_t> ids_;  // source -> notification id

  /// Whether the event should be delivered under the current settings.
  [[nodiscard]] bool shouldNotify(const AlertEvent& event) const;

  /// True if the source is still within its cooldown window.
  [[nodiscard]] bool inCooldown(const AlertEvent& event) const;

  /// Raw D-Bus delivery (one call). Returns false on any failure.
  bool deliver(const AlertEvent& event);
};

}  // namespace atm
