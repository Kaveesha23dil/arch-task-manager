#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace atm {

/// Where a startup application entry comes from.
enum class StartupScope {
  User,    // ~/.config/autostart (or $XDG_CONFIG_HOME/autostart)
  System,  // /etc/xdg/autostart (or another XDG config dir entry)
};

/// Human-readable name of a StartupScope.
[[nodiscard]] const char *startupScopeName(StartupScope scope);

/// Sorting order for the startup application list.
enum class StartupSort {
  Name,
  Enabled,
  Scope,
};

/// Human-readable name of a StartupSort variant.
[[nodiscard]] const char *startupSortName(StartupSort sort);

/// Outcome of an enable/disable startup operation.
enum class StartupOperationStatus {
  Success,
  PermissionDenied,
  NotFound,
  IoError,
};

/// Result of a startup application management action.
struct StartupOperationResult {
  StartupOperationStatus status = StartupOperationStatus::Success;
  std::string message;

  [[nodiscard]] bool success() const {
    return status == StartupOperationStatus::Success;
  }
};

/// One desktop autostart entry discovered via the standard XDG layout.
struct StartupApplication {
  std::string id;           // desktop file name, e.g. "discord.desktop"
  std::string name;
  std::string description;
  std::string exec_command;
  std::string icon;
  std::string desktop_file;  // full path of the file defining this entry
  std::string only_show_in;   // OnlyShowIn= value (raw, ';' separated)
  std::string not_show_in;    // NotShowIn= value (raw, ';' separated)

  StartupScope scope = StartupScope::User;
  bool enabled = true;        // effective enabled state (see XDG semantics)
  bool hidden = false;        // entry carries Hidden=true in its file
  bool overrides_system = false;  // a user entry that shadows a system entry
};

/// A snapshot of all discovered startup applications.
struct StartupSnapshot {
  std::vector<StartupApplication> apps;
  bool available = false;  // at least one autostart directory was scanned
};

/**
 * Application startup manager for the standard XDG autostart mechanism.
 *
 * Discovers *.desktop entries from the user's autostart directory
 * (~/.config/autostart, or $XDG_CONFIG_HOME/autostart) and the system
 * directories (/etc/xdg/autostart plus $XDG_CONFIG_DIRS entries). This is
 * configuration management only: it never executes any command. Enabling and
 * disabling a system entry writes a user-level override into the user's
 * autostart directory and never modifies /etc/xdg/autostart directly.
 *
 * Entries are scanned once and cached. `read()` returns a cheap copy of the
 * cached list (called every tick by the main loop); `refresh()` rescans the
 * directories on demand, so the filesystem is not polled continuously. A
 * user entry with the same file name as a system entry hides it, matching the
 * standard XDG precedence.
 */
class StartupManager {
 public:
  StartupManager();
  ~StartupManager() = default;

  // StartupManager owns a cached directory scan; copying/moving would
  // duplicate state whose meaning depends on the on-disk configuration.
  StartupManager(const StartupManager &) = delete;
  StartupManager &operator=(const StartupManager &) = delete;

  /// Rescans the user and system autostart directories and rebuilds the
  /// cached entry list. Missing/invalid entries are skipped, never fatal.
  void refresh();

  /// Returns a copy of the cached startup application snapshot.
  [[nodiscard]] StartupSnapshot read() const;

  /// Enables a startup application by its desktop file name. For a system
  /// entry this creates an enabling user-level override instead of touching
  /// the system file.
  [[nodiscard]] StartupOperationResult enableApp(const std::string &id);

  /// Disables a startup application by its desktop file name. For a system
  /// entry this creates a Hidden=true user-level override, leaving the
  /// system file untouched.
  [[nodiscard]] StartupOperationResult disableApp(const std::string &id);

 private:
  std::vector<StartupApplication> apps_;
  bool available_ = false;

  /// Looks up a cached entry by desktop file id ("" when missing).
  [[nodiscard]] const StartupApplication *findById(
      const std::string &id) const;

  /// Enables or disables an entry while only ever writing to the user's
  /// autostart directory.
  [[nodiscard]] StartupOperationResult setEnabled(const std::string &id,
                                                  bool enable);
};

}  // namespace atm