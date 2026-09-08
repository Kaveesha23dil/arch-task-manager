#include "app_autostart_manager.hpp"

#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

#include "logger.hpp"

namespace atm {

namespace {
namespace fs = std::filesystem;

constexpr const char *kEntryHeader = "[Desktop Entry]";
constexpr const char *kEntryTypeApplication = "Type=Application";
constexpr const char *kEntryName = "Name=Arch Task Manager";
constexpr const char *kEntryComment =
    "Comment=System monitoring and task management";
constexpr const char *kEntryTerminalFalse = "Terminal=false";
constexpr const char *kEntryStartupNotifyFalse = "StartupNotify=false";
constexpr const char *kEntryGnomeAutostart = "X-GNOME-Autostart-enabled=true";

/// Escapes a single executable argument for the Exec= value according to the
/// Desktop Entry Specification: double quotes wrap arguments that contain
/// whitespace, and '\', '"', '`' and '$' are backslash-escaped.
std::string escapeExecArgument(const std::string &argument) {
  std::string escaped;
  bool needs_quotes = false;
  for (const char ch : argument) {
    switch (ch) {
      case '\\':
        escaped += "\\\\";
        needs_quotes = true;
        break;
      case '"':
        escaped += "\\\"";
        needs_quotes = true;
        break;
      case '`':
        escaped += "\\`";
        needs_quotes = true;
        break;
      case '$':
        escaped += "\\$";
        needs_quotes = true;
        break;
      case ' ':
        escaped += ch;
        needs_quotes = true;
        break;
      default:
        escaped += ch;
        break;
    }
  }
  return needs_quotes ? "\"" + escaped + "\"" : escaped;
}

/// Serializes the full desktop-entry body for the application. Deterministic
/// ordering so the same binary always produces byte-identical content.
std::string buildDesktopEntry(const std::string &exec_path) {
  std::ostringstream out;
  out << kEntryHeader << '\n'
      << kEntryTypeApplication << '\n'
      << kEntryName << '\n'
      << kEntryComment << '\n'
      << "Exec=" << escapeExecArgument(exec_path) << '\n'
      << kEntryTerminalFalse << '\n'
      << kEntryStartupNotifyFalse << '\n'
      << kEntryGnomeAutostart << '\n'
      << AppAutostartManager::kOwnershipMarker << '\n';
  return out.str();
}

/// Reads a whole small text file. Returns false on any failure (the output
/// string is left as-is). Used to inspect existing entries before overwriting.
bool readFileToString(const fs::path &path, std::string &out) {
  std::ifstream in(path);
  if (!in) {
    return false;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  if (!in) {
    return false;
  }
  out = buffer.str();
  return true;
}

AutostartResult errorResult(const std::string &message) {
  AutostartResult result;
  result.ok = false;
  result.message = message;
  return result;
}

AutostartResult okResult() {
  AutostartResult result;
  result.ok = true;
  return result;
}

}  // namespace

AppAutostartManager::AppAutostartManager(fs::path autostart_dir)
    : dir_(std::move(autostart_dir)) {}

fs::path AppAutostartManager::defaultAutostartDirectory() {
  if (const char *xdg = std::getenv("XDG_CONFIG_HOME");
      xdg != nullptr && *xdg != '\0' && fs::path(xdg).is_absolute()) {
    return fs::path(xdg) / "autostart";
  }
  if (const char *home = std::getenv("HOME");
      home != nullptr && *home != '\0') {
    return fs::path(home) / ".config" / "autostart";
  }
  return {};
}

const fs::path &AppAutostartManager::autostartDirectory() const { return dir_; }

fs::path AppAutostartManager::desktopFilePath() const {
  if (dir_.empty()) {
    return {};
  }
  return dir_ / kDesktopFileName;
}

bool AppAutostartManager::isConfigAvailable() const { return !dir_.empty(); }

bool AppAutostartManager::desktopFileExists() const {
  if (dir_.empty()) {
    return false;
  }
  std::error_code ec;
  return std::filesystem::exists(desktopFilePath(), ec) && !ec;
}

bool AppAutostartManager::entryLooksManaged() const {
  if (!desktopFileExists()) {
    return false;
  }
  std::string content;
  if (!readFileToString(desktopFilePath(), content)) {
    return false;
  }
  return content.find(kOwnershipMarker) != std::string::npos;
}

bool AppAutostartManager::isEnabled() const {
  return desktopFileExists() && entryLooksManaged();
}

std::string AppAutostartManager::currentExecutablePath() {
  std::array<char, 4096> buffer{};
  const ssize_t length =
      ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (length <= 0) {
    return {};
  }
  buffer[static_cast<std::size_t>(length)] = '\0';
  std::string path(buffer.data());
  if (path.empty() || path.front() != '/') {
    return {};  // must be an absolute path to be usable as Exec=
  }
  return path;
}

AutostartResult AppAutostartManager::enable() {
  Logger::info("Autostart requested: enabled");

  if (dir_.empty()) {
    return errorResult(
        "Cannot determine the autostart directory (neither $XDG_CONFIG_HOME "
        "nor $HOME is set).");
  }
  const fs::path file = desktopFilePath();
  Logger::info("Autostart directory: " + dir_.string());

  const std::string exec_path = currentExecutablePath();
  if (exec_path.empty()) {
    return errorResult(
        "Cannot determine the application executable path (/proc/self/exe was "
        "not readable).");
  }

  // Never overwrite an existing entry we do not own (it may hold user edits).
  if (desktopFileExists() && !entryLooksManaged()) {
    Logger::warn("Existing autostart entry is not managed by this application; "
                 "it was left untouched: " +
                 file.string());
    return errorResult(
        "A non-managed autostart entry already exists at " + file.string() +
        " and was left untouched. Remove or edit it manually, then retry.");
  }

  // Create the autostart directory on demand (never system-wide autostart).
  std::error_code ec;
  try {
    std::filesystem::create_directories(dir_, ec);
  } catch (const std::filesystem::filesystem_error &) {
    ec = std::make_error_code(std::errc::io_error);
  }
  if (ec) {
    Logger::error("Failed to create autostart directory: " + dir_.string() +
                  " (" + ec.message() + ")");
    return errorResult("Permission denied while creating " + dir_.string() +
                       " (" + ec.message() + ")");
  }

  const std::string content = buildDesktopEntry(exec_path);

  // Idempotent fast path: an identical, managed entry needs no rewrite.
  if (desktopFileExists()) {
    std::string current;
    if (readFileToString(file, current) && current == content) {
      Logger::info("Autostart enabled successfully (entry already present)");
      return okResult();
    }
  }

  Logger::info("Creating desktop entry");
  // Atomic write: temporary file -> write complete content -> flush -> close
  // -> rename. A crash in the middle can never leave a half-written entry.
  const fs::path tmp = fs::path(file).string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::out | std::ios::trunc);
    if (!out) {
      Logger::error("Failed to write desktop entry: " + tmp.string());
      return errorResult("Failed to write desktop entry: " + tmp.string());
    }
    out << content;
    out.flush();
    if (!out) {
      Logger::error("Failed to write desktop entry: " + tmp.string());
      std::filesystem::remove(tmp, ec);
      return errorResult("Failed to write desktop entry: " + tmp.string());
    }
  }

  std::filesystem::rename(tmp, file, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
    Logger::error("Failed to write desktop entry: " + file.string() + " (" +
                  ec.message() + ")");
    return errorResult("Failed to write desktop entry (" + ec.message() + ")");
  }

  // Configuration data, so normal user-readable permissions; never executable.
  std::error_code perm_ec;
  std::filesystem::permissions(
      file,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
          std::filesystem::perms::group_read |
          std::filesystem::perms::others_read,
      std::filesystem::perm_options::replace, perm_ec);
  if (perm_ec) {
    Logger::warn("Could not set permissions on " + file.string());
  }

  Logger::info("Autostart enabled successfully");
  return okResult();
}

AutostartResult AppAutostartManager::disable() {
  Logger::info("Autostart requested: disabled");

  if (dir_.empty() || !desktopFileExists()) {
    Logger::info("Autostart disabled successfully (no entry to remove)");
    return okResult();  // treat as already disabled
  }

  const fs::path file = desktopFilePath();
  if (!entryLooksManaged()) {
    Logger::warn("Existing autostart entry is not managed by this application; "
                 "it was left untouched: " +
                 file.string());
    return errorResult(
        "A non-managed autostart entry exists at " + file.string() +
        " and was left untouched.");
  }

  Logger::info("Removing application autostart entry: " + file.string());
  std::error_code ec;
  std::filesystem::remove(file, ec);
  if (ec) {
    Logger::error("Failed to remove desktop entry: " + file.string() + " (" +
                  ec.message() + ")");
    return errorResult("Failed to remove " + file.string() + " (" +
                       ec.message() + ")");
  }
  Logger::info("Autostart disabled successfully");
  return okResult();
}

AutostartSyncState AppAutostartManager::synchronizeWithSettings(
    bool desired_enabled) {
  if (desired_enabled) {
    if (isEnabled()) {
      Logger::info("Autostart is already enabled");
      return AutostartSyncState::InSync;
    }
    if (desktopFileExists() && !entryLooksManaged()) {
      Logger::warn(
          "Autostart is enabled in settings but an unmanaged desktop entry "
          "exists; it was left untouched: " +
          desktopFilePath().string());
      return AutostartSyncState::LeftAlone;
    }
    return enable().ok ? AutostartSyncState::Repaired
                       : AutostartSyncState::Failed;
  }

  if (!desktopFileExists()) {
    Logger::info("Autostart is disabled and no desktop entry exists");
    return AutostartSyncState::InSync;
  }
  if (!entryLooksManaged()) {
    Logger::warn(
        "An unmanaged desktop entry exists while autostart is disabled in "
        "settings; it was left untouched: " +
        desktopFilePath().string());
    return AutostartSyncState::LeftAlone;
  }
  return disable().ok ? AutostartSyncState::Repaired
                      : AutostartSyncState::Failed;
}

}  // namespace atm