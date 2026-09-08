#include "settings_apply.hpp"

namespace atm::cfg {
namespace {

void applyCategory(atm::AlertManager &alerts, atm::AlertType type,
                   const AlertCategorySettings &category, int hysteresis) {
  atm::AlertThreshold &threshold = alerts.threshold(type);
  threshold.enabled = category.enabled;
  threshold.warning = category.warning;
  threshold.critical = category.critical;
  const double recovery =
      category.warning - static_cast<double>(hysteresis);
  threshold.recovery = recovery < 0.0 ? 0.0 : recovery;
}

}  // namespace

void applySettingsToRuntime(const AppSettings &settings,
                            int &refresh_interval_ms,
                            atm::HistoryManager &history,
                            atm::AlertManager &alerts,
                            atm::NotificationManager &notifications) {
  // General: the monitoring loop reads this value every cycle.
  refresh_interval_ms = settings.general.refresh_interval_ms;

  // History: resizing drops existing samples but keeps memory bounded.
  if (history.maxSamples() !=
      static_cast<std::size_t>(settings.history.max_samples)) {
    history.setMaxSamples(static_cast<std::size_t>(settings.history.max_samples));
  }

  // Alerts: only the five user-facing categories are managed by settings; the
  // remaining types (disk/network activity, GPU) keep their built-in defaults.
  applyCategory(alerts, atm::AlertType::CpuUsage, settings.alerts.cpu,
                settings.alerts.recovery_hysteresis);
  applyCategory(alerts, atm::AlertType::MemoryUsage, settings.alerts.memory,
                settings.alerts.recovery_hysteresis);
  applyCategory(alerts, atm::AlertType::SwapUsage, settings.alerts.swap,
                settings.alerts.recovery_hysteresis);
  applyCategory(alerts, atm::AlertType::DiskUsage, settings.alerts.disk,
                settings.alerts.recovery_hysteresis);
  applyCategory(alerts, atm::AlertType::Temperature, settings.alerts.temperature,
                settings.alerts.recovery_hysteresis);

  // Notifications: 1:1 mirror into the existing manager struct.
  atm::NotificationSettings &ns = notifications.settings();
  ns.enabled = settings.notifications.enabled;
  ns.notify_on_warning = settings.notifications.warning_notifications;
  ns.notify_on_critical = settings.notifications.critical_notifications;
  ns.notify_on_recovery = settings.notifications.recovery_notifications;
  ns.cooldown = std::chrono::seconds(settings.notifications.cooldown_seconds);
  ns.timeout_ms = static_cast<unsigned>(settings.notifications.timeout_ms);
}

}  // namespace atm::cfg