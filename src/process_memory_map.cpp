#include "process_memory_map.hpp"

#include <sys/stat.h>

#include <cerrno>
#include <charconv>
#include <cctype>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace atm {

namespace {

/// Strictly parses a whole token as an unsigned 64-bit integer in `base`.
/// Full token consumption is required (no trailing garbage, no 0x prefix).
std::optional<std::uint64_t> parseUInt(std::string_view view, int base) {
  if (view.empty()) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  const char *begin = view.data();
  const char *end = view.data() + view.size();
  const auto result = std::from_chars(begin, end, value, base);
  if (result.ec != std::errc{} || result.ptr != end) {
    return std::nullopt;
  }
  return value;
}

/// The five mandatory, whitespace-separated leading fields of a maps line:
/// address-range, permissions, offset, device, inode. `rest` receives the
/// optional pathname (everything remaining, interior spaces preserved).
bool splitMapFields(std::string_view line, std::string_view &address_range,
                    std::string_view &permissions, std::string_view &offset,
                    std::string_view &device, std::string_view &inode,
                    std::string_view &rest) {
  const auto next_field = [&line](std::string_view &field) {
    // Skip separating whitespace.
    std::size_t first = 0;
    while (first < line.size() &&
           std::isspace(static_cast<unsigned char>(line[first]))) {
      ++first;
    }
    if (first >= line.size()) {
      return false;
    }
    std::size_t last = first;
    while (last < line.size() &&
           !std::isspace(static_cast<unsigned char>(line[last]))) {
      ++last;
    }
    field = line.substr(first, last - first);
    line = line.substr(last);
    return true;
  };

  if (!next_field(address_range) || !next_field(permissions) ||
      !next_field(offset) || !next_field(device) || !next_field(inode)) {
    return false;
  }
  // Everything that follows (trimmed) is the optional mapping name.
  std::size_t first = 0;
  while (first < line.size() &&
         std::isspace(static_cast<unsigned char>(line[first]))) {
    ++first;
  }
  rest = line.substr(first);
  return true;
}

}  // namespace

std::optional<ProcessMemoryMap> parseMemoryMapLine(std::string_view line) {
  std::string_view address_range;
  std::string_view permissions;
  std::string_view offset_text;
  std::string_view device_text;
  std::string_view inode_text;
  std::string_view pathname;
  if (!splitMapFields(line, address_range, permissions, offset_text,
                      device_text, inode_text, pathname)) {
    return std::nullopt;  // too few fields: malformed line
  }

  // Address range "start-end", both hexadecimal.
  const std::size_t dash = address_range.find('-');
  if (dash == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view start_text = address_range.substr(0, dash);
  const std::string_view end_text = address_range.substr(dash + 1);
  const std::optional<std::uint64_t> start = parseUInt(start_text, 16);
  const std::optional<std::uint64_t> end = parseUInt(end_text, 16);
  // end > start only: an empty or reversed range is rejected, and the
  // subtraction in size() can never underflow because it is guarded above.
  if (!start || !end || *end <= *start) {
    return std::nullopt;
  }

  // Offset (hex), device "major:minor" (hex), inode (decimal) are mandatory
  // kernel fields; a malformed one marks the whole line as corrupt.
  const std::optional<std::uint64_t> offset = parseUInt(offset_text, 16);
  if (!offset) {
    return std::nullopt;
  }
  std::uint32_t device_major = 0;
  std::uint32_t device_minor = 0;
  const std::size_t colon = device_text.find(':');
  if (colon == std::string_view::npos) {
    return std::nullopt;
  }
  const std::optional<std::uint64_t> major =
      parseUInt(device_text.substr(0, colon), 16);
  const std::optional<std::uint64_t> minor =
      parseUInt(device_text.substr(colon + 1), 16);
  if (!major || !minor) {
    return std::nullopt;
  }
  device_major = static_cast<std::uint32_t>(*major);
  device_minor = static_cast<std::uint32_t>(*minor);
  const std::optional<std::uint64_t> inode = parseUInt(inode_text, 10);
  if (!inode) {
    return std::nullopt;
  }

  ProcessMemoryMap mapping;
  mapping.start = *start;
  mapping.end = *end;
  mapping.offset = *offset;
  mapping.device_major = device_major;
  mapping.device_minor = device_minor;
  mapping.inode = inode.value_or(0);
  mapping.permissions = std::string(permissions);
  mapping.pathname = std::string(pathname);
  for (const char c : permissions) {
    if (c == 'r') mapping.readable = true;
    if (c == 'w') mapping.writable = true;
    if (c == 'x') mapping.executable = true;
    if (c == 's') mapping.shared = true;
    if (c == 'p') mapping.shared = false;
  }
  mapping.category = classifyMemoryMap(mapping);
  return mapping;
}

MemoryMapCategory classifyMemoryMap(const ProcessMemoryMap &mapping) {
  if (mapping.pathname == "[heap]") {
    return MemoryMapCategory::Heap;
  }
  if (mapping.pathname == "[stack]") {
    return MemoryMapCategory::Stack;
  }
  if (mapping.pathname == "[vdso]") {
    return MemoryMapCategory::Vdso;
  }
  if (mapping.pathname == "[vvar]") {
    return MemoryMapCategory::Vvar;
  }
  if (mapping.pathname == "[vsyscall]") {
    return MemoryMapCategory::Vsyscall;
  }
  const bool file_backed =
      !mapping.pathname.empty() && mapping.pathname.front() == '/';
  if (file_backed) {
    const bool looks_library =
        mapping.pathname.find("/lib") != std::string::npos ||
        (mapping.pathname.size() > 3 &&
         mapping.pathname.compare(mapping.pathname.size() - 3, 3, ".so") == 0);
    if (mapping.executable) {
      return looks_library ? MemoryMapCategory::SharedLibrary
                           : MemoryMapCategory::Executable;
    }
    return looks_library ? MemoryMapCategory::SharedLibrary
                         : MemoryMapCategory::FileBacked;
  }
  return mapping.executable ? MemoryMapCategory::Executable
                            : MemoryMapCategory::Anonymous;
}

std::vector<ProcessMemoryMap> parseMemoryMaps(std::string_view contents,
                                              std::size_t max_mappings,
                                              bool &truncated) {
  truncated = false;
  std::vector<ProcessMemoryMap> result;
  std::size_t cursor = 0;
  while (cursor < contents.size()) {
    const std::size_t newline = contents.find('\n', cursor);
    std::string_view line =
        contents.substr(cursor,
                        newline == std::string_view::npos
                            ? std::string_view::npos
                            : newline - cursor);
    cursor = newline == std::string_view::npos ? contents.size() : newline + 1;
    // Tolerate a trailing carriage return (Windows-style input).
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (const std::optional<ProcessMemoryMap> mapping =
            parseMemoryMapLine(line)) {
      if (result.size() >= max_mappings) {
        truncated = true;
        break;
      }
      result.push_back(*mapping);
    }
  }
  return result;
}

ProcessMemoryMapsResult
ProcessMemoryMapManager::inspect(const ProcessIdentity &identity,
                                 std::size_t max_mappings) const {
  if (identity.pid <= 0) {
    return {MemoryMapStatus::InvalidPid, 0, {}, false};
  }

  // Identity gate (before the read): the mapping list must belong to the exact
  // process the caller selected, never to a reused PID.
  std::optional<ProcessIdentity> current = ProcessIdentity::current(identity.pid);
  if (!current) {
    return {MemoryMapStatus::IdentityUnknown, 0, {}, false};
  }
  if (current->starttime_ticks != identity.starttime_ticks) {
    return {MemoryMapStatus::ProcessReused, 0, {}, false};
  }

  const std::string path = "/proc/" + std::to_string(identity.pid) + "/maps";
  errno = 0;
  std::ifstream in(path, std::ios::binary);
  const int open_errno = errno;
  if (!in.is_open()) {
    // Disambiguate EACCES/EPERM from "process is gone": a vanished process
    // makes stat() of /proc/<pid>/maps fail with ENOENT.
    struct ::stat entry {};
    if (::stat(path.c_str(), &entry) != 0) {
      if (errno == EACCES || errno == EPERM) {
        return {MemoryMapStatus::PermissionDenied, errno, {}, false};
      }
      return {MemoryMapStatus::ProcessNotFound, errno, {}, false};
    }
    if (open_errno == EACCES || open_errno == EPERM) {
      return {MemoryMapStatus::PermissionDenied, open_errno, {}, false};
    }
    return {MemoryMapStatus::ReadError, open_errno, {}, false};
  }

  std::ostringstream contents;
  contents << in.rdbuf();
  if (in.bad()) {
    return {MemoryMapStatus::ReadError, errno, {}, false};
  }

  // Identity gate (after the read): if the process was replaced while we were
  // reading, discard the stale mappings instead of returning them.
  current = ProcessIdentity::current(identity.pid);
  if (!current) {
    return {MemoryMapStatus::IdentityUnknown, 0, {}, false};
  }
  if (current->starttime_ticks != identity.starttime_ticks) {
    return {MemoryMapStatus::ProcessReused, 0, {}, false};
  }

  ProcessMemoryMapsResult result;
  result.maps = parseMemoryMaps(contents.str(), max_mappings, result.truncated);
  result.status = MemoryMapStatus::Success;

  for (const ProcessMemoryMap &mapping : result.maps) {
    const std::uint64_t size = mapping.size();
    if (result.total_bytes > std::numeric_limits<std::uint64_t>::max() - size) {
      result.total_bytes = std::numeric_limits<std::uint64_t>::max();
    } else {
      result.total_bytes += size;
    }
    if (mapping.executable) {
      ++result.executable_count;
    }
    if (mapping.writable) {
      ++result.writable_count;
    }
    if (!mapping.pathname.empty() && mapping.pathname.front() == '/') {
      ++result.file_backed_count;
    }
    if (mapping.category == MemoryMapCategory::Anonymous ||
        mapping.category == MemoryMapCategory::Executable ||
        mapping.category == MemoryMapCategory::Heap ||
        mapping.category == MemoryMapCategory::Stack) {
      ++result.anonymous_count;
    }
  }
  return result;
}

}  // namespace atm