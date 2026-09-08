#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "settings.hpp"

namespace atm::cfg {

/// Loads, validates, and persists the typed application settings.
///
/// Owns the authoritative copy of AppSettings. Runtime components are handed
/// a snapshot of these values at startup and whenever a setting changes; they
/// never parse the configuration file themselves. Writes are atomic (temporary
/// file + rename), reads never crash on malformed input, and configuration is
/// data only: the manager never executes anything it reads.
class SettingsManager {
 public:
  /// Uses the XDG config path by default; a path may be injected (tests).
  explicit SettingsManager(std::filesystem::path path = defaultConfigPath());

  /// The configuration file following the XDG Base Directory specification:
  /// $XDG_CONFIG_HOME/arch-task-manager/config.toml, falling back to
  /// $HOME/.config/arch-task-manager/config.toml. Returns an empty path when
  /// neither environment variable is set.
  static std::filesystem::path defaultConfigPath();

  /// Loads configuration from disk. Never fatal: on missing, malformed or
  /// unreadable files the application continues with in-memory defaults. On
  /// first launch it creates the config directory and writes the defaults.
  bool load();

  /// Atomically writes the current settings to disk (temporary file, flush,
  /// close, rename). Returns false on any failure; the in-memory settings
  /// remain fully usable. Does nothing when the settings are not dirty.
  bool save();

  /// Restores application defaults in memory and marks the settings dirty.
  /// Does not write to disk (call save() when desired).
  void resetToDefaults();

  /// Replaces the settings with `next`, clamping/repairing values so the
  /// result is always valid, and marks the settings dirty. Returns true.
  bool updateSettings(const AppSettings &next);

  /// The authoritative settings. Read-only by convention; callers must go
  /// through updateSettings() to change values. Single-thread UI access.
  [[nodiscard]] const AppSettings &settings() const;

  /// Latest human-readable notes produced by load()/updateSettings()
  /// (invalid values, migrations, corrections).
  [[nodiscard]] const std::vector<std::string> &problems() const;

  /// True when the in-memory settings differ from what is on disk.
  [[nodiscard]] bool isDirty() const;

  [[nodiscard]] const std::filesystem::path &configPath() const;

  /// Migration hook for older configuration versions. The only shipped format
  /// is version 1, so no migration is required yet, but the hook exists so the
  /// config_version field is actually honoured by the loader.
  bool migrateConfig(int oldVersion);

 private:
  /// Lock-free core of migrateConfig(); the public method and load() take the
  /// mutex before calling it.
  bool migrateConfigUnlocked(int oldVersion);

  mutable std::mutex mutex_;
  std::filesystem::path path_;
  AppSettings settings_;
  bool dirty_ = false;
  std::vector<std::string> problems_;
};

}  // namespace atm::cfg