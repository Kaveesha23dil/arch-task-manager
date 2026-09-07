#include "startup_manager.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace atm {

namespace {

namespace fs = std::filesystem;

/// Strips leading/trailing whitespace (including a CR) from a line.
std::string trimWhitespace(const std::string &text) {
  std::size_t begin = 0;
  while (begin < text.size() &&
         (text[begin] == ' ' || text[begin] == '\t' ||
          text[begin] == '\r')) {
    ++begin;
  }
  std::size_t end = text.size();
  while (end > begin &&
         (text[end - 1] == ' ' || text[end - 1] == '\t' ||
          text[end - 1] == '\r')) {
    --end;
  }
  return text.substr(begin, end - begin);
}

/// Lowercases ASCII characters (for case-insensitive matching).
std::string toLowerAscii(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  return text;
}

/// Parses an XDG boolean ("true"/"false", "1"/"0"). Returns false when the
/// value is missing or unparseable, which matches the XDG convention that an
/// absent boolean field means "false".
bool parseXdBoolean(const std::string &value) {
  if (value == "true" || value == "1") return true;
  if (value == "false" || value == "0") return false;
  return false;
}

/// One parsed key/value pair book of a .desktop file.
struct DesktopEntry {
  std::unordered_map<std::string, std::string> keys;
  bool has_group = false;
};

/// Reads a .desktop file and parses the [Desktop Entry] group's keys.
/// Localized keys (e.g. Name[de]) are folded into their base key: a plain
/// key always wins; otherwise the first localized value is used.
DesktopEntry parseDesktopEntryFile(const fs::path &path) {
  DesktopEntry entry;

  std::ifstream in(path);
  if (!in) {
    return entry;
  }

  bool in_desktop_group = false;
  std::string line;
  while (std::getline(in, line)) {
    const std::string trimmed = trimWhitespace(line);
    if (trimmed.empty() || trimmed[0] == '#') {
      continue;
    }
    if (trimmed[0] == '[') {
      in_desktop_group = (trimmed == "[Desktop Entry]");
      if (in_desktop_group) {
        entry.has_group = true;
      }
      continue;
    }
    if (!in_desktop_group) {
      continue;
    }

    const std::size_t eq = trimmed.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    const std::string raw_key = trimWhitespace(trimmed.substr(0, eq));
    const std::string value = trimWhitespace(trimmed.substr(eq + 1));
    if (raw_key.empty()) {
      continue;
    }

    // Fold localized keys into the base key: "Name[de]" -> base "Name".
    const std::size_t bracket = raw_key.find('[');
    const bool localized = bracket != std::string::npos;
    const std::string base =
        localized ? raw_key.substr(0, bracket) : raw_key;
    if (base.empty()) {
      continue;
    }

    // Plain keys override localized ones; the first localized value wins.
    const auto it = entry.keys.find(base);
    if (it == entry.keys.end() || !localized) {
      entry.keys[base] = value;
    }
  }
  return entry;
}

/// True for files that should be ignored as backup/temporary leftovers.
bool ignoredDesktopFileName(const std::string &name) {
  if (name.empty()) {
    return true;
  }
  if (name[0] == '.' || name[0] == '#') {
    return true;
  }
  return name.find('~') != std::string::npos;
}

/// Returns the current user's autostart directory
/// ($XDG_CONFIG_HOME/autostart, or $HOME/.config/autostart). Empty when the
/// home directory cannot be determined.
fs::path userAutostartDir() {
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

/// System autostart directories: /etc/xdg/autostart plus one directory per
/// XDG_CONFIG_DIRS entry (defaults to /etc/xdg when unset).
std::vector<fs::path> systemAutostartDirs() {
  std::vector<fs::path> dirs;
  dirs.emplace_back("/etc/xdg/autostart");

  const char *config_dirs = std::getenv("XDG_CONFIG_DIRS");
  const std::string dirs_value =
      config_dirs != nullptr ? config_dirs : "/etc/xdg";
  std::size_t start = 0;
  while (true) {
    const std::size_t colon = dirs_value.find(':', start);
    const std::string dir =
        dirs_value.substr(start, colon == std::string::npos
                                     ? std::string::npos
                                     : colon - start);
    if (!dir.empty()) {
      const fs::path p(dir);
      if (p.is_absolute() && p != fs::path("/etc/xdg")) {
        dirs.emplace_back(p / "autostart");
      }
    }
    if (colon == std::string::npos) {
      break;
    }
    start = colon + 1;
  }
  return dirs;
}

/// Value of $XDG_CURRENT_DESKTOP (colon-separated list of DE identifiers),
/// empty when unset.
std::string currentDesktop() {
  const char *de = std::getenv("XDG_CURRENT_DESKTOP");
  return de != nullptr ? de : "";
}

/// True when `list` (a ';' or ':' separated string) contains `desktop` as one
/// of its entries. Matching is case-insensitive per the XDG spec.
bool listContainsEntry(const std::string &list, const std::string &desktop) {
  if (desktop.empty()) {
    return false;
  }
  const std::string haystack = toLowerAscii(list);
  const std::string needle = toLowerAscii(desktop);
  std::size_t start = 0;
  while (true) {
    const std::size_t sep = haystack.find_first_of(";:", start);
    const std::string entry =
        haystack.substr(start, sep == std::string::npos
                                   ? std::string::npos
                                   : sep - start);
    if (entry == needle) {
      return true;
    }
    if (sep == std::string::npos) {
      break;
    }
    start = sep + 1;
  }
  return false;
}

/// Builds a StartupApplication from a parsed desktop entry.
StartupApplication makeApplication(const fs::path &path,
                                   const DesktopEntry &entry) {
  const auto value = [&](const char *key) -> std::string {
    const auto it = entry.keys.find(key);
    return it == entry.keys.end() ? std::string{} : it->second;
  };

  StartupApplication app;
  app.id = path.filename().string();
  app.name = value("Name");
  app.description = value("Comment");
  app.exec_command = value("Exec");
  app.icon = value("Icon");
  app.only_show_in = value("OnlyShowIn");
  app.not_show_in = value("NotShowIn");
  app.desktop_file = path.string();
  app.hidden = parseXdBoolean(value("Hidden"));

  return app;
}

/// Recomputes the effective enabled state of an application using the XDG
/// autostart rules:
///   * a missing or unparseable boolean field does not mean "disabled";
///   * Hidden=true disables the entry (its usual purpose is to hide a
///     system-wide entry from the user's directory);
///   * X-GNOME-Autostart-enabled=false disables the entry (recognized without
///     requiring GNOME itself);
///   * OnlyShowIn / NotShowIn are applied against $XDG_CURRENT_DESKTOP when a
///     desktop environment is running; otherwise they do not disable.
void applyEnabledState(StartupApplication &app, const DesktopEntry &entry) {
  bool enabled = true;

  const auto value = [&](const char *key) -> std::string {
    const auto it = entry.keys.find(key);
    return it == entry.keys.end() ? std::string{} : it->second;
  };

  if (app.hidden) {
    enabled = false;
  }
  const std::string gnome_enabled = value("X-GNOME-Autostart-enabled");
  if (!gnome_enabled.empty() && !parseXdBoolean(gnome_enabled)) {
    enabled = false;
  }

  const std::string de = currentDesktop();
  if (!de.empty()) {
    if (!app.only_show_in.empty() && !listContainsEntry(app.only_show_in, de)) {
      enabled = false;
    }
    if (!app.not_show_in.empty() && listContainsEntry(app.not_show_in, de)) {
      enabled = false;
    }
  }

  app.enabled = enabled;
}

/// Scans one autostart directory for *.desktop files. Per-file exceptions are
/// swallowed so one broken entry never stops the scan.
void scanDirectory(const fs::path &dir,
                   std::vector<StartupApplication> &out) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    return;
  }

  fs::directory_iterator iter(dir, ec);
  const fs::directory_iterator end;
  for (; iter != end; iter.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    }
    try {
      const fs::directory_entry &entry = *iter;
      if (!entry.is_regular_file()) {
        continue;
      }
      const std::string name = entry.path().filename().string();
      if (name.size() < 8 ||
          name.compare(name.size() - 8, 8, ".desktop") != 0 ||
          ignoredDesktopFileName(name)) {
        continue;
      }

      const DesktopEntry parsed = parseDesktopEntryFile(entry.path());
      if (!parsed.has_group) {
        continue;  // not a desktop entry; ignored
      }

      const auto type_it = parsed.keys.find("Type");
      if (type_it != parsed.keys.end() && type_it->second != "Application") {
        continue;  // autostart only applies to Type=Application entries
      }

      StartupApplication app = makeApplication(entry.path(), parsed);
      if (app.name.empty()) {
        app.name = name;  // fall back to the file name when Name is missing
      }
      applyEnabledState(app, parsed);
      out.push_back(std::move(app));
    } catch (const std::exception &) {
      // Broken symlink, vanished file, unreadable entry: skip it.
      continue;
    }
  }
}

/// Maps a filesystem error code onto an operation status.
StartupOperationStatus classifyFileError(const std::error_code &ec) {
  if (ec == std::errc::permission_denied) {
    return StartupOperationStatus::PermissionDenied;
  }
  return StartupOperationStatus::IoError;
}

/// Sets the given keys inside the [Desktop Entry] group of a .desktop file,
/// preserving every other line (comments, unrelated keys, other groups).
/// Keys that already exist are updated in place; missing keys are inserted
/// right after the group header. Returns false with `error` filled on failure.
bool updateDesktopKeys(
    const fs::path &path,
    const std::vector<std::pair<std::string, std::string>> &updates,
    std::string &error) {
  std::ifstream in(path);
  if (!in) {
    error = "Could not open file for reading.";
    return false;
  }

  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) {
    lines.push_back(line);
  }
  in.close();

  std::size_t group_index = lines.size();  // index of "[Desktop Entry]"
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (trimWhitespace(lines[i]) == "[Desktop Entry]") {
      group_index = i;
      break;
    }
  }
  if (group_index == lines.size()) {
    // No [Desktop Entry] group: create one so the entry stays valid.
    lines.insert(lines.begin(), "[Desktop Entry]");
    group_index = 0;
  }

  for (const auto &[key, value] : updates) {
    bool replaced = false;
    for (std::size_t i = group_index + 1; i < lines.size(); ++i) {
      const std::string trimmed = trimWhitespace(lines[i]);
      if (!trimmed.empty() && trimmed[0] == '[') {
        break;  // left the [Desktop Entry] group
      }
      if (trimmed.empty() || trimmed[0] == '#') {
        continue;
      }
      const std::size_t eq = trimmed.find('=');
      if (eq == std::string::npos) {
        continue;
      }
      if (trimWhitespace(trimmed.substr(0, eq)) == key) {
        lines[i] = key + "=" + value;  // normalize to Key=Value
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      lines.insert(
          lines.begin() + static_cast<std::ptrdiff_t>(group_index + 1),
          key + "=" + value);
    }
  }

  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    error = "Could not open file for writing.";
    return false;
  }
  for (const std::string &line : lines) {
    out << line << '\n';
  }
  if (!out.good()) {
    error = "Failed while writing the file.";
    return false;
  }
  out.close();
  if (!out.good()) {
    error = "Failed while writing the file.";
    return false;
  }
  return true;
}

}  // namespace

const char *startupScopeName(StartupScope scope) {
  switch (scope) {
    case StartupScope::User:
      return "User";
    case StartupScope::System:
      return "System";
  }
  return "User";
}

const char *startupSortName(StartupSort sort) {
  switch (sort) {
    case StartupSort::Name:
      return "Name";
    case StartupSort::Enabled:
      return "Enabled";
    case StartupSort::Scope:
      return "Scope";
  }
  return "Name";
}

StartupManager::StartupManager() { refresh(); }

void StartupManager::refresh() {
  apps_.clear();
  available_ = false;

  const fs::path user_dir = userAutostartDir();
  const std::vector<fs::path> system_dirs = systemAutostartDirs();

  // id -> index into apps_, used both to detect system entries that a user
  // entry overrides and to replace entries in place.
  std::unordered_map<std::string, std::size_t> index;

  // System entries first (multiple XDG config dirs override each other in
  // order; later dirs take precedence by replacing earlier ones).
  for (const fs::path &dir : system_dirs) {
    std::vector<StartupApplication> found;
    scanDirectory(dir, found);
    for (StartupApplication &app : found) {
      app.scope = StartupScope::System;
      app.overrides_system = false;
      const auto [it, inserted] = index.emplace(app.id, apps_.size());
      if (!inserted) {
        apps_[it->second] = std::move(app);
      } else {
        apps_.push_back(std::move(app));
      }
    }
  }

  // User entries: a user entry with the same id shadows the system entry.
  if (!user_dir.empty()) {
    std::vector<StartupApplication> user_found;
    scanDirectory(user_dir, user_found);
    for (StartupApplication &app : user_found) {
      app.scope = StartupScope::User;
      app.overrides_system = index.count(app.id) != 0;
      const auto [it, inserted] = index.emplace(app.id, apps_.size());
      if (!inserted) {
        apps_[it->second] = std::move(app);
      } else {
        apps_.push_back(std::move(app));
      }
    }
  }

  std::error_code ec;
  bool any_system_dir = false;
  for (const fs::path &dir : system_dirs) {
    any_system_dir = any_system_dir || fs::is_directory(dir, ec);
    ec.clear();
  }
  available_ = !user_dir.empty() || any_system_dir;
}

StartupSnapshot StartupManager::read() const {
  StartupSnapshot snapshot;
  snapshot.apps = apps_;
  snapshot.available = available_;
  return snapshot;
}

const StartupApplication *StartupManager::findById(
    const std::string &id) const {
  for (const StartupApplication &app : apps_) {
    if (app.id == id) {
      return &app;
    }
  }
  return nullptr;
}

StartupOperationResult StartupManager::enableApp(const std::string &id) {
  return setEnabled(id, true);
}

StartupOperationResult StartupManager::disableApp(const std::string &id) {
  return setEnabled(id, false);
}

StartupOperationResult StartupManager::setEnabled(const std::string &id,
                                                  bool enable) {
  const StartupApplication *app = findById(id);
  if (!app) {
    return StartupOperationResult{
        StartupOperationStatus::NotFound, "Startup entry not found: " + id};
  }

  const fs::path user_dir = userAutostartDir();
  if (user_dir.empty()) {
    return StartupOperationResult{
        StartupOperationStatus::IoError,
        "Cannot determine the user autostart directory."};
  }

  // Only the Hidden= and X-GNOME-Autostart-enabled= keys are managed; every
  // other field of the entry is preserved verbatim.
  std::vector<std::pair<std::string, std::string>> updates;
  if (enable) {
    updates.emplace_back("Hidden", "false");
    updates.emplace_back("X-GNOME-Autostart-enabled", "true");
  } else {
    updates.emplace_back("Hidden", "true");
    updates.emplace_back("X-GNOME-Autostart-enabled", "false");
  }

  std::error_code ec;
  if (!fs::exists(user_dir, ec)) {
    fs::create_directories(user_dir, ec);
    if (ec) {
      return StartupOperationResult{
          classifyFileError(ec),
          "Could not create " + user_dir.string() + "."};
    }
  }

  // Only ever write below the user's autostart directory. For a system entry
  // this creates a user-level override instead of touching /etc/xdg.
  const fs::path dest = user_dir / id;
  if (!fs::exists(dest, ec)) {
    fs::copy_file(app->desktop_file, dest,
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
      return StartupOperationResult{
          classifyFileError(ec),
          "Could not copy " + app->desktop_file + "."};
    }
  }

  std::string write_error;
  if (!updateDesktopKeys(dest, updates, write_error)) {
    return StartupOperationResult{StartupOperationStatus::IoError,
                                  write_error};
  }

  refresh();  // rebuild the cached list so the change appears immediately
  return StartupOperationResult{};
}

}  // namespace atm