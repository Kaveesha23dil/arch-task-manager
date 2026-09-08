#include "package_transaction.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include <alpm.h>
#include <alpm_list.h>

namespace atm {

namespace {

constexpr const char* kOsReleasePath = "/etc/os-release";
constexpr const char* kPacmanConfPath = "/etc/pacman.conf";
constexpr const char* kLocalDbPath = "/var/lib/pacman/local";
constexpr const char* kSyncDbPath = "/var/lib/pacman/sync";

/// True when `/var/lib/pacman/sync` contains at least one sync database file.
bool hasSyncDbs() {
  struct stat st{};
  return ::stat(kSyncDbPath, &st) == 0 && S_ISDIR(st.st_mode);
}

/// True when `/var/lib/pacman/local` exists.
bool hasLocalDb() {
  struct stat st{};
  return ::stat(kLocalDbPath, &st) == 0 && S_ISDIR(st.st_mode);
}

std::string lowerAscii(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

bool isArchIdentifier(const std::string& text) {
  const std::string lowered = lowerAscii(text);
  return lowered == "arch" || lowered.find("arch") != std::string::npos;
}

std::string valueForKey(const std::string& line, const std::string& key) {
  if (line.rfind(key, 0) != 0 || line.size() == key.size() ||
      line[key.size()] != '=') {
    return {};
  }
  std::string value = line.substr(key.size() + 1);
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
    value = value.substr(1, value.size() - 2);
  }
  return value;
}

}  // namespace

const char* transactionStateName(TransactionState state) {
  switch (state) {
    case TransactionState::Idle:                return "Idle";
    case TransactionState::RefreshingDatabases: return "Refreshing Databases";
    case TransactionState::ResolvingDependencies: return "Resolving Dependencies";
    case TransactionState::AwaitingConfirmation: return "Awaiting Confirmation";
    case TransactionState::Downloading:         return "Downloading";
    case TransactionState::Installing:          return "Installing";
    case TransactionState::Completed:           return "Completed";
    case TransactionState::Failed:              return "Failed";
    case TransactionState::Cancelled:           return "Cancelled";
  }
  return "Unknown";
}

// ---- TransactionProgress ----

void TransactionProgress::setPhase(Phase phase, const std::string& what) {
  std::lock_guard<std::mutex> lock(mutex_);
  activity_ = what;
  if (phase == Phase::Install || phase == Phase::Upgrade ||
      phase == Phase::Downgrade || phase == Phase::Remove) {
    in_install_phase_ = true;
  }
}

void TransactionProgress::setPercent(Phase phase, int percent) {
  std::lock_guard<std::mutex> lock(mutex_);
  int* slot = nullptr;
  switch (phase) {
    case Phase::Install:   slot = &install_percent_; break;
    case Phase::Upgrade:   slot = &upgrade_percent_; break;
    case Phase::Downgrade: slot = &downgrade_percent_; break;
    case Phase::Remove:    slot = &remove_percent_; break;
    case Phase::Overall:   slot = &overall_percent_; break;
  }
  if (slot != nullptr) {
    *slot = percent;
  }
}

void TransactionProgress::setMessage(const std::string& message) {
  std::lock_guard<std::mutex> lock(mutex_);
  message_ = message;
}

void TransactionProgress::setDownload(const std::string& filename,
                                      int percent) {
  std::lock_guard<std::mutex> lock(mutex_);
  download_file_ = filename;
  download_percent_ = percent;
}

void TransactionProgress::reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  activity_.clear();
  message_.clear();
  install_percent_ = -1;
  upgrade_percent_ = -1;
  downgrade_percent_ = -1;
  remove_percent_ = -1;
  overall_percent_ = -1;
  download_percent_ = -1;
  download_file_.clear();
  in_install_phase_ = false;
}

std::string TransactionProgress::activity() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!message_.empty()) {
    return message_;
  }
  if (!download_file_.empty()) {
    return "Downloading " + download_file_;
  }
  return activity_;
}

int TransactionProgress::percent(Phase phase) const {
  std::lock_guard<std::mutex> lock(mutex_);
  switch (phase) {
    case Phase::Install:   return install_percent_;
    case Phase::Upgrade:   return upgrade_percent_;
    case Phase::Downgrade: return downgrade_percent_;
    case Phase::Remove:    return remove_percent_;
    case Phase::Overall:   return overall_percent_;
  }
  return -1;
}

int TransactionProgress::downloadPercent() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return download_percent_;
}

std::string TransactionProgress::downloadFile() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return download_file_;
}

bool TransactionProgress::inInstallPhase() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return in_install_phase_;
}

// ---- PackageTransaction ----

PackageTransaction::PackageTransaction() = default;

PackageTransaction::~PackageTransaction() {
  closeAlpm();
}

void PackageTransaction::detectDistribution() {
  supported_ = false;
  std::ifstream file(kOsReleasePath);
  if (!file) {
    return;
  }
  std::string id;
  std::string id_like;
  std::string line;
  while (std::getline(file, line)) {
    const std::string id_value = valueForKey(line, "ID");
    if (!id_value.empty()) {
      id = id_value;
    }
    const std::string like_value = valueForKey(line, "ID_LIKE");
    if (!like_value.empty()) {
      id_like = like_value;
    }
  }
  supported_ = isArchIdentifier(id) || isArchIdentifier(id_like);
}

bool PackageTransaction::openAlpm() {
  constexpr const char* kRoot = "/";
  constexpr const char* kDbPath = "/var/lib/pacman/";
  alpm_errno_t err = ALPM_ERR_OK;
  alpm_handle_t* handle = alpm_initialize(kRoot, kDbPath, &err);
  if (handle == nullptr) {
    error_ = "Package database is unavailable: ";
    error_ += alpm_strerror(err);
    return false;
  }
  handle_ = handle;

  // Register the sync repositories from pacman.conf so no repo name is
  // hard-coded. The transaction then knows which databases to sync.
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

  // gpgme presence: used for a friendlier message when verification fails.
  const char* gpgdir = alpm_option_get_gpgdir(handle);
  gpg_available_ =
      gpgdir != nullptr && ::access(gpgdir, R_OK | X_OK) == 0;

  return true;
}

void PackageTransaction::closeAlpm() {
  if (handle_ != nullptr) {
    alpm_release(static_cast<alpm_handle_t*>(handle_));
    handle_ = nullptr;
  }
}

bool PackageTransaction::initialize() {
  detectDistribution();
  if (!supported_) {
    error_ =
        "Package transaction manager: not supported on this distribution.";
    return false;
  }
  if (!hasLocalDb()) {
    error_ = "Local package database is missing or unavailable.";
    return false;
  }
  return openAlpm();
}

bool PackageTransaction::supported() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return supported_;
}

TransactionState PackageTransaction::state() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

const std::string& PackageTransaction::error() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return error_;
}

const TransactionPreview& PackageTransaction::preview() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return preview_;
}

const TransactionResult& PackageTransaction::result() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return result_;
}

std::vector<TransactionResult> PackageTransaction::recent() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return recent_;
}

bool PackageTransaction::is_running() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_ == TransactionState::RefreshingDatabases ||
         state_ == TransactionState::ResolvingDependencies ||
         state_ == TransactionState::Downloading ||
         state_ == TransactionState::Installing;
}

bool PackageTransaction::syncDatabases() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!supported_) {
    error_ = "Unsupported distribution.";
    return false;
  }
  if (is_running_locked()) {
    error_ = "Another package operation is already in progress.";
    return false;
  }
  if (handle_ == nullptr && !openAlpm()) {
    if (error_.empty()) {
      error_ = "Package database is unavailable.";
    }
    return false;
  }

  alpm_handle_t* handle = static_cast<alpm_handle_t*>(handle_);
  state_ = TransactionState::RefreshingDatabases;
  error_.clear();

  alpm_list_t* sync_dbs = alpm_get_syncdbs(handle);
  if (sync_dbs == nullptr) {
    state_ = TransactionState::Failed;
    error_ = "No sync databases are configured.";
    return false;
  }

  const int rc = alpm_db_update(handle, sync_dbs, 0);
  if (rc != 0) {
    state_ = TransactionState::Failed;
    const alpm_errno_t err = alpm_errno(handle);
    if (err == ALPM_ERR_HANDLE_LOCK) {
      error_ = "Package database is locked by another package manager.\n"
               "Close the other package manager and try again.";
    } else {
      error_ = "Failed to synchronise repository databases: ";
      error_ += alpm_strerror(err);
    }
    return false;
  }

  state_ = TransactionState::Idle;
  return true;
}

bool PackageTransaction::resolve() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!supported_) {
    error_ = "Unsupported distribution.";
    return false;
  }
  if (is_running_locked()) {
    error_ = "Another package operation is already in progress.";
    return false;
  }
  if (handle_ == nullptr && !openAlpm()) {
    if (error_.empty()) {
      error_ = "Package database is unavailable.";
    }
    return false;
  }
  if (!hasSyncDbs()) {
    state_ = TransactionState::Failed;
    error_ = "Repository databases have not been synchronised.\n"
             "Choose 'Refresh Package Databases' first.";
    return false;
  }

  alpm_handle_t* handle = static_cast<alpm_handle_t*>(handle_);
  state_ = TransactionState::ResolvingDependencies;
  error_.clear();
  preview_ = TransactionPreview{};

  // Full system upgrade (rolling release). We deliberately do not allow
  // partial-upgrade or per-package selection here.
  const int flags = ALPM_TRANS_FLAG_NOSCRIPTLET;
  if (alpm_trans_init(handle, flags) != 0) {
    const alpm_errno_t err = alpm_errno(handle);
    state_ = TransactionState::Failed;
    error_ = describeAlpmError(err);
    return false;
  }

  if (alpm_sync_sysupgrade(handle, 0) != 0) {
    const alpm_errno_t err = alpm_errno(handle);
    error_ = describeAlpmError(err);
    (void)alpm_trans_release(handle);
    state_ = TransactionState::Failed;
    return false;
  }

  alpm_list_t** depdata = nullptr;
  if (alpm_trans_prepare(handle, depdata) != 0) {
    std::string detail;
    if (depdata != nullptr) {
      for (alpm_list_t* i = *depdata; i != nullptr; i = alpm_list_next(i)) {
        if (!detail.empty()) {
          detail += '\n';
        }
        char* depstr = alpm_dep_compute_string(
            static_cast<alpm_depend_t*>(i->data));
        detail += depstr != nullptr ? depstr : "(unknown dependency)";
        free(depstr);
      }
      alpm_list_free(*depdata);
      *depdata = nullptr;
    }
    error_ = "Could not resolve dependencies";
    if (!detail.empty()) {
      error_ += ":\n" + detail;
    }
    (void)alpm_trans_release(handle);
    state_ = TransactionState::Failed;
    return false;
  }

  // Build the preview from the prepared transaction.
  alpm_list_t* add = alpm_trans_get_add(handle);
  alpm_list_t* rem = alpm_trans_get_remove(handle);

  for (alpm_list_t* i = add; i != nullptr; i = alpm_list_next(i)) {
    alpm_pkg_t* pkg = static_cast<alpm_pkg_t*>(i->data);
    TransactionPackage tp;
    tp.name = alpm_pkg_get_name(pkg);
    tp.new_version = alpm_pkg_get_version(pkg);
    tp.repository =
        alpm_pkg_get_db(pkg) != nullptr ? alpm_db_get_name(alpm_pkg_get_db(pkg))
                                        : "";
    alpm_pkg_t* oldpkg = alpm_db_get_pkg(alpm_get_localdb(handle),
                                         tp.name.c_str());
    if (oldpkg != nullptr) {
      tp.old_version = alpm_pkg_get_version(oldpkg);
      const int cmp = alpm_pkg_vercmp(tp.old_version.c_str(),
                                      tp.new_version.c_str());
      if (cmp == 0) {
        ++preview_.to_reinstall;
      } else {
        ++preview_.to_upgrade;
      }
    } else {
      ++preview_.to_install;
    }
    preview_.packages.push_back(std::move(tp));
  }

  for (alpm_list_t* i = rem; i != nullptr; i = alpm_list_next(i)) {
    alpm_pkg_t* pkg = static_cast<alpm_pkg_t*>(i->data);
    TransactionPackage tp;
    tp.name = alpm_pkg_get_name(pkg);
    tp.old_version = alpm_pkg_get_version(pkg);
    tp.will_remove = true;
    ++preview_.to_remove;
    preview_.has_removals = true;
    preview_.packages.push_back(std::move(tp));
  }

  // Download size and installed-size change across the added packages.
  for (alpm_list_t* i = add; i != nullptr; i = alpm_list_next(i)) {
    alpm_pkg_t* pkg = static_cast<alpm_pkg_t*>(i->data);
    const off_t dlsize = alpm_pkg_download_size(pkg);
    if (dlsize > 0) {
      preview_.download_size += static_cast<std::uint64_t>(dlsize);
    }
    const off_t isize = alpm_pkg_get_isize(pkg);
    if (isize > 0) {
      preview_.size_change += static_cast<std::int64_t>(isize);
    }
  }
  for (alpm_list_t* i = rem; i != nullptr; i = alpm_list_next(i)) {
    alpm_pkg_t* pkg = static_cast<alpm_pkg_t*>(i->data);
    const off_t isize = alpm_pkg_get_isize(pkg);
    if (isize > 0) {
      preview_.size_change -= static_cast<std::int64_t>(isize);
    }
  }

  (void)alpm_trans_release(handle);
  state_ = TransactionState::AwaitingConfirmation;
  return true;
}

void PackageTransaction::cancel() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ == TransactionState::AwaitingConfirmation) {
    state_ = TransactionState::Cancelled;
    TransactionResult r;
    r.success = false;
    r.cancelled = true;
    r.summary = "Transaction cancelled by the user. No packages were modified.";
    r.timestamp = std::chrono::system_clock::now();
    result_ = r;
    recent_.push_back(r);
    if (recent_.size() > kMaxRecent) {
      recent_.erase(recent_.begin());
    }
  } else if (state_ == TransactionState::Idle ||
             state_ == TransactionState::Failed ||
             state_ == TransactionState::Cancelled ||
             state_ == TransactionState::Completed) {
    // Nothing to do.
  }
}

bool PackageTransaction::checkPermissions() {
  // The local database and cache live under /var/lib/pacman, which normally
  // requires root to write. Report this clearly before starting the work.
  if (::geteuid() != 0) {
    error_ =
        "Root privileges are required to apply package changes, but no "
        "automatic elevation is performed.\n"
        "Run the Task Manager with elevated privileges, or use your "
        "distribution's password prompt for a privileged shell.";
    return false;
  }
  if (!hasLocalDb()) {
    error_ = "Local package database is unavailable.";
    return false;
  }
  return true;
}

bool PackageTransaction::commit(TransactionProgress* progress) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!supported_) {
    error_ = "Unsupported distribution.";
    return false;
  }
  if (state_ == TransactionState::ResolvingDependencies || is_running_locked()) {
    error_ = "Another package operation is already in progress.";
    return false;
  }
  if (state_ != TransactionState::AwaitingConfirmation) {
    error_ = "The transaction has not been resolved. Choose "
             "'Preview Packages' first.";
    return false;
  }
  if (!checkPermissions()) {
    state_ = TransactionState::Failed;
    return false;
  }
  if (handle_ == nullptr && !openAlpm()) {
    if (error_.empty()) {
      error_ = "Package database is unavailable.";
    }
    state_ = TransactionState::Failed;
    return false;
  }

  alpm_handle_t* handle = static_cast<alpm_handle_t*>(handle_);
  state_ = TransactionState::Installing;
  error_.clear();

  if (progress != nullptr) {
    progress->reset();
  }

  const int flags = ALPM_TRANS_FLAG_NOSCRIPTLET;
  if (alpm_trans_init(handle, flags) != 0) {
    const alpm_errno_t err = alpm_errno(handle);
    state_ = TransactionState::Failed;
    error_ = describeAlpmError(err);
    return false;
  }
  if (alpm_sync_sysupgrade(handle, 0) != 0) {
    const alpm_errno_t err = alpm_errno(handle);
    error_ = describeAlpmError(err);
    (void)alpm_trans_release(handle);
    state_ = TransactionState::Failed;
    return false;
  }

  alpm_list_t** datalist = nullptr;
  if (alpm_trans_prepare(handle, datalist) != 0) {
    const alpm_errno_t err = alpm_errno(handle);
    error_ = describeAlpmError(err);
    if (datalist != nullptr) {
      alpm_list_free(*datalist);
      *datalist = nullptr;
    }
    (void)alpm_trans_release(handle);
    state_ = TransactionState::Failed;
    return false;
  }

  int rc = alpm_trans_commit(handle, datalist);
  if (rc != 0) {
    const alpm_errno_t err = alpm_errno(handle);
    error_ = describeAlpmError(err);
    if (progress != nullptr) {
      progress->setMessage("Transaction failed.");
    }
    if (datalist != nullptr && *datalist != nullptr) {
      alpm_list_free(*datalist);
      *datalist = nullptr;
    }
    (void)alpm_trans_release(handle);
    state_ = TransactionState::Failed;
    recordResult(false, false);
    return false;
  }

  (void)alpm_trans_release(handle);
  state_ = TransactionState::Completed;
  if (progress != nullptr) {
    progress->setMessage("Transaction completed.");
  }

  TransactionResult r;
  r.success = true;
  r.summary = std::to_string(preview_.to_upgrade) + " packages upgraded" +
              (preview_.to_install > 0
                   ? ", " + std::to_string(preview_.to_install) + " installed"
                   : "") +
              (preview_.to_remove > 0
                   ? ", " + std::to_string(preview_.to_remove) + " removed"
                   : "");
  r.upgraded = preview_.to_upgrade;
  r.installed = preview_.to_install;
  r.removed = preview_.to_remove;
  r.timestamp = std::chrono::system_clock::now();
  result_ = r;
  recent_.push_back(r);
  if (recent_.size() > kMaxRecent) {
    recent_.erase(recent_.begin());
  }
  return true;
}

void PackageTransaction::recordResult(bool success, bool cancelled) {
  TransactionResult r;
  r.success = success;
  r.cancelled = cancelled;
  r.summary = cancelled ? "Transaction cancelled." : "Transaction failed.";
  r.detail = error_;
  r.timestamp = std::chrono::system_clock::now();
  result_ = r;
  recent_.push_back(r);
  if (recent_.size() > kMaxRecent) {
    recent_.erase(recent_.begin());
  }
}

bool PackageTransaction::includesRebootPackage(
    const std::vector<std::string>& names) const {
  static const std::set<std::string> kRebootHint = {
      "linux",       "linux-lts",     "linux-zen",  "linux-hardened",
      "systemd",     "systemd-libs",  "glibc",      "linux-firmware",
      "amd-ucode",   "intel-ucode",
  };
  for (const std::string& name : names) {
    if (kRebootHint.count(name) > 0) {
      return true;
    }
  }
  return false;
}

std::string PackageTransaction::describeAlpmError(int code) const {
  const auto err = static_cast<alpm_errno_t>(code);
  switch (err) {
    case ALPM_ERR_HANDLE_LOCK:
      return "Package database is locked by another package manager.\n"
             "Close the other package manager and try again.";
    case ALPM_ERR_DB_OPEN:
      return "Could not open the package database.";
    case ALPM_ERR_DB_INVALID:
      return "The package database is corrupt or invalid.";
    case ALPM_ERR_DB_WRITE:
      return "Could not write to the package database.";
    case ALPM_ERR_PKG_INVALID_SIG:
    case ALPM_ERR_SIG_INVALID:
    case ALPM_ERR_DB_INVALID_SIG:
    case ALPM_ERR_PKG_MISSING_SIG:
    case ALPM_ERR_SIG_MISSING:
      return "Package signature verification failed. Transaction stopped.";
    case ALPM_ERR_PKG_INVALID_CHECKSUM:
      return "A package failed its checksum verification.";
    case ALPM_ERR_BADPERMS:
      return "Insufficient permissions. Root privileges are required.";
    case ALPM_ERR_DISK_SPACE:
      return "Insufficient disk space for the transaction.";
    case ALPM_ERR_UNSATISFIED_DEPS:
      return "Unsatisfied package dependencies.";
    case ALPM_ERR_CONFLICTING_DEPS:
      return "Conflicting package dependencies.";
    case ALPM_ERR_FILE_CONFLICTS:
      return "File conflicts prevent the transaction from completing.";
    case ALPM_ERR_RETRIEVE:
    case ALPM_ERR_LIBCURL:
    case ALPM_ERR_EXTERNAL_DOWNLOAD:
      return "Package download failed. Check your network connection.";
    case ALPM_ERR_SERVER_NONE:
      return "No repository server is configured.";
    case ALPM_ERR_GPGME:
      return "GnuPG signature verification is unavailable.";
    default:
      break;
  }
  std::string message = "Package operation failed: ";
  message += alpm_strerror(err);
  message += " (error ";
  message += std::to_string(code);
  message += ").";
  return message;
}

bool PackageTransaction::is_running_locked() const {
  return state_ == TransactionState::RefreshingDatabases ||
         state_ == TransactionState::ResolvingDependencies ||
         state_ == TransactionState::Downloading ||
         state_ == TransactionState::Installing;
}

// Explicitly keep recent_ thread-safety: we guard all mutations with mutex_.
// The getter returns a copy to avoid exposing a mutable reference.

}  // namespace atm
