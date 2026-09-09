#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "process_scheduling.hpp"

namespace atm {

/// Upper bound on the total bytes read from a process's /proc/<pid>/environ.
/// Real environments are a few kilobytes; the cap is purely defensive so a
/// hostile or corrupted process can never make the inspector read unbounded
/// memory. Reading never blocks meaningfully: the file is a kernel snapshot.
constexpr std::size_t kDefaultMaxEnvironmentBytes = 256U * 1024U;

/// Upper bound on the number of environment variables collected for one
/// process. Keeps output bounded even for a degenerate process.
constexpr std::size_t kDefaultMaxEnvironmentVariables = 1024;

/// The placeholder shown (and stored) for a sensitive variable's value. It is
/// the only value representation that ever leaves the parser: plaintext
/// secrets are never kept in the result model at all.
constexpr std::string_view kMaskedSecretPlaceholder = "********";

/// Outcome of reading a process's environment.
enum class EnvironmentStatus {
  Success,          // the environment was read (possibly empty or truncated)
  InvalidPid,       // pid <= 0, rejected before any system access
  ProcessNotFound,  // /proc/<pid> is absent (the process has exited / ESRCH)
  IdentityUnknown,  // the process identity could not be re-read before applying
  ProcessReused,    // the PID was reused by a different process since selection
  PermissionDenied, // EACCES / EPERM while opening /proc/<pid>/environ
  EmptyEnvironment, // the environment file was genuinely empty
  MalformedData,    // the file had bytes but not one well-formed NAME=value record
  ReadError,        // any other failure reading /proc/<pid>/environ
};

/// One environment variable of the selected process.
///
/// The variable name and value stay together once parsed. For a variable
/// classified as potentially sensitive `value` holds kMaskedSecretPlaceholder
/// — the plaintext secret is never stored, displayed, logged, copied or
/// persisted by this component. Non-sensitive values are kept verbatim.
struct ProcessEnvironmentEntry {
  std::string name;    // variable name, e.g. "PATH"
  std::string value;   // "********" when sensitive, the raw value otherwise
  bool sensitive = false;  // classifier result (heuristic, may over- or under-match)
};

/// Statistics + parsed rows produced from one /proc/<pid>/environ snapshot.
struct EnvironmentParseResult {
  std::vector<ProcessEnvironmentEntry> entries;  // sorted by name ascending
  std::size_t record_count = 0;      // every NAME=value record seen
  std::size_t duplicate_count = 0;   // records whose name was already seen (dropped)
  std::size_t malformed_count = 0;   // records without a valid NAME=... form
  bool variable_truncated = false;   // record_count hit the variable limit
};

/// Result of one environment inspection.
struct ProcessEnvironmentResult {
  EnvironmentStatus status = EnvironmentStatus::ReadError;
  int errno_value = 0;
  std::vector<ProcessEnvironmentEntry> entries;  // sorted by name ascending
  std::uint64_t byte_count = 0;   // bytes actually read from the file
  std::size_t sensitive_count = 0;
  std::size_t duplicate_count = 0;
  std::size_t malformed_count = 0;
  bool size_truncated = false;      // total file bytes exceeded the byte limit
  bool variable_truncated = false;  // record count exceeded the variable limit

  [[nodiscard]] bool success() const {
    return status == EnvironmentStatus::Success ||
           status == EnvironmentStatus::EmptyEnvironment;
  }
};

/**
 * Case-insensitive, conservative sensitivity classifier for a variable name.
 *
 * A variable is treated as potentially sensitive when its name (compared
 * case-insensitively) contains one of the known secret-bearing tokens:
 * PASSWORD, PASSWD, SECRET, TOKEN, API_KEY, APIKEY, ACCESS_KEY, PRIVATE_KEY,
 * CREDENTIAL, AUTH, BEARER, SESSION, COOKIE. Substring matching means
 * compound names such as DATABASE_PASSWORD, GITHUB_TOKEN,
 * AWS_SECRET_ACCESS_KEY and AUTH_TOKEN are all caught without an exact-match
 * table. The heuristic may flag a harmless variable (over-masking is safe) or
 * miss a genuinely secret one — it is a heuristic, not a guarantee. Extend
 * kSensitiveSubstrings to broaden coverage. Never throws.
 */
[[nodiscard]] bool isSensitiveVariableName(std::string_view name);

/**
 * Parses NUL-separated environment bytes (the raw form of /proc/<pid>/environ)
 * into sorted, deduplicated entries.
 *
 * Each record is split at the FIRST '=' only, so values may contain '='
 * (e.g. URL=https://example.com?a=b preserves the whole URL). Records without
 * a '=', with an empty variable name, or with a name containing '=' are counted
 * as malformed and skipped; the FIRST occurrence of a name wins and later
 * duplicates are counted and dropped — deterministic, never recomputed from a
 * hash map iteration order. A record not terminated by a trailing NUL is still
 * parsed. Sensitive names are masked immediately. Never throws.
 */
[[nodiscard]] EnvironmentParseResult parseEnvironment(
    std::string_view data,
    std::size_t max_variables = kDefaultMaxEnvironmentVariables);

/**
 * Read-only, security-conscious environment inspector for one process.
 *
 * Reads /proc/<pid>/environ natively (open + bounded read, no strings(1), no
 * `env`, no shell, no subprocess, no root requirement) on the inspect call and
 * only then. The raw environment and every plaintext secret are transient
 * locals, never stored beyond this call, never logged, never persisted, never
 * notified. The result model only ever contains displayed values (masked for
 * sensitive variables).
 */
class ProcessEnvironmentManager {
 public:
  ProcessEnvironmentManager() = default;
  ~ProcessEnvironmentManager() = default;

  // Stateless wrapper object; copy/move are harmless.
  ProcessEnvironmentManager(const ProcessEnvironmentManager &) = default;
  ProcessEnvironmentManager &operator=(const ProcessEnvironmentManager &) =
      default;

  /**
   * Reads `identity.pid`'s environment. The process disappears mid-inspection,
   * permission is denied, the environment is empty or malformed, or the
   * identity changed — each produces a distinct status. The identity gate runs
   * before the read and again after it, so a reused PID's environment is never
   * returned. Byte and variable limits set truncation flags; a partial file is
   * never presented as complete. Reading never throws and never requires root.
   */
  [[nodiscard]] ProcessEnvironmentResult inspect(
      const ProcessIdentity &identity,
      std::size_t max_bytes = kDefaultMaxEnvironmentBytes,
      std::size_t max_variables = kDefaultMaxEnvironmentVariables) const;
};

}  // namespace atm