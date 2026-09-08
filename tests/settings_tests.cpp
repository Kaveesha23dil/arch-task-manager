#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "alert_manager.hpp"
#include "history_manager.hpp"
#include "notification_manager.hpp"
#include "settings.hpp"
#include "settings_apply.hpp"
#include "settings_manager.hpp"

// --- Minimal standalone test harness (no external framework) -------------
namespace {
int g_checks = 0;
int g_failures = 0;

void expect(bool condition, const char *expr, const char *file, int line) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
  }
}

void expectNear(double a, double b, double eps, const char *expr,
                const char *file, int line) {
  ++g_checks;
  if (!(a > b - eps && a < b + eps)) {
    ++g_failures;
    std::fprintf(stderr, "FAIL %s:%d: %s (%f vs %f)\n", file, line, expr, a, b);
  }
}

void run(const char *name) {
  std::fprintf(stderr, "TEST %s\n", name);
}

std::filesystem::path makeTempDir() {
  static int counter = 0;
  const std::filesystem::path base =
      std::filesystem::temp_directory_path() /
      ("arch-task-manager-test-" + std::to_string(::getpid()) + "-" +
       std::to_string(counter++));
  std::error_code ec;
  std::filesystem::create_directories(base, ec);
  return base;
}
}  // namespace

#define CHECK(expr) ::expect((expr), #expr, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, eps) \
  ::expectNear(double(a), double(b), (eps), #a " ~ " #b, __FILE__, __LINE__)

using atm::cfg::AppSettings;
using atm::cfg::SettingsManager;

int main() {
  run("defaults produce valid settings");
  {
    const AppSettings s = AppSettings::defaults();
    CHECK(s.validate().empty());
    CHECK(s.config_version == atm::cfg::kCurrentConfigVersion);
    CHECK(s.general.refresh_interval_ms == 1000);
    CHECK(s.general.default_page == "list");
    CHECK(s.history.max_samples == 120);
    CHECK(s.alerts.cpu.warning < s.alerts.cpu.critical);
    CHECK(s.packages.check_for_updates);
    CHECK(!s.general.autostart_enabled);  // autostart is off by default
  }

  run("serialize/parse round trip preserves values");
  {
    AppSettings s = AppSettings::defaults();
    s.general.refresh_interval_ms = 2500;
    s.general.default_page = "tree";
    s.general.autostart_enabled = true;
    s.history.max_samples = 300;
    s.alerts.cpu.warning = 70.0;
    s.alerts.cpu.critical = 92.0;
    s.notifications.enabled = true;
    s.notifications.cooldown_seconds = 5;
    s.packages.check_for_updates = false;

    const std::string text = atm::cfg::serializeSettings(s);
    AppSettings parsed;
    std::vector<std::string> problems;
    CHECK(atm::cfg::parseSettings(text, parsed, problems));
    CHECK(problems.empty());
    CHECK(parsed.general.refresh_interval_ms == 2500);
    CHECK(parsed.general.default_page == "tree");
    CHECK(parsed.general.autostart_enabled);
    CHECK(parsed.history.max_samples == 300);
    CHECK_NEAR(parsed.alerts.cpu.warning, 70.0, 1e-6);
    CHECK_NEAR(parsed.alerts.cpu.critical, 92.0, 1e-6);
    CHECK(parsed.notifications.enabled);
    CHECK(parsed.notifications.cooldown_seconds == 5);
    CHECK(!parsed.packages.check_for_updates);
  }

  run("validation rejects invalid values");
  {
    AppSettings s = AppSettings::defaults();
    s.general.refresh_interval_ms = 0;
    CHECK(!s.validate().empty());
    s = AppSettings::defaults();
    s.general.refresh_interval_ms = -5;
    CHECK(!s.validate().empty());
    s = AppSettings::defaults();
    s.general.refresh_interval_ms = 100000;
    CHECK(!s.validate().empty());
    s = AppSettings::defaults();
    s.history.history_duration_seconds = -1;
    CHECK(!s.validate().empty());
    s = AppSettings::defaults();
    s.alerts.cpu.warning = 95.0;
    s.alerts.cpu.critical = 90.0;  // warning >= critical
    CHECK(!s.validate().empty());
    s = AppSettings::defaults();
    s.notifications.cooldown_seconds = -1;
    CHECK(!s.validate().empty());
    s = AppSettings::defaults();
    s.history.max_samples = 1000000;
    CHECK(!s.validate().empty());
    s = AppSettings::defaults();
    s.alerts.temperature.warning = 200.0;
    CHECK(!s.validate().empty());
  }

  run("clampAndFix repairs invalid values and relationships");
  {
    AppSettings s = AppSettings::defaults();
    s.general.refresh_interval_ms = 50000;
    s.history.max_samples = -3;
    s.notifications.cooldown_seconds = -10;
    s.alerts.cpu.warning = 0.5;
    s.alerts.cpu.critical = 0.5;  // equal -> relationship broken
    const auto corrections = s.clampAndFix();
    CHECK(!corrections.empty());
    CHECK(s.validate().empty());
    CHECK(s.general.refresh_interval_ms == atm::cfg::kMaxRefreshIntervalMs);
    CHECK(s.history.max_samples == atm::cfg::kMinHistorySamples);
    CHECK(s.notifications.cooldown_seconds == atm::cfg::kMinCooldownSeconds);
    CHECK(s.alerts.cpu.warning < s.alerts.cpu.critical);
  }

  run("malformed configuration does not crash and falls back safely");
  {
    AppSettings out;
    std::vector<std::string> problems;
    const std::string garbage = "%%% not a configuration\n@@@@\n";
    CHECK(!atm::cfg::parseSettings(garbage, out, problems));
    CHECK(out.general.refresh_interval_ms == 1000);  // defaults survive

    // Partial file: readable keys are applied, broken values keep defaults.
    AppSettings out2;
    std::vector<std::string> problems2;
    const std::string partial =
        "config_version = 1\n[general]\nrefresh_interval_ms = nonsense\n"
        "[alerts.cpu]\nwarning = 80\ncritical = 95\n";
    CHECK(atm::cfg::parseSettings(partial, out2, problems2));
    CHECK(!problems2.empty());
    CHECK(out2.general.refresh_interval_ms == 1000);
    CHECK_NEAR(out2.alerts.cpu.warning, 80.0, 1e-6);
  }

  run("first launch creates defaults, then persists changes");
  {
    const auto dir = makeTempDir();
    const auto path = dir / "config.toml";
    {
      SettingsManager manager(path);
      CHECK(manager.configPath() == path);
      manager.load();
      CHECK(std::filesystem::exists(path));  // defaults written on first launch
      CHECK(manager.settings().validate().empty());
      AppSettings next = manager.settings();
      next.general.refresh_interval_ms = 2000;
      next.general.autostart_enabled = true;
      manager.updateSettings(next);
      CHECK(manager.isDirty());
      CHECK(manager.save());
      CHECK(!manager.isDirty());
    }
    {
      SettingsManager manager(path);
      manager.load();
      CHECK(manager.settings().general.refresh_interval_ms == 2000);
      CHECK(manager.settings().general.autostart_enabled);  // survives reload
      CHECK(!manager.isDirty());
    }
  }

  run("reset restores the autostart preference to disabled");
  {
    const auto dir = makeTempDir();
    SettingsManager manager(dir / "config.toml");
    manager.load();
    AppSettings next = manager.settings();
    next.general.autostart_enabled = true;
    manager.updateSettings(next);
    CHECK(manager.settings().general.autostart_enabled);
    manager.resetToDefaults();
    CHECK(!manager.settings().general.autostart_enabled);
    CHECK(manager.settings().validate().empty());
    CHECK(manager.isDirty());
  }

  run("reset restores defaults and marks dirty");
  {
    const auto dir = makeTempDir();
    SettingsManager manager(dir / "config.toml");
    manager.load();
    AppSettings next = manager.settings();
    next.general.refresh_interval_ms = 3333;
    manager.updateSettings(next);
    CHECK(manager.settings().general.refresh_interval_ms == 3333);
    manager.resetToDefaults();
    CHECK(manager.settings().general.refresh_interval_ms == 1000);
    CHECK(manager.settings().validate().empty());
    CHECK(manager.isDirty());
  }

  run("updateSettings rejects nothing destructive and always clamps");
  {
    const auto dir = makeTempDir();
    SettingsManager manager(dir / "config.toml");
    AppSettings bad = AppSettings::defaults();
    bad.general.refresh_interval_ms = 999999;
    bad.alerts.disk.warning = 99.0;
    bad.alerts.disk.critical = 10.0;  // warning > critical
    CHECK(manager.updateSettings(bad));
    CHECK(manager.settings().validate().empty());
    CHECK(manager.settings().general.refresh_interval_ms ==
          atm::cfg::kMaxRefreshIntervalMs);
    CHECK(manager.settings().alerts.disk.warning <
          manager.settings().alerts.disk.critical);
    CHECK(manager.isDirty());
  }

  run("XDG_CONFIG_HOME overrides $HOME/.config");
  {
    ::setenv("XDG_CONFIG_HOME", "/tmp/arch-task-manager-xdg-test", 1);
    const auto p = SettingsManager::defaultConfigPath();
    CHECK(p == std::filesystem::path(
                   "/tmp/arch-task-manager-xdg-test/arch-task-manager/"
                   "config.toml"));

    ::unsetenv("XDG_CONFIG_HOME");
    const char *home = ::getenv("HOME");
    CHECK(home != nullptr && *home != '\0');
    const auto p2 = SettingsManager::defaultConfigPath();
    CHECK(p2 == std::filesystem::path(home) / ".config" /
                     "arch-task-manager" / "config.toml");
  }

  run("unversioned/older config files are migrated");
  {
    const auto dir = makeTempDir();
    const auto path = dir / "config.toml";
    std::ofstream out(path);
    out << "# old unversioned configuration\n"
           "[general]\nrefresh_interval_ms = 1500\n";
    out.close();
    SettingsManager manager(path);
    manager.load();
    CHECK(manager.settings().general.refresh_interval_ms == 1500);
    CHECK(manager.settings().config_version == atm::cfg::kCurrentConfigVersion);
  }

  run("unwritable configuration keeps the application usable");
  {
    if (::geteuid() == 0) {
      std::fprintf(stderr, "  (skipped: running as root)\n");
      ++g_checks;
    } else {
      const auto dir = makeTempDir();
      ::chmod(dir.c_str(), 0500);  // read-only directory
      SettingsManager manager(dir / "config.toml");
      manager.load();  // must not crash; defaults remain usable
      CHECK(manager.settings().general.refresh_interval_ms == 1000);
      CHECK(manager.settings().validate().empty());

      AppSettings next = manager.settings();
      next.general.refresh_interval_ms = 2500;
      manager.updateSettings(next);
      CHECK(!manager.save());  // gracefully refuses
      CHECK(manager.settings().general.refresh_interval_ms == 2500);
      ::chmod(dir.c_str(), 0700);  // allow cleanup
    }
  }

  run("settings apply to the runtime components");
  {
    atm::HistoryManager history;
    atm::AlertManager alerts;
    atm::NotificationManager notifications;
    int refresh_interval_ms = 1000;

    AppSettings s = AppSettings::defaults();
    s.general.refresh_interval_ms = 2000;
    s.history.max_samples = 300;
    s.alerts.cpu.warning = 88.0;
    s.alerts.cpu.critical = 99.0;
    s.notifications.enabled = true;
    s.notifications.cooldown_seconds = 7;
    s.notifications.warning_notifications = true;

    atm::cfg::applySettingsToRuntime(s, refresh_interval_ms, history, alerts,
                                     notifications);
    CHECK(refresh_interval_ms == 2000);
    CHECK(history.maxSamples() == 300);
    CHECK_NEAR(alerts.threshold(atm::AlertType::CpuUsage).warning, 88.0, 1e-6);
    CHECK_NEAR(alerts.threshold(atm::AlertType::CpuUsage).critical, 99.0, 1e-6);
    CHECK(alerts.threshold(atm::AlertType::CpuUsage).enabled);
    CHECK_NEAR(alerts.threshold(atm::AlertType::CpuUsage).recovery, 83.0, 1e-6);
    CHECK_NEAR(alerts.threshold(atm::AlertType::Temperature).warning, 75.0,
               1e-6);  // non-configured categories keep their defaults
    CHECK(notifications.settings().enabled);
    CHECK(notifications.settings().notify_on_warning);
    CHECK(notifications.settings().cooldown == std::chrono::seconds(7));
  }

  run("package settings are check-only and never install anything");
  {
    const AppSettings s = AppSettings::defaults();
    CHECK(s.packages.check_for_updates);
    // The configuration model exposes no auto-install / auto-upgrade field;
    // package operations remain explicit. Toggling the check only affects
    // that one preference.
    AppSettings t = s;
    t.packages.check_for_updates = false;
    CHECK(!t.packages.check_for_updates);
    CHECK(t.general.refresh_interval_ms == s.general.refresh_interval_ms);
    CHECK(t.validate().empty());
  }

  std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
  if (g_failures != 0) {
    return EXIT_FAILURE;
  }
  std::fprintf(stderr, "ALL TESTS PASSED\n");
  return EXIT_SUCCESS;
}