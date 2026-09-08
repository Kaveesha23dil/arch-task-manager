#pragma once

#include <filesystem>
#include <string>

namespace atm {

/// Outcome of an autostart operation that touched (or tried to touch) the
/// filesystem. `ok` is false whenever the request could not be applied; the UI
/// uses `message` to tell the user what went wrong (never a raw stack trace).
struct AutostartResult {
  bool ok = false;
  std::string message;  // empty on success, else a displayable error
};

/// Result codes for reconciling the persisted setting with the on-disk entry.
enum class AutostartSyncState {
  InSync,                 // on-disk state already matches the requested state
  Repaired,               // entry was created or removed to match
  LeftAlone,              // an unmanaged entry exists; deliberately untouched
  Failed,                 // an operation failed; see the returned message
};

/**
 * Manages *this application's own* XDG desktop autostart entry.
 *
 * This is deliberately a small, single-purpose component. It owns exactly one
 * desktop entry — `arch-task-manager.desktop` — inside the current user's
 * autostart directory, and it never touches system-wide autostart
 * (/etc/xdg/autostart), never runs shell commands, never requires privileges,
 * and never modifies any other application's desktop entry.
 *
 * The entry follows the XDG Autostart specification:
 *
 *   $XDG_CONFIG_HOME/autostart/arch-task-manager.desktop
 *   or (when $XDG_CONFIG_HOME is unset) $HOME/.config/autostart/arch-task-manager.desktop
 *
 * Ownership is established with the `X-ArchTaskManager=true` marker written by
 * this component. An existing entry that does not carry the marker is treated
 * as potentially user-crafted and is never overwritten or deleted silently.
 *
 * Writes are atomic (temporary file + flush + close + rename) so a crash can
 * never leave a half-written .desktop file. The component integrates with the
 * SettingsManager from the settings subsystem: the persisted `autostart_enabled`
 * preference holds the user's intent and synchronizeWithSettings() reconciles
 * it with the file on disk at application startup.
 */
class AppAutostartManager {
 public:
  /// The desktop-file basename this application owns (stable identifier).
  static constexpr const char *kDesktopFileName = "arch-task-manager.desktop";

  /// Marker written into generated entries so this application can recognize
  /// its own files later (ownership, not a generic desktop-entry key).
  static constexpr const char *kOwnershipMarker = "X-ArchTaskManager=true";

  /// Uses the XDG autostart directory by default; a directory may be injected
  /// (tests). An empty dir means the location could not be determined.
  explicit AppAutostartManager(std::filesystem::path autostart_dir =
                                   defaultAutostartDirectory());

  /// $XDG_CONFIG_HOME/autostart, or $HOME/.config/autostart when
  /// $XDG_CONFIG_HOME is unset. Empty when neither can be determined. Only an
  /// absolute $XDG_CONFIG_HOME is honoured (relative values are ignored, as in
  /// the standard XDG autostart handling of this project).
  static std::filesystem::path defaultAutostartDirectory();

  /// The current user's autostart directory (may be empty).
  [[nodiscard]] const std::filesystem::path &autostartDirectory() const;

  /// The desktop-file path inside the autostart directory.
  [[nodiscard]] std::filesystem::path desktopFilePath() const;

  /// True when an autostart location can be determined at all.
  [[nodiscard]] bool isConfigAvailable() const;

  /// True when the application currently has a desktop-session autostart entry
  /// (the file exists AND is recognized as application-managed).
  [[nodiscard]] bool isEnabled() const;

  /// True when the own desktop file exists on disk (managed or not).
  [[nodiscard]] bool desktopFileExists() const;

  /// True when the existing own-name file carries the application ownership
  /// marker and thus may be regenerated/removed by this application.
  [[nodiscard]] bool entryLooksManaged() const;

  /// Creates (or refreshes) the application's autostart entry. Idempotent:
  /// when the entry already exists with the exact content we would write,
  /// nothing is rewritten. An existing entry WITHOUT the ownership marker is
  /// never overwritten — enable() then fails with a clear message instead of
  /// destroying user modifications.
  [[nodiscard]] AutostartResult enable();

  /// Removes only the application's own autostart entry. The autostart
  /// directory itself and every other .desktop file are left untouched. A
  /// missing entry/directory is treated as "already disabled" (success). An
  /// existing entry without the ownership marker is never deleted.
  [[nodiscard]] AutostartResult disable();

  /// Reconciles the on-disk entry with the requested `desired_enabled` state
  /// (the value persisted by the SettingsManager). Used once at application
  /// startup:
  ///   - desired true,  entry present & managed   -> in sync
  ///   - desired true,  entry missing             -> repair (recreate)
  ///   - desired true,  entry present & unmanaged -> left alone + warning
  ///   - desired false, entry present & managed   -> remove our own entry
  ///   - desired false, entry present & unmanaged -> left alone + warning
  ///   - desired false, entry missing             -> in sync
  /// Never throws and never prevents the application from starting.
  [[nodiscard]] AutostartSyncState synchronizeWithSettings(bool desired_enabled);

  /// The absolute path of the running executable, resolved via the Linux
  /// /proc/self/exe symlink (readlink(2)). Empty on failure.
  [[nodiscard]] static std::string currentExecutablePath();

 private:
  std::filesystem::path dir_;
};

}  // namespace atm