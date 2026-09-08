#include "settings.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <sstream>

namespace atm::cfg {
namespace {

int clampInt(int value, int lo, int hi) {
  return value < lo ? lo : (value > hi ? hi : value);
}

double clampDouble(double value, double lo, double hi) {
  return value < lo ? lo : (value > hi ? hi : value);
}

std::string boolText(bool value) { return value ? "true" : "false"; }

std::string trim(const std::string &input) {
  const std::size_t first = input.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return std::string();
  }
  const std::size_t last = input.find_last_not_of(" \t\r\n");
  return input.substr(first, last - first + 1);
}

bool isIdentifierChar(char ch) {
  return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '.';
}

/// Applies one parsed [section]key = value entry to the settings model.
/// Unknown keys are ignored (forward compatibility); out-of-range values are
/// clamped so a hostile/broken file can never produce dangerous values.
void applyEntry(AppSettings &s, const std::string &section,
                const std::string &key, const std::string &raw,
                std::vector<std::string> &problems) {
  const std::string location =
      section.empty() ? key : section + "." + key;

  const auto addProblem = [&](const char *reason) {
    problems.push_back(std::string(reason) + " '" + location + "'");
  };

  if (section.empty()) {
    if (key == "config_version") {
      char *end = nullptr;
      errno = 0;
      const long v = std::strtol(raw.c_str(), &end, 10);
      if (errno == 0 && end != nullptr && *end == '\0' && v > 0) {
        s.config_version = static_cast<int>(v);
      } else {
        addProblem("invalid config value for");
      }
      return;
    }
    addProblem("unrecognized setting (ignored)");
    return;
  }

  const auto setInt = [&](int &field, int lo, int hi) {
    char *end = nullptr;
    errno = 0;
    const long v = std::strtol(raw.c_str(), &end, 10);
    if (errno == 0 && end != nullptr && *end == '\0') {
      field = clampInt(static_cast<int>(v), lo, hi);
    } else {
      addProblem("invalid config value for");
    }
  };
  const auto setDouble = [&](double &field, double lo, double hi) {
    char *end = nullptr;
    errno = 0;
    const double v = std::strtod(raw.c_str(), &end);
    if (errno == 0 && end != nullptr && *end == '\0') {
      field = clampDouble(v, lo, hi);
    } else {
      addProblem("invalid config value for");
    }
  };
  const auto setBool = [&](bool &field) {
    const std::string v = trim(raw);
    if (v == "true" || v == "1" || v == "yes") {
      field = true;
    } else if (v == "false" || v == "0" || v == "no") {
      field = false;
    } else {
      addProblem("invalid config value for");
    }
  };
  const auto setString = [&](std::string &field) {
    std::string v = trim(raw);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
      v = v.substr(1, v.size() - 2);
    }
    if (!v.empty()) {
      field = v;
    } else {
      addProblem("invalid config value for");
    }
  };

  if (section == "general") {
    if (key == "refresh_interval_ms")
      setInt(s.general.refresh_interval_ms, kMinRefreshIntervalMs,
             kMaxRefreshIntervalMs);
    else if (key == "default_page")
      setString(s.general.default_page);
    else
      addProblem("unrecognized setting (ignored)");
    return;
  }

  if (section == "history") {
    if (key == "sample_interval_ms")
      setInt(s.history.sample_interval_ms, kMinSampleIntervalMs,
             kMaxSampleIntervalMs);
    else if (key == "history_duration_seconds")
      setInt(s.history.history_duration_seconds, kMinHistoryDurationSeconds,
             kMaxHistoryDurationSeconds);
    else if (key == "max_samples")
      setInt(s.history.max_samples, kMinHistorySamples, kMaxHistorySamples);
    else
      addProblem("unrecognized setting (ignored)");
    return;
  }

  if (section == "alerts") {
    if (key == "recovery_hysteresis")
      setInt(s.alerts.recovery_hysteresis, kMinRecoveryHysteresis,
             kMaxRecoveryHysteresis);
    else
      addProblem("unrecognized setting (ignored)");
    return;
  }

  auto applyCategory = [&](AlertCategorySettings &cat) {
    if (key == "enabled")
      setBool(cat.enabled);
    else if (key == "warning") {
      const double max_warn =
          section == "alerts.temperature" ? kMaxTemperatureWarning
                                          : kMaxPercentThreshold;
      setDouble(cat.warning, 0.0, max_warn);
    } else if (key == "critical") {
      const double max_crit =
          section == "alerts.temperature" ? kMaxTemperatureCritical
                                          : kMaxPercentThreshold;
      setDouble(cat.critical, 0.0, max_crit);
    } else {
      addProblem("unrecognized setting (ignored)");
    }
  };

  if (section == "alerts.cpu") {
    applyCategory(s.alerts.cpu);
    return;
  }
  if (section == "alerts.memory") {
    applyCategory(s.alerts.memory);
    return;
  }
  if (section == "alerts.swap") {
    applyCategory(s.alerts.swap);
    return;
  }
  if (section == "alerts.disk") {
    applyCategory(s.alerts.disk);
    return;
  }
  if (section == "alerts.temperature") {
    applyCategory(s.alerts.temperature);
    return;
  }

  if (section == "notifications") {
    if (key == "enabled")
      setBool(s.notifications.enabled);
    else if (key == "warning_notifications")
      setBool(s.notifications.warning_notifications);
    else if (key == "critical_notifications")
      setBool(s.notifications.critical_notifications);
    else if (key == "recovery_notifications")
      setBool(s.notifications.recovery_notifications);
    else if (key == "cooldown_seconds")
      setInt(s.notifications.cooldown_seconds, kMinCooldownSeconds,
             kMaxCooldownSeconds);
    else if (key == "timeout_ms")
      setInt(s.notifications.timeout_ms, kMinNotificationTimeoutMs,
             kMaxNotificationTimeoutMs);
    else
      addProblem("unrecognized setting (ignored)");
    return;
  }

  if (section == "packages") {
    if (key == "check_for_updates")
      setBool(s.packages.check_for_updates);
    else
      addProblem("unrecognized setting (ignored)");
    return;
  }

  addProblem("unrecognized setting (ignored)");
}

}  // namespace

AppSettings AppSettings::defaults() {
  AppSettings s;
  s.config_version = kCurrentConfigVersion;

  s.general.refresh_interval_ms = 1000;
  s.general.default_page = "list";

  s.history.sample_interval_ms = 1000;
  s.history.history_duration_seconds = 120;
  s.history.max_samples = 120;

  s.alerts.recovery_hysteresis = 5;
  s.alerts.cpu = {true, 80.0, 95.0};
  s.alerts.memory = {true, 80.0, 95.0};
  s.alerts.swap = {true, 70.0, 90.0};
  s.alerts.disk = {true, 85.0, 95.0};
  s.alerts.temperature = {true, 75.0, 90.0};

  s.notifications.enabled = false;
  s.notifications.warning_notifications = false;
  s.notifications.critical_notifications = true;
  s.notifications.recovery_notifications = false;
  s.notifications.cooldown_seconds = 60;
  s.notifications.timeout_ms = 10000;

  s.packages.check_for_updates = true;
  return s;
}

std::vector<std::string> AppSettings::validate() const {
  std::vector<std::string> problems;

  const auto range = [&problems](const std::string &name, int value, int lo,
                                 int hi) {
    if (value < lo || value > hi) {
      problems.push_back(name + " out of range [" + std::to_string(lo) + "-" +
                         std::to_string(hi) + "]");
    }
  };
  const auto category = [&problems](const std::string &name,
                                    const AlertCategorySettings &cat,
                                    double warn_max, double crit_max) {
    if (cat.warning < 0.0 || cat.warning > warn_max) {
      problems.push_back(name + ".warning out of range");
    }
    if (cat.critical < 0.0 || cat.critical > crit_max) {
      problems.push_back(name + ".critical out of range");
    }
    if (cat.warning >= cat.critical) {
      problems.push_back(name + ": warning must be lower than critical");
    }
  };

  if (config_version != kCurrentConfigVersion) {
    problems.push_back("config_version is " + std::to_string(config_version) +
                       ", expected " + std::to_string(kCurrentConfigVersion));
  }
  range("general.refresh_interval_ms", general.refresh_interval_ms,
        kMinRefreshIntervalMs, kMaxRefreshIntervalMs);
  if (general.default_page != "list" && general.default_page != "tree") {
    problems.push_back("general.default_page must be 'list' or 'tree'");
  }
  range("history.sample_interval_ms", history.sample_interval_ms,
        kMinSampleIntervalMs, kMaxSampleIntervalMs);
  range("history.history_duration_seconds", history.history_duration_seconds,
        kMinHistoryDurationSeconds, kMaxHistoryDurationSeconds);
  range("history.max_samples", history.max_samples, kMinHistorySamples,
        kMaxHistorySamples);
  range("alerts.recovery_hysteresis", alerts.recovery_hysteresis,
        kMinRecoveryHysteresis, kMaxRecoveryHysteresis);
  category("alerts.cpu", alerts.cpu, kMaxPercentThreshold,
           kMaxPercentThreshold);
  category("alerts.memory", alerts.memory, kMaxPercentThreshold,
           kMaxPercentThreshold);
  category("alerts.swap", alerts.swap, kMaxPercentThreshold,
           kMaxPercentThreshold);
  category("alerts.disk", alerts.disk, kMaxPercentThreshold,
           kMaxPercentThreshold);
  category("alerts.temperature", alerts.temperature, kMaxTemperatureWarning,
           kMaxTemperatureCritical);
  range("notifications.cooldown_seconds", notifications.cooldown_seconds,
        kMinCooldownSeconds, kMaxCooldownSeconds);
  range("notifications.timeout_ms", notifications.timeout_ms,
        kMinNotificationTimeoutMs, kMaxNotificationTimeoutMs);
  return problems;
}

std::vector<std::string> AppSettings::clampAndFix() {
  std::vector<std::string> corrections;
  const auto note = [&corrections](const std::string &message) {
    corrections.push_back(message);
  };

  const auto clampIntField = [&note](int &value, int lo, int hi,
                                     const char *name) {
    if (value < lo || value > hi) {
      value = clampInt(value, lo, hi);
      note(std::string("adjusted out-of-range setting: ") + name);
    }
  };

  clampIntField(general.refresh_interval_ms, kMinRefreshIntervalMs,
                kMaxRefreshIntervalMs, "general.refresh_interval_ms");
  if (general.default_page != "list" && general.default_page != "tree") {
    general.default_page = "list";
    note("adjusted setting: general.default_page");
  }
  clampIntField(history.sample_interval_ms, kMinSampleIntervalMs,
                kMaxSampleIntervalMs, "history.sample_interval_ms");
  clampIntField(history.history_duration_seconds, kMinHistoryDurationSeconds,
                kMaxHistoryDurationSeconds, "history.history_duration_seconds");
  clampIntField(history.max_samples, kMinHistorySamples, kMaxHistorySamples,
                "history.max_samples");
  clampIntField(alerts.recovery_hysteresis, kMinRecoveryHysteresis,
                kMaxRecoveryHysteresis, "alerts.recovery_hysteresis");

  const auto fixCategory = [&note](AlertCategorySettings &cat,
                                   const char *name, double warn_max,
                                   double crit_max,
                                   const AlertCategorySettings &fallback) {
    bool changed = cat.warning < 0.0 || cat.warning > warn_max ||
                   cat.critical < 0.0 || cat.critical > crit_max;
    cat.warning = clampDouble(cat.warning, 0.0, warn_max);
    cat.critical = clampDouble(cat.critical, 0.0, crit_max);
    if (cat.warning >= cat.critical) {
      cat = fallback;  // relationship cannot be repaired predictably
      note(std::string("invalid threshold relationship reset to defaults: ") +
           name);
    } else if (changed) {
      note(std::string("adjusted out-of-range setting: ") + name);
    }
  };

  fixCategory(alerts.cpu, "alerts.cpu", kMaxPercentThreshold,
              kMaxPercentThreshold, {true, 80.0, 95.0});
  fixCategory(alerts.memory, "alerts.memory", kMaxPercentThreshold,
              kMaxPercentThreshold, {true, 80.0, 95.0});
  fixCategory(alerts.swap, "alerts.swap", kMaxPercentThreshold,
              kMaxPercentThreshold, {true, 70.0, 90.0});
  fixCategory(alerts.disk, "alerts.disk", kMaxPercentThreshold,
              kMaxPercentThreshold, {true, 85.0, 95.0});
  fixCategory(alerts.temperature, "alerts.temperature", kMaxTemperatureWarning,
              kMaxTemperatureCritical, {true, 75.0, 90.0});

  clampIntField(notifications.cooldown_seconds, kMinCooldownSeconds,
                kMaxCooldownSeconds, "notifications.cooldown_seconds");
  clampIntField(notifications.timeout_ms, kMinNotificationTimeoutMs,
                kMaxNotificationTimeoutMs, "notifications.timeout_ms");
  return corrections;
}

std::string serializeSettings(const AppSettings &s) {
  std::ostringstream out;
  out << "# arch-task-manager configuration\n"
         "# This file contains no passwords or secrets.\n\n"
      << "config_version = " << s.config_version << "\n\n";

  out << "[general]\n"
      << "refresh_interval_ms = " << s.general.refresh_interval_ms << '\n'
      << "default_page = \"" << s.general.default_page << "\"\n\n";

  out << "[history]\n"
      << "sample_interval_ms = " << s.history.sample_interval_ms << '\n'
      << "history_duration_seconds = " << s.history.history_duration_seconds
      << '\n'
      << "max_samples = " << s.history.max_samples << "\n\n";

  out << "[alerts]\n"
      << "recovery_hysteresis = " << s.alerts.recovery_hysteresis << '\n';

  const auto writeCategory = [&out](const char *name,
                                    const AlertCategorySettings &cat) {
    out << "\n[" << name << "]\n"
        << "enabled = " << boolText(cat.enabled) << '\n'
        << "warning = " << cat.warning << '\n'
        << "critical = " << cat.critical << '\n';
  };
  writeCategory("alerts.cpu", s.alerts.cpu);
  writeCategory("alerts.memory", s.alerts.memory);
  writeCategory("alerts.swap", s.alerts.swap);
  writeCategory("alerts.disk", s.alerts.disk);
  writeCategory("alerts.temperature", s.alerts.temperature);

  out << "\n[notifications]\n"
      << "enabled = " << boolText(s.notifications.enabled) << '\n'
      << "warning_notifications = "
      << boolText(s.notifications.warning_notifications) << '\n'
      << "critical_notifications = "
      << boolText(s.notifications.critical_notifications) << '\n'
      << "recovery_notifications = "
      << boolText(s.notifications.recovery_notifications) << '\n'
      << "cooldown_seconds = " << s.notifications.cooldown_seconds << '\n'
      << "timeout_ms = " << s.notifications.timeout_ms << "\n\n";

  out << "[packages]\n"
      << "check_for_updates = " << boolText(s.packages.check_for_updates)
      << '\n';
  return out.str();
}

bool parseSettings(std::string_view text, AppSettings &out,
                   std::vector<std::string> &problems) {
  out = AppSettings::defaults();
  problems.clear();

  struct Entry {
    std::string section;
    std::string key;
    std::string value;
  };
  std::vector<Entry> entries;
  std::string current_section;
  std::size_t content_lines = 0;

  std::istringstream stream{std::string(text)};
  std::string line;
  while (std::getline(stream, line)) {
    const std::string trimmed = trim(line);
    if (trimmed.empty() || trimmed[0] == '#') {
      continue;
    }
    ++content_lines;

    if (trimmed[0] == '[') {
      if (trimmed.back() != ']') {
        problems.push_back("malformed section header: '" + trimmed + "'");
        continue;
      }
      const std::string name = trimmed.substr(1, trimmed.size() - 2);
      const bool valid =
          !name.empty() &&
          std::all_of(name.begin(), name.end(),
                      [](char ch) { return isIdentifierChar(ch); });
      if (!valid) {
        problems.push_back("invalid section header: '" + trimmed + "'");
        continue;
      }
      current_section = name;
      continue;
    }

    const std::size_t eq = trimmed.find('=');
    if (eq == std::string::npos) {
      problems.push_back("malformed line (expected key = value): '" +
                         trimmed + "'");
      continue;
    }
    const std::string key = trim(trimmed.substr(0, eq));
    const std::string value = trim(trimmed.substr(eq + 1));
    const bool valid_key =
        !key.empty() &&
        std::all_of(key.begin(), key.end(),
                    [](char ch) { return isIdentifierChar(ch); });
    if (!valid_key || value.empty() || value[0] == '#') {
      problems.push_back("malformed assignment: '" + trimmed + "'");
      continue;
    }
    entries.push_back({current_section, key, value});
  }

  // A file with content but zero readable assignments cannot be trusted as a
  // configuration file; signal failure so the caller falls back to defaults.
  if (content_lines > 0 && entries.empty()) {
    problems.push_back("configuration contains no readable settings");
    return false;
  }

  for (const Entry &entry : entries) {
    applyEntry(out, entry.section, entry.key, entry.value, problems);
  }
  return true;
}

}  // namespace atm::cfg