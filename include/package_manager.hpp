#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace atm {

/// Where a package comes from. Official packages are found in one of the
/// configured sync repositories; foreign packages are installed locally but
/// are not present in any sync database. A foreign package is only labelled
/// AUR when a read-only AUR provider positively identifies it.
enum class PackageSource {
  Official,  // present in a sync repository (core, extra, multilib, ...)
  AUR,       // positively identified as an AUR package by an AUR provider
  Foreign,   // installed locally but absent from every sync repository
  Unknown,   // could not be classified
};

/// Human-readable name of a PackageSource.
[[nodiscard]] const char *packageSourceName(PackageSource source);

/// One detected package that has a newer version available.
struct PackageUpdate {
  std::string name;
  std::string installed_version;
  std::string available_version;
  std::string repository;      // sync repo name for official, "" otherwise
  bool is_aur = false;         // convenience flag (see source)
  PackageSource source = PackageSource::Unknown;

  // Extended metadata used by the package-details screen.
  std::string description;
  std::string architecture;
  std::uint64_t installed_size = 0;  // bytes, 0 = unknown
  int install_reason = 0;            // alpm_pkgreason_t (explicit/depend)
};

/// Human-readable name of an alpm install reason (explicit / dependency).
[[nodiscard]] const char *packageInstallReasonName(int reason);

/// Result of a refresh: counts plus status for the UI.
struct PackageUpdateSummary {
  std::size_t total_updates = 0;
  std::size_t official_updates = 0;
  std::size_t aur_updates = 0;
  std::size_t foreign_packages = 0;
  bool supported = false;        // this is an Arch-based distro
  bool initialized = false;      // libalpm was opened successfully
  bool refresh_in_progress = false;
  bool refresh_failed = false;
  std::string error_message;     // human explanation when refresh_failed
  std::string distribution;      // PRETTY_NAME from /etc/os-release
  std::string aur_helper;        // detected yay/paru, "" when none present
};

/// Read-only AUR update provider. Kept behind an interface so the core
/// official-repository detection never depends on an AUR helper. AUR checking
/// is an explicit, infrequent operation (never every monitoring tick).
class AurProvider {
 public:
  virtual ~AurProvider() = default;

  /// Returns updates whose available_version was obtained from the AUR.
  /// Must only use machine-readable, trusted sources; must never execute an
  /// AUR helper or parse arbitrary shell output. May return an empty vector
  /// when AUR checking is not configured.
  virtual std::vector<PackageUpdate> checkUpdates() = 0;

  /// Whether this provider can currently report AUR updates.
  [[nodiscard]] virtual bool available() const = 0;
};

/// Default provider: does not check the AUR at all. Kept as the default so
/// the package manager works without yay/paru and never executes a helper.
class NoneAurProvider final : public AurProvider {
 public:
  std::vector<PackageUpdate> checkUpdates() override { return {}; }
  [[nodiscard]] bool available() const override { return false; }
};

/**
 * Arch Linux package update manager.
 *
 * Pure detection and presentation: it reports which installed packages have
 * newer versions available in the configured sync repositories. It never
 * installs, upgrades, removes, or synchronises package databases, never runs
 * pacman/AUR helpers, never needs root, and never parses command output.
 *
 * Official data comes from libalpm: the local database is inspected for
 * installed packages and `alpm_sync_get_new_version()` (which uses libalpm's
 * native version comparison) is used to find newer versions already present
 * in the local sync databases. No network access is required. Sync databases
 * are registered from the repository sections of pacman.conf so no repo names
 * are hard-coded.
 *
 * Results are cached until the next explicit refresh(); see summary().
 */
class PackageManager {
 public:
  PackageManager();
  ~PackageManager();

  PackageManager(const PackageManager &) = delete;
  PackageManager &operator=(const PackageManager &) = delete;

  /// Detects Arch Linux from /etc/os-release and opens libalpm. Safe to call
  /// on any distribution; non-Arch systems report unsupported() instead of
  /// failing. Does not touch the network or any package database.
  bool initialize();

  /// Re-reads the local package database and resync-registered databases and
  /// rebuilds the update list. Never modifies databases, never requires
  /// network access, never runs as root. Cheap enough to call on demand only.
  bool refresh();

  /// Attaches an AUR provider used by refresh() to fill in AUR updates. The
  /// default (NoneAurProvider) reports no AUR updates; core official-repo
  /// detection is never affected by the provider.
  void setAurProvider(AurProvider* provider) { aur_provider_ = provider; }

  /// True when /etc/os-release identifies an Arch-based distribution.
  [[nodiscard]] bool supported() const { return summary_.supported; }

  /// True when libalpm was opened successfully and data can be read.
  [[nodiscard]] bool initialized() const { return summary_.initialized; }

  /// The last refresh summary (available even before the first refresh()).
  [[nodiscard]] const PackageUpdateSummary &summary() const { return summary_; }

  /// Updates detected by the last refresh (empty before any refresh).
  [[nodiscard]] const std::vector<PackageUpdate> &updates() const { return updates_; }

  /// Look up one detected update by package name (nullptr when absent).
  [[nodiscard]] const PackageUpdate *findUpdate(const std::string &name) const;

 private:
  PackageUpdateSummary summary_;
  std::vector<PackageUpdate> updates_;
  AurProvider* aur_provider_ = nullptr;  // owned by the caller

  void *handle_ = nullptr;  // alpm_handle_t*

  /// Reads /etc/os-release and fills supported_/distribution_.
  void detectDistribution();

  /// Opens libalpm and registers sync databases from pacman.conf.
  bool openAlpm();

  /// Releases libalpm (closes databases, drops lock).
  void closeAlpm();
};

}  // namespace atm