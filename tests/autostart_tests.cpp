#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "app_autostart_manager.hpp"

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

void run(const char *name) { std::fprintf(stderr, "TEST %s\n", name); }

std::filesystem::path makeTempDir() {
  static int counter = 0;
  const std::filesystem::path base =
      std::filesystem::temp_directory_path() /
      ("arch-task-manager-autostart-test-" + std::to_string(::getpid()) +
       "-" + std::to_string(counter++));
  std::error_code ec;
  std::filesystem::create_directories(base, ec);
  return base;
}

std::string readFile(const std::filesystem::path &path) {
  std::ifstream in(path);
  if (!in) {
    return {};
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

// Returns the unquoted/unescaped value after "Exec=" (may differ from the raw
// path when the desktop-entry escaping produced quotes or backslash escapes).
std::string execLineValue(const std::string &content) {
  std::istringstream lines(content);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.rfind("Exec=", 0) == 0) {
      std::string value = line.substr(5);
      if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
        std::string unescaped;
        for (std::size_t i = 0; i < value.size(); ++i) {
          if (value[i] == '\\' && i + 1 < value.size()) {
            unescaped += value[i + 1];
            ++i;
          } else {
            unescaped += value[i];
          }
        }
        return unescaped;
      }
      return value;
    }
  }
  return {};
}

}  // namespace

#define CHECK(expr) ::expect((expr), #expr, __FILE__, __LINE__)

static void setEnv(const char *name, const char *value) {
  if (value == nullptr) {
    ::unsetenv(name);
  } else {
    ::setenv(name, value, 1);
  }
}

int main() {
  using atm::AppAutostartManager;
  namespace fs = std::filesystem;

  run("path resolution: XDG_CONFIG_HOME honoured (absolute)");
  {
    const std::string home = makeTempDir().string() + "/home-user";
    setEnv("HOME", home.c_str());
    setEnv("XDG_CONFIG_HOME", "/tmp/atm-xdg-test");
    const auto dir = AppAutostartManager::defaultAutostartDirectory();
    CHECK(dir == fs::path("/tmp/atm-xdg-test") / "autostart");
  }

  run("path resolution: relative XDG_CONFIG_HOME falls back to HOME");
  {
    const std::string home = makeTempDir().string() + "/home-user";
    setEnv("HOME", home.c_str());
    setEnv("XDG_CONFIG_HOME", "relative/value");
    const auto dir = AppAutostartManager::defaultAutostartDirectory();
    CHECK(dir == fs::path(home) / ".config" / "autostart");
  }

  run("path resolution: no XDG_CONFIG_HOME uses $HOME/.config/autostart");
  {
    const std::string home = makeTempDir().string() + "/home-user";
    setEnv("HOME", home.c_str());
    setEnv("XDG_CONFIG_HOME", nullptr);
    const auto dir = AppAutostartManager::defaultAutostartDirectory();
    CHECK(dir == fs::path(home) / ".config" / "autostart");
  }

  run("path resolution: neither XDG_CONFIG_HOME nor HOME leaves it unknown");
  {
    const char *saved_home = ::getenv("HOME");
    const char *saved_xdg = ::getenv("XDG_CONFIG_HOME");
    setEnv("HOME", nullptr);
    setEnv("XDG_CONFIG_HOME", nullptr);
    const AppAutostartManager manager;
    CHECK(manager.autostartDirectory().empty());
    CHECK(!manager.isConfigAvailable());
    // restore for later tests
    ::setenv("HOME", saved_home != nullptr ? saved_home : "/tmp", 1);
    if (saved_xdg != nullptr) {
      ::setenv("XDG_CONFIG_HOME", saved_xdg, 1);
    } else {
      ::unsetenv("XDG_CONFIG_HOME");
    }
  }

  run("executable path is absolute and resolves to a real file");
  {
    const std::string exe = AppAutostartManager::currentExecutablePath();
    CHECK(!exe.empty());
    CHECK(exe.front() == '/');
    std::error_code ec;
    CHECK(fs::exists(exe, ec) && !ec);
  }

  run("enable creates a valid managed desktop entry");
  {
    const auto dir = makeTempDir();
    AppAutostartManager manager(dir);
    CHECK(!manager.isEnabled());
    const auto result = manager.enable();
    CHECK(result.ok);
    CHECK(manager.isEnabled());
    CHECK(manager.desktopFileExists());
    CHECK(manager.entryLooksManaged());
    CHECK(fs::exists(manager.desktopFilePath()));

    const std::string content = readFile(manager.desktopFilePath());
    CHECK(content.find("[Desktop Entry]") != std::string::npos);
    CHECK(content.find("Type=Application") != std::string::npos);
    CHECK(content.find("Name=Arch Task Manager") != std::string::npos);
    CHECK(content.find("X-GNOME-Autostart-enabled=true") != std::string::npos);
    CHECK(content.find(AppAutostartManager::kOwnershipMarker) !=
          std::string::npos);
    const std::string exec = execLineValue(content);
    CHECK(!exec.empty());
    CHECK(exec.front() == '/');
    std::error_code ec;
    CHECK(fs::exists(exec, ec) && !ec);  // points at the running binary
  }

  run("enable is idempotent and leaves no temporary files");
  {
    const auto dir = makeTempDir();
    AppAutostartManager manager(dir);
    CHECK(manager.enable().ok);
    const std::string first = readFile(manager.desktopFilePath());
    CHECK(manager.enable().ok);  // second call is a no-op fast path
    CHECK(readFile(manager.desktopFilePath()) == first);
    std::size_t files = 0;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
      (void)entry;
      ++files;
    }
    CHECK(!ec);
    CHECK(files == 1);  // only arch-task-manager.desktop, no .tmp leftover
  }

  run("disable removes only the own entry and preserves other files");
  {
    const auto dir = makeTempDir();
    const fs::path other = dir / "other-app.desktop";
    const std::string other_content = "[Desktop Entry]\nType=Application\n"
                                      "Name=Other\nExec=/usr/bin/other\n";
    std::ofstream(other) << other_content;

    AppAutostartManager manager(dir);
    CHECK(manager.enable().ok);
    CHECK(manager.disable().ok);
    CHECK(!manager.desktopFileExists());
    CHECK(!manager.isEnabled());
    CHECK(fs::exists(other));                      // other entry untouched
    CHECK(readFile(other) == other_content);       // content untouched
    CHECK(fs::exists(dir));                        // directory preserved

    CHECK(manager.disable().ok);                   // second disable is a no-op
  }

  run("enable/disable leave a user (unmanaged) file untouched");
  {
    const auto dir = makeTempDir();
    const fs::path file = dir / AppAutostartManager::kDesktopFileName;
    const std::string user_content = "[Desktop Entry]\nType=Application\n"
                                     "Name=Hand Written\nExec=/usr/bin/foo\n";
    std::ofstream(file) << user_content;

    AppAutostartManager manager(dir);
    CHECK(manager.desktopFileExists());
    CHECK(!manager.entryLooksManaged());
    CHECK(!manager.isEnabled());

    const auto enable_result = manager.enable();
    CHECK(!enable_result.ok);                      // refused to overwrite
    CHECK(!enable_result.message.empty());
    CHECK(readFile(file) == user_content);         // preserved
    CHECK(!manager.isEnabled());

    const auto disable_result = manager.disable();
    CHECK(!disable_result.ok);                     // refused to delete
    CHECK(fs::exists(file));
    CHECK(readFile(file) == user_content);
  }

  run("a managed entry may be regenerated and removed");
  {
    const auto dir = makeTempDir();
    AppAutostartManager manager(dir);
    std::ofstream(manager.desktopFilePath())
        << "[Desktop Entry]\nType=Application\nName=Old\n"
        << AppAutostartManager::kOwnershipMarker << "\nX-GNOME-Autostart-enabled=true\n";
    CHECK(manager.entryLooksManaged());
    const auto result = manager.enable();          // refresh allowed
    CHECK(result.ok);
    const std::string content = readFile(manager.desktopFilePath());
    CHECK(content.find("Name=Arch Task Manager") != std::string::npos);
    CHECK(manager.disable().ok);
    CHECK(!manager.desktopFileExists());
  }

  run("synchronizeWithSettings repairs a missing entry when enabled");
  {
    const auto dir = makeTempDir();
    AppAutostartManager manager(dir);
    CHECK(manager.synchronizeWithSettings(true) == atm::AutostartSyncState::Repaired);
    CHECK(manager.isEnabled());
    CHECK(manager.synchronizeWithSettings(true) == atm::AutostartSyncState::InSync);
  }

  run("synchronizeWithSettings removes a managed entry when disabled");
  {
    const auto dir = makeTempDir();
    AppAutostartManager manager(dir);
    CHECK(manager.enable().ok);
    CHECK(manager.synchronizeWithSettings(false) == atm::AutostartSyncState::Repaired);
    CHECK(!manager.desktopFileExists());
    CHECK(manager.synchronizeWithSettings(false) == atm::AutostartSyncState::InSync);
  }

  run("synchronizeWithSettings leaves unmanaged entries alone");
  {
    const auto dir = makeTempDir();
    const fs::path file = dir / AppAutostartManager::kDesktopFileName;
    const std::string user_content = "[Desktop Entry]\nType=Application\n"
                                     "Name=Mine\nExec=/usr/bin/foo\n";
    std::ofstream(file) << user_content;

    AppAutostartManager manager(dir);
    // desired disabled, unmanaged file present: do not delete it.
    CHECK(manager.synchronizeWithSettings(false) ==
          atm::AutostartSyncState::LeftAlone);
    CHECK(readFile(file) == user_content);
    // desired enabled, unmanaged file present: do not overwrite it.
    CHECK(manager.synchronizeWithSettings(true) ==
          atm::AutostartSyncState::LeftAlone);
    CHECK(readFile(file) == user_content);
  }

  run("enable fails gracefully on an unwritable directory");
  {
    if (::geteuid() == 0) {
      std::fprintf(stderr, "  (skipped: running as root)\n");
      ++g_checks;
    } else {
      const auto dir = makeTempDir();
      ::chmod(dir.c_str(), 0500);  // read-only: no file creation possible
      AppAutostartManager manager(dir);
      const auto result = manager.enable();
      CHECK(!result.ok);
      CHECK(!result.message.empty());
      CHECK(!manager.isEnabled());  // application keeps running
      ::chmod(dir.c_str(), 0700);   // allow cleanup
    }
  }

  run("enable fails gracefully when the autostart path is a plain file");
  {
    const auto dir = makeTempDir();
    const fs::path path = dir / "autostart";
    std::ofstream(path) << "not a directory";
    AppAutostartManager manager(path);
    const auto result = manager.enable();
    CHECK(!result.ok);
    CHECK(!manager.isEnabled());
  }

  run("disable with unknown location treats itself as already disabled");
  {
    AppAutostartManager manager{fs::path{}};
    const auto result = manager.disable();
    CHECK(result.ok);
    CHECK(!manager.isEnabled());
  }

  std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
  if (g_failures != 0) {
    return EXIT_FAILURE;
  }
  std::fprintf(stderr, "ALL TESTS PASSED\n");
  return EXIT_SUCCESS;
}