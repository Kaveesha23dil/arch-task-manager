#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace atm {

/// Lifecycle of a package transaction. The UI renders the current state so the
/// user always knows whether work is in progress.
enum class TransactionState {
  Idle,                  // no transaction running
  RefreshingDatabases,   // synchronising repository metadata (explicit)
  ResolvingDependencies, // libalpm is computing the transaction target list
  AwaitingConfirmation,  // preview computed, user verification required
  Downloading,           // fetching packages
  Installing,            // applying the transaction to the system
  Completed,             // finished successfully
  Failed,                // stopped with an error
  Cancelled,             // aborted by the user before commit
};

/// Human-readable name of a TransactionState.
[[nodiscard]] const char *transactionStateName(TransactionState state);

/// One package that participates in the computed transaction.
struct TransactionPackage {
  std::string name;
  std::string old_version;
  std::string new_version;
  std::string repository;
  bool will_remove = false;  // true => this is a removal, not an upgrade
};

/// The result of calculating and executing a transaction. Held in the
/// transaction object and also kept in a bounded recent-results log.
struct TransactionResult {
  bool success = false;
  bool cancelled = false;
  std::string summary;   // one-line human summary ("17 packages upgraded")
  std::string detail;    // multi-line detail (error context, reboot note, ...)

  std::size_t upgraded = 0;
  std::size_t installed = 0;
  std::size_t removed = 0;

  std::chrono::system_clock::time_point timestamp;
};

/// Read-only view of a running/libalpm-computed transaction preview.
struct TransactionPreview {
  std::size_t to_upgrade = 0;
  std::size_t to_install = 0;
  std::size_t to_remove = 0;
  std::size_t to_reinstall = 0;
  std::uint64_t download_size = 0;      // bytes, 0 = unknown
  std::int64_t size_change = 0;         // bytes, signed (install minus remove)
  std::vector<TransactionPackage> packages;
  bool has_removals = false;            // true if any package is removed
};

/// Thread-safe callback sink used to surface transaction progress to the UI.
///
/// libalpm invokes the callbacks on the worker thread; the sink forwards the
/// latest values to a struct that the UI thread can read without blocking.
/// Exactly one transaction runs at a time; no locking is needed inside this
/// struct beyond the single mutex guarding the shared progress fields.
class TransactionProgress {
 public:
  TransactionProgress() = default;
  TransactionProgress(const TransactionProgress &) = delete;
  TransactionProgress &operator=(const TransactionProgress &) = delete;

  /// The operation phases libalpm reports progress for.
  enum class Phase { Install, Upgrade, Downgrade, Remove, Overall };

  /// Set by libalpm callbacks and read by the progress renderer.
  void setPhase(Phase phase, const std::string &what);
  void setPercent(Phase phase, int percent);
  void setMessage(const std::string &message);
  void setDownload(const std::string &filename, int percent);
  void reset();

  /// Interrupt/cancel coordination with the worker thread.
  std::atomic_bool cancel_requested{false};  // set by the UI, honoured by commit
  std::atomic_bool cancel_acknowledged{false};  // set by commit when it stops

  /// Current activity text (e.g. "Downloading firefox-142.0-1-x86_64.pkg.tar.zst").
  [[nodiscard]] std::string activity() const;

  /// Latest percent for a phase (-1 when none reported yet).
  [[nodiscard]] int percent(Phase phase) const;

  /// Latest download percent (-1 when none reported yet).
  [[nodiscard]] int downloadPercent() const;

  /// File currently being downloaded ("" when none).
  [[nodiscard]] std::string downloadFile() const;

  /// True once libalpm has entered the install/commit phase.
  [[nodiscard]] bool inInstallPhase() const;

 private:
  mutable std::mutex mutex_;
  std::string activity_;
  std::string message_;
  int install_percent_ = -1;
  int upgrade_percent_ = -1;
  int downgrade_percent_ = -1;
  int remove_percent_ = -1;
  int overall_percent_ = -1;
  int download_percent_ = -1;
  std::string download_file_;
  bool in_install_phase_ = false;
};

/**
 * Executes an explicit, user-confirmed full system upgrade using libalpm.
 *
 * The transaction is deliberately kept separate from the UI: the caller drives
 * the state machine (refresh databases, resolve, preview, confirm, commit) and
 * the UI only reads state/progress/results and issues the confirmation.
 *
 * Privilege handling: the transaction itself never calls sudo or collects a
 * password. Databases that require root (the local database under /var/lib/
 * pacman and the package cache) are opened with libalpm's normal root path and
 * file ownership rules; on Arch, `alpm_trans_commit` refuses operations it
 * cannot perform, reporting ALPM_ERR_BADPERMS. For a non-root process the
 * application surfaces that error clearly rather than attempting to elevate.
 *
 * Only one transaction may be active at a time. The class is non-copyable and
 * guarded with a mutex; concurrent attempts to start a transaction are refused.
 */
class PackageTransaction {
 public:
  PackageTransaction();
  ~PackageTransaction();

  PackageTransaction(const PackageTransaction &) = delete;
  PackageTransaction &operator=(const PackageTransaction &) = delete;

  /// Opens libalpm and registers sync databases (same discovery as the package
  /// manager). Safe on any distribution; returns false when unsupported.
  bool initialize();

  /// Synchronises repository metadata (explicit; may touch the network and
  /// modify /var/lib/pacman). Returns false on failure.
  bool syncDatabases();

  /// Computes the full-system-upgrade target list and fills preview(). Does
  /// not modify the system; requires syncDatabases() first. Returns false and
  /// fills error state on lock/availability failures.
  bool resolve();

  /// Executes the already-computed and user-confirmed transaction. Returns
  /// when the transaction finishes. On a non-root process this typically
  /// fails with a clear permission error before modifying anything.
  bool commit(TransactionProgress *progress);

  /// Aborts before commit (e.g. user declines confirmation). No system change.
  void cancel();

  /// Safely requests interruption of a running download phase. During the
  /// install phase libalpm may not support interruption; that is reported back
  /// to the caller via a response flag in the progress struct.
  void requestInterrupt(TransactionProgress *progress);

  /// True while a transaction, sync or resolve is running.
  [[nodiscard]] bool is_running() const;

  /// True when this is an Arch-based distribution.
  [[nodiscard]] bool supported() const;

  /// Current state.
  [[nodiscard]] TransactionState state() const;

  /// Human explanation for the current error state.
  [[nodiscard]] const std::string &error() const;

  /// The computed preview (meaningful after a successful resolve()).
  [[nodiscard]] const TransactionPreview &preview() const;

  /// Result of the most recent commit attempt.
  [[nodiscard]] const TransactionResult &result() const;

  /// Bounded in-memory log of recent transaction results (oldest first).
  [[nodiscard]] std::vector<TransactionResult> recent() const;

  /// True when any package name in `names` appears in the kernel or a known
  /// recomended-reboot package set (used for the informational reboot note).
  [[nodiscard]] bool includesRebootPackage(const std::vector<std::string> &names) const;

 private:
  mutable std::mutex mutex_;

  TransactionState state_ = TransactionState::Idle;
  std::string error_;
  TransactionPreview preview_;
  TransactionResult result_;
  std::vector<TransactionResult> recent_;  // bounded (kMaxRecent)
  bool supported_ = false;

  void *handle_ = nullptr;  // alpm_handle_t*
  bool gpg_available_ = false;

  static constexpr std::size_t kMaxRecent = 10;

  /// Detects Arch from /etc/os-release.
  void detectDistribution();

  /// Opens libalpm and registers sync databases from pacman.conf.
  bool openAlpm();

  /// Releases the libalpm handle (closes databases, drops lock).
  void closeAlpm();

  /// Splits an alpm error code into a friendly message + optional debug hint.
  [[nodiscard]] std::string describeAlpmError(int code) const;

  /// Do a permission pre-check before commit; fills error_ on failure.
  bool checkPermissions();

  /// Appends a result to the bounded recent log (caller holds the mutex).
  void recordResult(bool success, bool cancelled);

  /// True iff a transaction is in a running state (caller holds the mutex).
  [[nodiscard]] bool is_running_locked() const;
};

}  // namespace atm
