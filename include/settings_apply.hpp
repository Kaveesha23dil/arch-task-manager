#pragma once

#include "alert_manager.hpp"
#include "history_manager.hpp"
#include "notification_manager.hpp"
#include "settings.hpp"

namespace atm::cfg {

/// Copies the effective settings into the runtime components owned by the
/// application. Cheap enough to be called at startup, after any single setting
/// change, and after a reset. Never restarts the application; every setting
/// takes effect on the next monitoring cycle.
void applySettingsToRuntime(const AppSettings &settings,
                            int &refresh_interval_ms,
                            atm::HistoryManager &history,
                            atm::AlertManager &alerts,
                            atm::NotificationManager &notifications);

}  // namespace atm::cfg