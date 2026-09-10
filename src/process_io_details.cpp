#include "process_io_details.hpp"

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>

namespace atm {

namespace {

/// Reads a bounded file content. Returns std::nullopt on failure with errno
/// in `err_out`. Retries once on EINTR.
std::optional<std::string> readBoundedFile(const std::string &path,
                                           std::size_t max_bytes,
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
  contents.reserve(std::min(max_bytes, std::size_t{4096}));
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
      break;
    }
    contents.append(buffer, static_cast<std::size_t>(n));
  }
  ::close(fd);
  if (err_out != nullptr) {
    *err_out = 0;
  }
  return contents;
}

}  // namespace

const char *ioDetailsStatusMessage(IoDetailsStatus status) {
  switch (status) {
    case IoDetailsStatus::Success:
      return "I/O details loaded successfully.";
    case IoDetailsStatus::InvalidPid:
      return "Invalid PID.";
    case IoDetailsStatus::ProcessNotFound:
      return "Process no longer exists.";
    case IoDetailsStatus::IdentityUnknown:
      return "Process identity could not be determined.";
    case IoDetailsStatus::ProcessReused:
      return "The PID was reused; I/O data was discarded.";
    case IoDetailsStatus::PermissionDenied:
      return "Permission denied.";
    case IoDetailsStatus::ReadError:
      return "Failed to read I/O information.";
  }
  return "Unknown error.";
}

ProcessIoDetailsInfo parseIoDetails(std::string_view contents) {
  ProcessIoDetailsInfo info;
  std::istringstream lines{std::string(contents)};
  std::string line;

  while (std::getline(lines, line)) {
    std::istringstream head(line);
    std::string key;
    if (!(head >> key)) {
      continue;
    }

    // Read the value token as a string first to reject negative numbers before
    // parsing as uint64_t (>> uint64_t silently wraps negatives).
    std::string token;
    if (!(head >> token)) {
      continue;
    }
    if (!token.empty() && token[0] == '-') {
      continue;  // I/O counters are non-negative
    }
    std::uint64_t value = 0;
    const char *begin = token.data();
    const char *end = token.data() + token.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr == begin) {
      continue;  // malformed or empty
    }

    if (key == "rchar:") {
      info.chars_read = value;
    } else if (key == "wchar:") {
      info.chars_written = value;
    } else if (key == "syscr:") {
      info.read_syscalls = value;
    } else if (key == "syscw:") {
      info.write_syscalls = value;
    } else if (key == "read_bytes:") {
      info.bytes_read = value;
    } else if (key == "write_bytes:") {
      info.bytes_written = value;
    } else if (key == "cancelled_write_bytes:") {
      info.cancelled_write_bytes = value;
    }
  }

  return info;
}

ProcessIoDetailsResult
ProcessIoDetailsManager::inspect(const ProcessIdentity &identity) const {
  if (identity.pid <= 0) {
    return {IoDetailsStatus::InvalidPid, 0, {}};
  }

  // Identity gate (before the read).
  std::optional<ProcessIdentity> current =
      ProcessIdentity::current(identity.pid);
  if (!current) {
    return {IoDetailsStatus::IdentityUnknown, 0, {}};
  }
  if (current->starttime_ticks != identity.starttime_ticks) {
    return {IoDetailsStatus::ProcessReused, 0, {}};
  }

  const std::string path =
      "/proc/" + std::to_string(identity.pid) + "/io";

  int io_errno = 0;
  const std::optional<std::string> content =
      readBoundedFile(path, kDefaultMaxIoBytes, &io_errno);
  if (!content) {
    if (io_errno == EACCES || io_errno == EPERM) {
      return {IoDetailsStatus::PermissionDenied, io_errno, {}};
    }
    if (io_errno == ENOENT || io_errno == ESRCH) {
      return {IoDetailsStatus::ProcessNotFound, io_errno, {}};
    }
    return {IoDetailsStatus::ReadError, io_errno, {}};
  }

  ProcessIoDetailsInfo info = parseIoDetails(*content);

  // Identity gate (after the read): discard a stale result if the PID was
  // reused while we were inspecting.
  current = ProcessIdentity::current(identity.pid);
  if (!current) {
    return {IoDetailsStatus::IdentityUnknown, 0, {}};
  }
  if (current->starttime_ticks != identity.starttime_ticks) {
    return {IoDetailsStatus::ProcessReused, 0, {}};
  }

  return {IoDetailsStatus::Success, 0, std::move(info)};
}

}  // namespace atm
