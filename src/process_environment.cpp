#include "process_environment.hpp"

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace atm {

namespace {

/// Reads the environment file with a hard byte bound and detects when the
/// snapshot was cut short. `size_truncated` becomes true when more data existed
/// after the last buffered byte. Retries a single EINTR open/read. Returns
/// std::nullopt on failure with the errno in `err_out`.
std::optional<std::string> readBoundedFile(const std::string &path,
                                           std::size_t max_bytes,
                                           bool *size_truncated,
                                           int *err_out) {
  int fd = -1;
  for (int attempt = 0; attempt < 2; ++attempt) {
    fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
      break;
    }
    if (errno != EINTR || attempt == 1) {
      if (err_out != nullptr) {
        *err_out = errno;
      }
      return std::nullopt;
    }
  }
  if (fd < 0) {
    return std::nullopt;
  }

  std::string contents;
  contents.reserve(max_bytes);
  bool truncated = false;
  while (contents.size() < max_bytes) {
    char buffer[4096];
    const std::size_t want =
        std::min(sizeof(buffer), max_bytes - contents.size());
    const ssize_t n = ::read(fd, buffer, want);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      const int err = errno;
      ::close(fd);
      if (err_out != nullptr) {
        *err_out = err;
      }
      return std::nullopt;
    }
    if (n == 0) {
      break;  // end of the kernel snapshot
    }
    contents.append(buffer, static_cast<std::size_t>(n));
    if (static_cast<std::size_t>(n) < want) {
      continue;  // 4096-boundary partial read: loop to fetch the rest
    }
    if (contents.size() == max_bytes) {
      // Probe whether the file holds more than we read.
      char probe = 0;
      const ssize_t m = ::read(fd, &probe, 1);
      if (m > 0) {
        truncated = true;
      }
      break;
    }
  }
  ::close(fd);
  if (err_out != nullptr) {
    *err_out = 0;
  }
  if (size_truncated != nullptr) {
    *size_truncated = truncated;
  }
  return contents;
}

}  // namespace

bool isSensitiveVariableName(std::string_view name) {
  // Substring signals, compared case-insensitively. See the header for the
  // documented semantics (conservative heuristic, straightforward to extend).
  static constexpr std::string_view kSensitiveSubstrings[] = {
      "PASSWORD", "PASSWD",     "SECRET",    "TOKEN",   "API_KEY",
      "APIKEY",   "ACCESS_KEY", "PRIVATE_KEY", "CREDENTIAL", "AUTH",
      "BEARER",   "SESSION",    "COOKIE",
  };

  std::string upper;
  upper.reserve(name.size());
  for (const char c : name) {
    upper.push_back(
        static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  for (const std::string_view signal : kSensitiveSubstrings) {
    if (upper.find(signal) != std::string::npos) {
      return true;
    }
  }
  return false;
}

EnvironmentParseResult parseEnvironment(std::string_view data,
                                        std::size_t max_variables) {
  EnvironmentParseResult result;
  std::set<std::string> seen;
  std::size_t pos = 0;
  const std::size_t n = data.size();
  while (pos < n) {
    const std::size_t nul = data.find('\0', pos);
    const std::string_view record =
        nul == std::string_view::npos ? data.substr(pos) : data.substr(pos, nul - pos);
    pos = nul == std::string_view::npos ? n : nul + 1;
    if (record.empty()) {
      continue;  // trailing NUL / empty record: not a variable
    }
    ++result.record_count;

    const std::size_t equals = record.find('=');
    if (equals == std::string_view::npos || equals == 0) {
      ++result.malformed_count;  // no '=' or empty variable name
      continue;
    }
    const std::string_view name_view = record.substr(0, equals);
    const std::string_view value_view = record.substr(equals + 1);
    if (seen.count(std::string(name_view)) != 0) {
      ++result.duplicate_count;  // first occurrence already kept
      continue;
    }
    if (result.entries.size() >= max_variables) {
      result.variable_truncated = true;
      continue;  // keep counting records, stop storing entries
    }

    seen.insert(std::string(name_view));
    ProcessEnvironmentEntry entry;
    entry.name = std::string(name_view);
    entry.sensitive = isSensitiveVariableName(entry.name);
    if (entry.sensitive) {
      // The plaintext secret is never copied into the model.
      entry.value = std::string(kMaskedSecretPlaceholder);
    } else {
      entry.value = std::string(value_view);
    }
    result.entries.push_back(std::move(entry));
  }

  std::sort(result.entries.begin(), result.entries.end(),
            [](const ProcessEnvironmentEntry &a,
               const ProcessEnvironmentEntry &b) {
              return a.name < b.name;
            });
  return result;
}

ProcessEnvironmentResult ProcessEnvironmentManager::inspect(
    const ProcessIdentity &identity, std::size_t max_bytes,
    std::size_t max_variables) const {
  if (identity.pid <= 0) {
    return {EnvironmentStatus::InvalidPid, 0, {}, 0, 0, 0, 0, false, false};
  }

  // Identity gate (before the read).
  std::optional<ProcessIdentity> current =
      ProcessIdentity::current(identity.pid);
  if (!current) {
    return {EnvironmentStatus::IdentityUnknown, 0, {}, 0, 0, 0, 0, false, false};
  }
  if (current->starttime_ticks != identity.starttime_ticks) {
    return {EnvironmentStatus::ProcessReused, 0, {}, 0, 0, 0, 0, false, false};
  }

  const std::string environ_path =
      "/proc/" + std::to_string(identity.pid) + "/environ";
  bool size_truncated = false;
  int file_errno = 0;
  const std::optional<std::string> contents =
      readBoundedFile(environ_path, max_bytes, &size_truncated, &file_errno);
  if (!contents) {
    const int err = file_errno;
    if (err == EACCES || err == EPERM) {
      return {EnvironmentStatus::PermissionDenied, err, {}, 0, 0, 0, 0, false,
              false};
    }
    if (err == ENOENT || err == ESRCH) {
      return {EnvironmentStatus::ProcessNotFound, err, {}, 0, 0, 0, 0, false,
              false};
    }
    return {EnvironmentStatus::ReadError, err, {}, 0, 0, 0, 0, false, false};
  }

  ProcessEnvironmentResult result;
  result.byte_count = contents->size();
  result.size_truncated = size_truncated;

  const EnvironmentParseResult parsed =
      parseEnvironment(*contents, max_variables);
  result.entries = std::move(parsed.entries);
  result.duplicate_count = parsed.duplicate_count;
  result.malformed_count = parsed.malformed_count;
  result.variable_truncated = parsed.variable_truncated;
  for (const ProcessEnvironmentEntry &entry : result.entries) {
    if (entry.sensitive) {
      ++result.sensitive_count;
    }
  }

  if (contents->empty()) {
    result.status = EnvironmentStatus::EmptyEnvironment;
  } else if (result.entries.empty()) {
    result.status = EnvironmentStatus::MalformedData;
  } else {
    result.status = EnvironmentStatus::Success;
  }

  // Identity gate (after the read): discard a stale result if the PID was
  // reused while we were inspecting.
  current = ProcessIdentity::current(identity.pid);
  if (!current) {
    return {EnvironmentStatus::IdentityUnknown, 0, {}, 0, 0, 0, 0, false, false};
  }
  if (current->starttime_ticks != identity.starttime_ticks) {
    return {EnvironmentStatus::ProcessReused, 0, {}, 0, 0, 0, 0, false, false};
  }
  return result;
}

}  // namespace atm