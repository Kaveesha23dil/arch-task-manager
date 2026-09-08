#include "package_manager.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>

#include <alpm.h>
#include <alpm_list.h>

namespace atm {

namespace {

/// Values from /etc/os-release we are interested in.
constexpr const char* kOsReleasePath = "/etc/os-release";
constexpr const char* kPacmanConfPath = "/etc/pacman.conf";

bool isArchIdentifier(const std::string& text) {
  std::string lowered = text;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  return lowered == "arch" || lowered.find("arch") != std::string::npos;
}

/// Parses a KEY="value" (or KEY=value) line from os-release / pacman.conf.
std::optional<std::string> valueForKey(const std::string& line,
                                       const std::string& key) {
  if (line.rfind(key, 0) != 0) {
    return std::nullopt;
  }
  if (line.size() == key.size() || line[key.size()] != '=') {
    return std::nullopt;
  }
  std::string value = line.substr(key.size() + 1);
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
    value = value.substr(1, value.size() - 2);
  }
  return value;
}

/// Detects whether an AUR helper exists somewhere on PATH. This is purely a
/// presence check (used for display); the helper is never executed.
bool executableInPath(const std::string& name) {
  const char* path = std::getenv("PATH");
  if (path == nullptr) {
    return false;
  }
  std::istringstream stream(path);
  std::string dir;
  while (std::getline(stream, dir, ':')) {
    if (dir.empty()) {
      continue;
    }
    std::error_code ec;
    const std::filesystem::path candidate =
        std::filesystem::path(dir) / name;
    if (!std::filesystem::is_regular_file(candidate, ec)) {
      continue;
    }
    std::filesystem::perms p =
        std::filesystem::status(candidate, ec).permissions();
    if ((p & std::filesystem::perms::owner_exec) !=
            std::filesystem::perms::none ||
        (p & std::filesystem::perms::group_exec) !=
            std::filesystem::perms::none ||
        (p & std::filesystem::perms::others_exec) !=
            std::filesystem::perms::none) {
      return true;
    }
  }
  return false;
}

}  // namespace

const char* packageSourceName(PackageSource source) {
  switch (source) {
    case PackageSource::Official: return "Official";
    case PackageSource::AUR:      return "AUR";
    case PackageSource::Foreign:  return "Foreign";
    case PackageSource::Unknown:  return "Unknown";
  }
  return "Unknown";
}

const char* packageInstallReasonName(int reason) {
  switch (reason) {
    case ALPM_PKG_REASON_EXPLICIT: return "Explicitly installed";
    case ALPM_PKG_REASON_DEPEND:   return "Installed as dependency";
    case ALPM_PKG_REASON_UNKNOWN:  return "Unknown";
  }
  return "Unknown";
}

PackageManager::PackageManager() = default;

PackageManager::~PackageManager() {
  closeAlpm();
}

void PackageManager::detectDistribution() {
  summary_.distribution.clear();
  summary_.supported = false;

  std::ifstream file(kOsReleasePath);
  if (!file) {
    summary_.error_message = "Could not read /etc/os-release.";
    return;
  }

  std::string id;
  std::string id_like;
  std::string pretty_name;
  std::string line;
  while (std::getline(file, line)) {
    if (const auto v = valueForKey(line, "ID"); v.has_value()) {
      id = *v;
    } else if (const auto v = valueForKey(line, "ID_LIKE");
               v.has_value()) {
      id_like = *v;
    } else if (const auto v = valueForKey(line, "PRETTY_NAME");
               v.has_value()) {
      pretty_name = *v;
    }
  }

  summary_.distribution =
      pretty_name.empty() ? (id.empty() ? "Unknown" : id) : pretty_name;
  summary_.supported = isArchIdentifier(id) || isArchIdentifier(id_like);
  if (!summary_.supported) {
    summary_.error_message =
        "Package update manager: not supported on this distribution.";
  }
}

bool PackageManager::initialize() {
  detectDistribution();
  if (!summary_.supported) {
    return false;
  }

  // AUR helper detection is presence-only and never executes the helper.
  for (const char* helper : {"paru", "yay"}) {
    if (executableInPath(helper)) {
      summary_.aur_helper = helper;
      break;
    }
  }

  return openAlpm();
}

bool PackageManager::openAlpm() {
  constexpr const char* kRoot = "/";
  constexpr const char* kDbPath = "/var/lib/pacman/";
  alpm_errno_t err = ALPM_ERR_OK;
  alpm_handle_t* handle = alpm_initialize(kRoot, kDbPath, &err);
  if (handle == nullptr) {
    summary_.error_message = "Package database is unavailable: ";
    summary_.error_message += alpm_strerror(err);
    summary_.initialized = false;
    return false;
  }
  handle_ = handle;
  summary_.initialized = true;

  // Register sync databases from the [repo] sections of pacman.conf. This is
  // config parsing (not shell output) and keeps the repository list correct
  // without hard-coding any repo names.
  std::ifstream conf(kPacmanConfPath);
  if (conf) {
    std::string line;
    while (std::getline(conf, line)) {
      std::string section = line;
      section.erase(0, section.find_first_not_of(" \t"));
      if (section.empty() || section.front() != '[') {
        continue;
      }
      const std::size_t close = section.find(']');
      if (close == std::string::npos) {
        continue;
      }
      std::string name = section.substr(1, close - 1);
      if (name.empty() || name == "options") {
        continue;
      }
      (void)alpm_register_syncdb(handle, name.c_str(), ALPM_SIG_USE_DEFAULT);
    }
  }

  if (alpm_get_syncdbs(handle) == nullptr) {
    summary_.error_message =
        "No sync databases are configured. Run 'pacman -Sy' (with user "
        "confirmation) to initialise repository metadata.";
  }
  return true;
}

void PackageManager::closeAlpm() {
  if (handle_ != nullptr) {
    alpm_release(static_cast<alpm_handle_t*>(handle_));
    handle_ = nullptr;
  }
}

bool PackageManager::refresh() {
  if (!summary_.supported) {
    summary_.refresh_failed = true;
    summary_.refresh_in_progress = false;
    if (summary_.error_message.empty()) {
      summary_.error_message =
          "Package update manager: not supported on this distribution.";
    }
    return false;
  }

  // Lazily open libalpm (or retry after an earlier failure, e.g. while pacman
  // held the database or the database was temporarily unavailable).
  if (handle_ == nullptr && !openAlpm()) {
    summary_.refresh_failed = true;
    summary_.refresh_in_progress = false;
    if (summary_.error_message.empty()) {
      summary_.error_message = "Package database is unavailable.";
    }
    return false;
  }

  summary_.refresh_in_progress = true;
  summary_.refresh_failed = false;
  summary_.error_message.clear();
  updates_.clear();

  alpm_handle_t* handle = static_cast<alpm_handle_t*>(handle_);
  alpm_db_t* local = alpm_get_localdb(handle);
  alpm_list_t* sync = alpm_get_syncdbs(handle);

  if (local == nullptr) {
    summary_.refresh_in_progress = false;
    summary_.refresh_failed = true;
    summary_.error_message = "Local package database is unavailable.";
    return false;
  }

  // No sync databases configured (e.g. never ran 'pacman -Sy'): nothing can
  // be classified as official or foreign, so report zero and keep the
  // explanatory message from openAlpm().
  if (sync == nullptr) {
    summary_.official_updates = 0;
    summary_.aur_updates = aur_provider_ != nullptr &&
                                   aur_provider_->available()
                               ? aur_provider_->checkUpdates().size()
                               : 0;
    summary_.foreign_packages = 0;
    summary_.total_updates = summary_.official_updates + summary_.aur_updates;
    summary_.refresh_in_progress = false;
    return true;
  }

  // Build the set of package names present in every sync database so a local
  // package that appears in none of them can be classified as foreign.
  std::vector<std::string> sync_names;
  for (alpm_list_t* it = sync; it != nullptr; it = alpm_list_next(it)) {
    const alpm_db_t* db = static_cast<const alpm_db_t*>(it->data);
    alpm_list_t* cache = alpm_db_get_pkgcache(const_cast<alpm_db_t*>(db));
    for (alpm_list_t* p = cache; p != nullptr; p = alpm_list_next(p)) {
      sync_names.push_back(
          alpm_pkg_get_name(static_cast<alpm_pkg_t*>(p->data)));
    }
  }
  std::sort(sync_names.begin(), sync_names.end());
  sync_names.erase(std::unique(sync_names.begin(), sync_names.end()),
                   sync_names.end());

  std::size_t official = 0;
  std::size_t foreign = 0;

  alpm_list_t* local_cache = alpm_db_get_pkgcache(local);
  for (alpm_list_t* it = local_cache; it != nullptr;
       it = alpm_list_next(it)) {
    alpm_pkg_t* pkg = static_cast<alpm_pkg_t*>(it->data);
    const char* name = alpm_pkg_get_name(pkg);

    if (!std::binary_search(sync_names.begin(), sync_names.end(), name)) {
      ++foreign;
      continue;
    }

    // Look for a newer version already present in the local sync databases.
    // alpm_sync_get_new_version uses libalpm's native version comparison and
    // never touches the network.
    alpm_pkg_t* newer = alpm_sync_get_new_version(pkg, sync);
    if (newer == nullptr) {
      continue;
    }

    PackageUpdate update;
    update.name = name;
    update.installed_version = alpm_pkg_get_version(pkg);
    update.available_version = alpm_pkg_get_version(newer);
    const alpm_db_t* ndb = alpm_pkg_get_db(newer);
    update.repository =
        ndb != nullptr ? alpm_db_get_name(ndb) : "";
    update.source = PackageSource::Official;
    update.is_aur = false;
    update.description = alpm_pkg_get_desc(pkg) != nullptr
                             ? alpm_pkg_get_desc(pkg)
                             : "";
    update.architecture = alpm_pkg_get_arch(pkg) != nullptr
                              ? alpm_pkg_get_arch(pkg)
                              : "";
    update.installed_size = static_cast<std::uint64_t>(alpm_pkg_get_isize(pkg));
    update.install_reason = static_cast<int>(alpm_pkg_get_reason(pkg));
    updates_.push_back(std::move(update));
    ++official;
  }

  summary_.official_updates = official;
  summary_.foreign_packages = foreign;
  summary_.total_updates = official;

  // AUR updates come from the attached provider only. The default provider is
  // NoneAurProvider, which never executes a helper, so AUR detection is an
  // explicit, infrequent, read-only operation that never affects official
  // repository detection.
  if (aur_provider_ != nullptr && aur_provider_->available()) {
    std::vector<PackageUpdate> aur_updates = aur_provider_->checkUpdates();
    for (PackageUpdate& update : aur_updates) {
      update.source = PackageSource::AUR;
      update.is_aur = true;
    }
    summary_.aur_updates = aur_updates.size();
    summary_.total_updates += aur_updates.size();
    updates_.insert(updates_.end(), aur_updates.begin(), aur_updates.end());
  }

  summary_.refresh_in_progress = false;
  return true;
}

const PackageUpdate* PackageManager::findUpdate(const std::string& name) const {
  for (const PackageUpdate& update : updates_) {
    if (update.name == name) {
      return &update;
    }
  }
  return nullptr;
}

}  // namespace atm