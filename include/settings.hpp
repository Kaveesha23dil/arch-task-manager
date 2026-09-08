#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace atm::cfg {

/// Schema/format version written into every configuration file. Future
/// releases bump this constant and extend SettingsManager::migrateConfig so
/// older files can be upgraded.
inline constexpr int kCurrentConfigVersion = 1;

// --- Numeric limits. Every value read from disk or entered in the settings
// --- page is clamped into these ranges before it is used. -----------------
inline constexpr int kMinRefreshIntervalMs = 100;
inline constexpr int kMaxRefreshIntervalMs = 10000;        // 10 seconds
inline constexpr int kMinSampleIntervalMs = 100;
inline constexpr int kMaxSampleIntervalMs = 10000;
inline constexpr int kMinHistoryDurationSeconds = 10;
inline constexpr int kMaxHistoryDurationSeconds = 3600;    // 1 hour
inline constexpr int kMinHistorySamples = 10;
inline constexpr int kMaxHistorySamples = 3600;            // upper bound for memory
inline constexpr int kMinCooldownSeconds = 0;
inline constexpr int kMaxCooldownSeconds = 3600;           // 1 hour
inline constexpr int kMinNotificationTimeoutMs = 1000;
inline constexpr int kMaxNotificationTimeoutMs = 60000;    // 1 minute
inline constexpr int kMinRecoveryHysteresis = 0;
inline constexpr int kMaxRecoveryHysteresis = 50;
inline constexpr double kMaxPercentThreshold = 100.0;
inline constexpr double kMaxTemperatureWarning = 120.0;
inline constexpr double kMaxTemperatureCritical = 150.0;

/// Settings that affect the main monitoring loop and general behaviour.
struct GeneralSettings {
  int refresh_interval_ms = 1000;    // main loop cadence (default 1 s)
  std::string default_page = "list"; // starting page: "list" or "tree"
};

/// Resource-history settings.
struct HistorySettings {
  int sample_interval_ms = 1000;      // nominal sampling cadence
  int history_duration_seconds = 120; // nominal retention window (2 min default)
  int max_samples = 120;              // ring-buffer bound actually used
};

/// One configurable alert category (enabled + warning/critical thresholds).
struct AlertCategorySettings {
  bool enabled = true;
  double warning = 0.0;
  double critical = 0.0;
};

/// Alert thresholds for the five user-facing categories. Recovery is derived
/// from `warning - recovery_hysteresis` so the three values can never
/// contradict each other in a configuration file.
struct AlertSettings {
  AlertCategorySettings cpu;
  AlertCategorySettings memory;
  AlertCategorySettings swap;
  AlertCategorySettings disk;
  AlertCategorySettings temperature;
  int recovery_hysteresis = 5;  // recovery = warning - hysteresis
};

/// Desktop-notification behaviour. Mirrors the fields used by the existing
/// NotificationManager so settings can be applied 1:1 at runtime.
struct NotificationSettings {
  bool enabled = false;              // master switch
  bool warning_notifications = false;  // push Warning notifications
  bool critical_notifications = true;  // push Critical notifications
  bool recovery_notifications = false; // push return-to-Normal events
  int cooldown_seconds = 60;           // per-source cooldown
  int timeout_ms = 10000;              // notification display duration
};

/// Package-manager preferences. Deliberately contains only detection/read
/// preferences. There is intentionally no "auto_install", "auto_upgrade" or
/// "sudo" field anywhere in the configuration model: package operations always
/// require explicit user confirmation in the package page.
struct PackageSettings {
  bool check_for_updates = true;  // allow the startup update check
};

/// The complete persistent configuration (single source of truth). All values
/// are validated before use; invalid values never crash the application.
struct AppSettings {
  int config_version = kCurrentConfigVersion;

  GeneralSettings general;
  HistorySettings history;
  AlertSettings alerts;
  NotificationSettings notifications;
  PackageSettings packages;

  /// Sensible defaults; used for first launch, migration fallback and reset.
  static AppSettings defaults();

  /// Returns a list of problems; empty when the settings are valid.
  [[nodiscard]] std::vector<std::string> validate() const;

  /// Clamps every value into its allowed range and repairs contradictory
  /// relationships (e.g. warning >= critical). Returns the list of corrections
  /// that were applied, for logging. After this the settings are valid.
  std::vector<std::string> clampAndFix();
};

/// Serializes settings to the TOML-subset text format (tables, key = value,
/// '#' comments). Deterministic ordering so files diff cleanly.
std::string serializeSettings(const AppSettings &settings);

/// Parses settings from the TOML-subset text format (as written by
/// serializeSettings). Returns false when the content cannot be trusted as a
/// configuration file at all (the caller falls back to defaults). `problems`
/// receives human-readable notes for malformed/unknown fields; those fields
/// keep their defaults. Never throws.
bool parseSettings(std::string_view text, AppSettings &out,
                   std::vector<std::string> &problems);

}  // namespace atm::cfg