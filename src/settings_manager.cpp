#include "settings_manager.hpp"

#include <sys/stat.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "logger.hpp"

namespace atm::cfg {

namespace {

/// Atomic write: temporary file -> flush -> close -> rename. The original file
/// is never left half-written on failures. The file is created with user-only
/// permissions (0600); it stores no secrets, but defaults to private anyway.
bool writeSettingsFile(const std::filesystem::path &path,
                       const AppSettings &settings) {
  const std::filesystem::path tmp =
      std::filesystem::path(path).string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::out | std::ios::trunc);
    if (!out) {
      return false;
    }
    out << serializeSettings(settings);
    out.flush();
    if (!out) {
      return false;  // disk full / IO error
    }
  }

  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
    return false;
  }
  (void)::chmod(path.c_str(), S_IRUSR | S_IWUSR);
  return true;
}

}  // namespace

SettingsManager::SettingsManager(std::filesystem::path path)
    : path_(std::move(path)), settings_(AppSettings::defaults()) {}

std::filesystem::path SettingsManager::defaultConfigPath() {
  std::filesystem::path base;
  if (const char *xdg = std::getenv("XDG_CONFIG_HOME");
      xdg != nullptr && *xdg != '\0') {
    base = std::filesystem::path(xdg);
  } else if (const char *home = std::getenv("HOME");
             home != nullptr && *home != '\0') {
    base = std::filesystem::path(home) / ".config";
  }
  if (base.empty()) {
    return {};  // no XDG_CONFIG_HOME and no HOME: cannot determine a location
  }
  return base / "arch-task-manager" / "config.toml";
}

bool SettingsManager::load() {
  std::lock_guard<std::mutex> lock(mutex_);
  problems_.clear();
  settings_ = AppSettings::defaults();

  if (path_.empty()) {
    Logger::warn("Configuration path unavailable; continuing with defaults");
    return true;
  }
  Logger::info("Loading configuration: " + path_.string());

  std::error_code ec;
  if (!std::filesystem::exists(path_, ec) || ec) {
    Logger::info("Configuration file not found, using defaults");
    dirty_ = true;  // first launch: defaults should be persisted
    try {
      std::filesystem::create_directories(path_.parent_path(), ec);
      if (ec) {
        Logger::error("Configuration directory could not be created: " +
                      path_.parent_path().string());
        return true;  // continue with in-memory defaults
      }
    } catch (const std::filesystem::filesystem_error &) {
      Logger::error("Configuration directory could not be created");
      return true;
    }
    if (writeSettingsFile(path_, settings_)) {
      dirty_ = false;
      Logger::info("Created configuration: " + path_.string());
    } else {
      Logger::error("Failed to save configuration (in-memory defaults remain "
                    "active): " +
                    path_.string());
    }
    return true;
  }

  std::ifstream in(path_);
  if (!in) {
    Logger::error("Configuration file could not be read: " + path_.string());
    dirty_ = true;
    return true;  // keep running with defaults
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();

  AppSettings parsed;
  std::vector<std::string> parse_problems;
  if (!parseSettings(buffer.str(), parsed, parse_problems)) {
    Logger::error("Configuration file could not be parsed, using defaults: " +
                  path_.string());
    settings_ = AppSettings::defaults();
    problems_ = std::move(parse_problems);
    dirty_ = true;
    return true;
  }
  for (const std::string &p : parse_problems) {
    problems_.push_back(p);
  }

  if (parsed.config_version != kCurrentConfigVersion) {
    if (migrateConfigUnlocked(parsed.config_version)) {
      parsed.config_version = kCurrentConfigVersion;
      dirty_ = true;
    }
  }

  for (const std::string &correction : parsed.clampAndFix()) {
    problems_.push_back(correction);
  }
  if (!parsed.validate().empty()) {
    Logger::warn("Configuration validation failed, using defaults");
    settings_ = AppSettings::defaults();
    dirty_ = true;
    return true;
  }

  settings_ = parsed;
  Logger::info("Configuration loaded successfully: " + path_.string());
  if (!problems_.empty()) {
    Logger::warn("Configuration file contains invalid values; defaults used "
                 "for the affected settings");
  }
  return true;
}

bool SettingsManager::save() {
  AppSettings snapshot;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!dirty_) {
      return true;  // nothing changed; avoid unnecessary writes
    }
    snapshot = settings_;
  }

  if (path_.empty()) {
    Logger::error("Failed to save configuration: no config path available");
    return false;
  }
  Logger::info("Saving configuration: " + path_.string());

  std::error_code ec;
  try {
    std::filesystem::create_directories(path_.parent_path(), ec);
  } catch (const std::filesystem::filesystem_error &) {
    ec = std::make_error_code(std::errc::io_error);
  }
  if (ec) {
    Logger::error("Failed to save configuration: " + path_.string() + " (" +
                  ec.message() + ")");
    return false;
  }

  if (!writeSettingsFile(path_, snapshot)) {
    Logger::error("Failed to save configuration: " + path_.string());
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    dirty_ = false;
  }
  Logger::info("Configuration saved successfully");
  return true;
}

void SettingsManager::resetToDefaults() {
  std::lock_guard<std::mutex> lock(mutex_);
  settings_ = AppSettings::defaults();
  problems_.clear();
  dirty_ = true;
  Logger::info("Configuration reset to defaults");
}

bool SettingsManager::updateSettings(const AppSettings &next) {
  std::lock_guard<std::mutex> lock(mutex_);
  settings_ = next;
  settings_.config_version = kCurrentConfigVersion;
  problems_ = settings_.clampAndFix();
  dirty_ = true;
  return settings_.validate().empty();
}

const AppSettings &SettingsManager::settings() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return settings_;
}

const std::vector<std::string> &SettingsManager::problems() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return problems_;
}

bool SettingsManager::isDirty() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return dirty_;
}

const std::filesystem::path &SettingsManager::configPath() const {
  return path_;
}

bool SettingsManager::migrateConfig(int oldVersion) {
  std::lock_guard<std::mutex> lock(mutex_);
  return migrateConfigUnlocked(oldVersion);
}

bool SettingsManager::migrateConfigUnlocked(int oldVersion) {
  if (oldVersion == kCurrentConfigVersion) {
    return true;
  }
  if (oldVersion < 1) {
    Logger::info("Migrating configuration from an unversioned format");
    return true;
  }
  if (oldVersion < kCurrentConfigVersion) {
    Logger::warn("Configuration format v" + std::to_string(oldVersion) +
                 " is not supported; falling back to defaults");
    settings_ = AppSettings::defaults();
    return true;
  }
  // A file from a newer version: keep going with the current schema. Unknown
  // extra keys are ignored by the parser.
  Logger::warn("Configuration format v" + std::to_string(oldVersion) +
               " is newer than this application supports");
  return false;
}

}  // namespace atm::cfg