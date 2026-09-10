#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "process_details.hpp"
#include "process_scheduling.hpp"

namespace atm {

/// Upper bound on the generated report size in bytes. A report that would
/// exceed this is truncated with an explicit marker instead of producing
/// unbounded output for a pathological process.
constexpr std::size_t kDefaultMaxReportBytes = 4U * 1024U * 1024U;  // 4 MiB

/// Upper bound on the number of rows rendered per list section (file
/// descriptors, memory mappings, network connections, namespaces, cgroups,
/// environment variables). Sections with more rows are truncated with an
/// explicit "[...] omitted — truncated" note and the count is preserved.
constexpr std::size_t kDefaultMaxReportRows = 2048;

/// Outcome of one process-report export attempt.
enum class ReportStatus {
  Success,            // written while the process identity was still valid
  SuccessProcessGone, // written, but the process exited during the export; the
                      // report carries an explicit note about incompleteness
  InvalidPid,         // pid <= 0, rejected before any work
  InvalidPath,        // the destination path is empty or not a usable file
  ProcessReused,      // refused: the PID now belongs to a different process,
                      // so no report was written (data would be mislabelled)
  IdentityUnknown,    // refused: the process identity could not be re-read
  WriteError,         // open / write / flush / rename failed (errno_value set)
  EmptyReport,        // no identity data was available, so nothing was written
};

/// Result of one process-report export.
struct ProcessReportResult {
  ReportStatus status = ReportStatus::EmptyReport;
  int errno_value = 0;    // original errno for WriteError / InvalidPath
  std::size_t bytes = 0;  // bytes written to disk
  bool truncated = false; // the report hit a size/row bound while rendering
  std::string path;       // the destination actually written (on Success variants)
};

/// Human-readable description of a ReportStatus.
[[nodiscard]] const char *reportStatusMessage(ReportStatus status);

/**
 * Sanitizes a process name so it can safely appear in a filename.
 *
 * Only ASCII letters, digits, '_' and '-' are kept; every other byte
 * (including '/', '\\', ':', '.', '*', '"', '<', '>', '|', '?', control
 * bytes, multibyte sequences) becomes '_'. Runs of '_' are collapsed and
 * leading/trailing '_' trimmed. The result never contains a path separator,
 * a '.' (so it can never form ".." or hide a file extension), an absolute
 * path prefix or any byte that could escape the destination directory.
 * An empty result falls back to "process". Never throws.
 */
[[nodiscard]] std::string sanitizeReportName(std::string_view raw);

/// Builds the default report filename "process-<pid>-<sanitized-name>.txt".
/// The name is first passed through sanitizeReportName so a hostile process
/// name can never traverse paths; the result is always a plain filename.
[[nodiscard]] std::string defaultReportFilename(pid_t pid,
                                                std::string_view name);

/**
 * Generates a deterministic, UTF-8, human-readable text report from a
 * process snapshot.
 *
 * This function is a pure formatter: it consumes only the models already
 * collected by ProcessDetails and never reads /proc itself. The masking and
 * truncation rules applied by the inspectors are therefore preserved
 * verbatim. Sensitive environment variables are additionally re-masked here
 * (by name, case-insensitively) so a plaintext secret can never appear in
 * generated text even if an upstream model were ever mis-populated. Output is
 * bounded by `max_rows` per list section and `max_bytes` total; overruns are
 * clearly marked, never silent.
 */
[[nodiscard]] std::string generateProcessReport(
    const ProcessDetailsInfo &info,
    std::size_t max_rows = kDefaultMaxReportRows,
    std::size_t max_bytes = kDefaultMaxReportBytes);

/// Whether the PID still refers to the process that was selected earlier.
enum class ReportTargetState {
  Same,        // the running process still has the same start-time ticks
  Disappeared, // /proc/<pid> no longer exists (the process exited)
  Reused,      // the PID now identifies a different process
  Unreadable,  // the identity could not be read (permission or other failure)
};

/// Re-verifies that `selected.pid` still denotes the process captured in
/// `selected` (PID + kernel start-time tick) by reading /proc/<pid>/stat.
/// Never throws.
[[nodiscard]] ReportTargetState
verifyReportTarget(const ProcessIdentity &selected);

/**
 * Performs a complete export: generates the report from `info`, re-verifies
 * that the process is still the one captured by `selected`, and atomically
 * writes the report to `path`.
 *
 * When the process disappeared during the export the report is still written
 * (it contains the data collected while the process was alive) but embeds an
 * explicit "process exited during export" note and the result status is
 * SuccessProcessGone. When the PID was reused or the identity could not be
 * re-read, nothing is written — a following process is never presented as the
 * originally selected one.
 */
[[nodiscard]] ProcessReportResult exportProcessReport(
    const std::string &path, const ProcessDetailsInfo &info,
    const ProcessIdentity &selected,
    std::size_t max_rows = kDefaultMaxReportRows,
    std::size_t max_bytes = kDefaultMaxReportBytes);

/**
 * Atomically writes `contents` to `path`: temporary file in the same
 * directory, write, flush, close, rename. A crash never leaves a partially
 * written report and an existing file is only replaced by the atomic rename
 * (an all-or-nothing replacement). `errno_value` is set on failure. Never
 * throws and never requires root. Does not touch the target process.
 */
[[nodiscard]] ProcessReportResult writeProcessReport(
    const std::string &path, const std::string &contents);

}  // namespace atm