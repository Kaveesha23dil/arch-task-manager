#include "process_cgroup.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace atm {

namespace {

std::optional<std::uint64_t> parseUInt(std::string_view view) {
  if (view.empty()) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  const char *begin = view.data();
  const char *end = view.data() + view.size();
  const auto result = std::from_chars(begin, end, value, 10);
  if (result.ec != std::errc{} || result.ptr != end) {
    return std::nullopt;
  }
  return value;
}

std::string_view trimWhitespace(std::string_view view) {
  const std::size_t begin = view.find_first_not_of(" \t\r\n");
  if (begin == std::string_view::npos) {
    return {};
  }
  const std::size_t end = view.find_last_not_of(" \t\r\n");
  return view.substr(begin, end - begin + 1);
}

bool isOctalDigit(char c) {
  return c >= '0' && c <= '7';
}

/// Decodes the octal escape sequences used in /proc paths and /proc/self/
/// mountinfo for special bytes (\040 = space, \072 = ':', \134 = backslash,
/// ...). A '\' not followed by exactly three octal digits is kept verbatim.
std::string decodeOctalEscapes(std::string_view token) {
  std::string out;
  out.reserve(token.size());
  const std::size_t n = token.size();
  for (std::size_t i = 0; i < n;) {
    if (token[i] == '\\' && i + 3 < n && isOctalDigit(token[i + 1]) &&
        isOctalDigit(token[i + 2]) && isOctalDigit(token[i + 3])) {
      out.push_back(static_cast<char>((token[i + 1] - '0') * 64 +
                                      (token[i + 2] - '0') * 8 +
                                      (token[i + 3] - '0')));
      i += 4;
    } else {
      out.push_back(token[i]);
      ++i;
    }
  }
  return out;
}

/// Reads a whole file (the kernel control/metadata files here are small).
/// Returns std::nullopt on failure with the errno in `err_out`. A single
/// bounded retry is performed when the open was interrupted (EINTR).
std::optional<std::string> readFileContents(const std::string &path,
                                            int *err_out) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    std::ifstream in(path);
    if (!in.is_open()) {
      const int err = errno;
      if (err == EINTR && attempt == 0) {
        continue;
      }
      if (err_out != nullptr) {
        *err_out = err;
      }
      return std::nullopt;
    }
    std::string contents;
    std::string chunk;
    while (std::getline(in, chunk)) {
      contents += chunk;
      contents.push_back('\n');
    }
    if (!in.eof() && in.fail()) {
      if (err_out != nullptr) {
        *err_out = EIO;
      }
      return std::nullopt;
    }
    if (err_out != nullptr) {
      *err_out = 0;
    }
    return contents;
  }
  if (err_out != nullptr) {
    *err_out = EINTR;
  }
  return std::nullopt;
}

/// One cgroup filesystem mount parsed from /proc/self/mountinfo.
struct CgroupMount {
  std::string root;        // mount root, octal escapes decoded
  std::string mount_point; // mount point, octal escapes decoded
  std::string fs_type;     // "cgroup" (v1) or "cgroup2" (v2)
  std::string options;     // super options; for v1 the controller set
};

/// Parses /proc/self/mountinfo lines. The format is
/// "id parent maj:min root mountpoint options [optional...] - fstype source
///  superopts"; the "superopts" field is joined verbatim because it may be
/// empty (whitespace splitting would drop it). Debris lines are skipped.
/// Never throws.
std::vector<CgroupMount> parseMountInfo(std::string_view contents) {
  std::vector<CgroupMount> mounts;
  std::size_t line_start = 0;
  while (line_start < contents.size()) {
    const std::size_t line_end = contents.find('\n', line_start);
    const std::string_view line =
        line_end == std::string_view::npos
            ? contents.substr(line_start)
            : contents.substr(line_start, line_end - line_start);
    line_start = line_end == std::string_view::npos
                     ? contents.size()
                     : line_end + 1;

    std::vector<std::string_view> fields;
    {
      std::size_t pos = 0;
      while (pos < line.size()) {
        while (pos < line.size() &&
               (line[pos] == ' ' || line[pos] == '\t')) {
          ++pos;
        }
        if (pos >= line.size()) {
          break;
        }
        const std::size_t start = pos;
        while (pos < line.size() && line[pos] != ' ' && line[pos] != '\t') {
          ++pos;
        }
        fields.push_back(line.substr(start, pos - start));
      }
    }

    // Token separators: id(0) parent(1) maj:min(2) root(3) mountpoint(4)
    // options(5) [optional fields] - fstype source superopts.
    const auto separator = std::find(fields.begin(), fields.end(), "-");
    if (separator == fields.end()) {
      continue;
    }
    const std::ptrdiff_t sep_index = separator - fields.begin();
    if (sep_index < 6 || sep_index + 2 >= static_cast<std::ptrdiff_t>(fields.size())) {
      continue;
    }
    CgroupMount mount;
    mount.root = decodeOctalEscapes(fields[3]);
    mount.mount_point = decodeOctalEscapes(fields[4]);
    mount.fs_type = std::string(fields[sep_index + 1]);
    std::string super_options;
    for (std::size_t i = static_cast<std::size_t>(sep_index) + 3;
         i < fields.size(); ++i) {
      if (!super_options.empty()) {
        super_options.push_back(' ');
      }
      super_options.append(fields[i]);
    }
    mount.options = super_options;
    if (mount.fs_type == "cgroup" || mount.fs_type == "cgroup2") {
      mounts.push_back(std::move(mount));
    }
  }
  return mounts;
}

/// The controller tokens of a v1 cgroup mount's super options.
std::set<std::string> optionTokens(const CgroupMount &mount) {
  std::set<std::string> tokens;
  std::size_t pos = 0;
  while (pos < mount.options.size()) {
    const std::size_t comma = mount.options.find_first_of(", \t", pos);
    const std::string token = mount.options.substr(
        pos, comma == std::string::npos ? std::string::npos : comma - pos);
    if (!token.empty()) {
      tokens.insert(token);
    }
    pos = comma == std::string::npos ? mount.options.size() : comma + 1;
  }
  return tokens;
}

/// Picks the v1 mount that covers `controllers` with the fewest extra
/// controllers (most specific match); std::nullopt when no mount covers them.
const CgroupMount *bestV1Mount(const std::vector<CgroupMount> &mounts,
                               const std::vector<std::string> &controllers) {
  if (controllers.empty()) {
    return nullptr;
  }
  const CgroupMount *best = nullptr;
  std::size_t best_extra = 0;
  for (const CgroupMount &mount : mounts) {
    if (mount.fs_type != "cgroup") {
      continue;
    }
    const std::set<std::string> tokens = optionTokens(mount);
    std::size_t covered = 0;
    for (const std::string &controller : controllers) {
      if (tokens.count(controller) != 0) {
        ++covered;
      }
    }
    if (covered != controllers.size()) {
      continue;
    }
    if (best == nullptr || tokens.size() - controllers.size() < best_extra) {
      best = &mount;
      best_extra = tokens.size() - controllers.size();
    }
  }
  return best;
}

/// Joins the cgroup mount point, the mount root and the hierarchy's relative
/// path into one absolute path. Every component is sanity-checked: "..", ".",
/// and empty components are rejected (the relative path is untrusted metadata
/// read from /proc/<pid>/cgroup and must never escape the cgroup mount). With
/// every component validated the result is guaranteed to stay inside the mount
/// point. Returns std::nullopt when the path cannot be resolved safely.
std::optional<std::string> safeAbsolutePath(
    const std::string &mount_point, const std::string &mount_root,
    const std::string &relative_path) {
  if (mount_point.empty()) {
    return std::nullopt;
  }
  std::string out;
  out.reserve(mount_point.size() + mount_root.size() +
              relative_path.size() + 8);
  out = mount_point;

  auto appendComponent = [&out](std::string_view component) {
    if (component.empty() || component == "." || component == "..") {
      return false;
    }
    if (out.empty() || out.back() != '/') {
      out.push_back('/');
    }
    out.append(component);
    return true;
  };

  auto appendComponents = [&appendComponent](std::string_view path) {
    std::size_t pos = 0;
    while (pos < path.size()) {
      const std::size_t slash = path.find('/', pos);
      const std::string_view component =
          slash == std::string_view::npos
              ? path.substr(pos)
              : path.substr(pos, slash - pos);
      if (component.empty()) {
        // Empty components (leading '/', repeated '/') are harmless separators
        // and are skipped; ".", ".." are still rejected by appendComponent.
        pos = slash == std::string_view::npos ? path.size() : slash + 1;
        continue;
      }
      if (!appendComponent(component)) {
        return false;
      }
      pos = slash == std::string_view::npos ? path.size() : slash + 1;
    }
    return true;
  };

  if (!mount_root.empty() && mount_root != "/") {
    if (!appendComponents(mount_root)) {
      return std::nullopt;
    }
  }
  if (!appendComponents(relative_path)) {
    return std::nullopt;
  }
  return out;
}

/// Reads+parses one control file value (or cpu.max) into the resources struct.
/// Returns true when the file was readable and produced a value.
template <typename ValueType>
bool readResourceFile(const std::string &directory,
                      const std::string &file_name,
                      ValueType &target,
                      std::optional<ValueType> (*parser)(std::string_view)) {
  int file_errno = 0;
  const std::optional<std::string> contents =
      readFileContents(directory + "/" + file_name, &file_errno);
  if (!contents) {
    return false;
  }
  const std::optional<ValueType> parsed = parser(*contents);
  if (!parsed) {
    return false;
  }
  target = *parsed;
  return true;
}

/// Splits cgroup.controllers content ("memory pids") into its names.
std::vector<std::string> splitControllerNames(std::string_view contents) {
  std::vector<std::string> names;
  std::size_t pos = 0;
  while (pos < contents.size()) {
    const std::size_t space = contents.find_first_of(" \t\n", pos);
    const std::string_view token =
        space == std::string_view::npos
            ? contents.substr(pos)
            : contents.substr(pos, space - pos);
    if (!token.empty()) {
      names.emplace_back(token);
    }
    pos = space == std::string_view::npos ? contents.size() : space + 1;
  }
  return names;
}

/// Discovers the mounted cgroup filesystems and the resulting version.
struct MountDiscovery {
  std::vector<CgroupMount> mounts;
  CgroupVersion version = CgroupVersion::Unknown;
};

MountDiscovery discoverCgroupMounts() {
  MountDiscovery discovery;
  int file_errno = 0;
  const std::optional<std::string> contents =
      readFileContents("/proc/self/mountinfo", &file_errno);
  if (!contents) {
    return discovery;
  }
  discovery.mounts = parseMountInfo(*contents);
  bool has_v2 = false;
  bool has_v1 = false;
  for (const CgroupMount &mount : discovery.mounts) {
    if (mount.fs_type == "cgroup2") {
      has_v2 = true;
    } else if (mount.fs_type == "cgroup") {
      has_v1 = true;
    }
  }
  if (has_v2) {
    discovery.version = CgroupVersion::V2;
  } else if (has_v1) {
    discovery.version = CgroupVersion::V1;
  }
  return discovery;
}

}  // namespace

const char *cgroupVersionName(CgroupVersion version) {
  switch (version) {
    case CgroupVersion::V1:
      return "cgroup v1";
    case CgroupVersion::V2:
      return "cgroup v2";
    case CgroupVersion::Unknown:
      return "unknown";
  }
  return "unknown";
}

std::optional<ProcessCgroupHierarchy> parseCgroupLine(
    std::string_view line) {
  // A single line: embedded newlines/carriage returns mark malformed input.
  if (line.find_first_of("\n\r") != std::string_view::npos) {
    return std::nullopt;
  }
  const std::size_t first = line.find(':');
  if (first == std::string_view::npos || first == line.size() - 1) {
    return std::nullopt;
  }
  const std::size_t second = line.find(':', first + 1);
  if (second == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view id_view = line.substr(0, first);
  const std::string_view controllers_view =
      line.substr(first + 1, second - first - 1);
  const std::string_view path_view = line.substr(second + 1);

  const std::optional<std::uint64_t> id = parseUInt(id_view);
  if (!id) {
    return std::nullopt;
  }
  if (path_view.empty() || path_view[0] != '/') {
    return std::nullopt;
  }
  // The kernel escapes tab/newline bytes (octal) in /proc paths; a raw control
  // character here indicates malformed data, not a legitimate path.
  if (path_view.find_first_of("\t\n\r") != std::string_view::npos) {
    return std::nullopt;
  }

  ProcessCgroupHierarchy hierarchy;
  hierarchy.hierarchy_id = *id;
  if (!controllers_view.empty()) {
    std::size_t start = 0;
    while (true) {
      const std::size_t comma = controllers_view.find(',', start);
      const std::string_view token =
          comma == std::string_view::npos
              ? controllers_view.substr(start)
              : controllers_view.substr(start, comma - start);
      if (token.empty()) {
        return std::nullopt;  // ",," or trailing comma: malformed
      }
      hierarchy.controllers.emplace_back(token);
      if (comma == std::string_view::npos) {
        break;
      }
      start = comma + 1;
    }
  }
  hierarchy.relative_path = decodeOctalEscapes(path_view);
  return hierarchy;
}

std::optional<CgroupValue> parseCgroupValue(std::string_view contents) {
  const std::string_view trimmed = trimWhitespace(contents);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  CgroupValue value;
  if (trimmed == "max") {
    value.available = true;
    value.unlimited = true;
    return value;
  }
  const std::optional<std::uint64_t> number = parseUInt(trimmed);
  if (!number) {
    return std::nullopt;
  }
  value.available = true;
  value.value = *number;
  return value;
}

std::optional<CgroupCpuMax> parseCgroupCpuMax(std::string_view contents) {
  const std::string_view trimmed = trimWhitespace(contents);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  std::vector<std::string_view> tokens;
  std::size_t pos = 0;
  while (pos < trimmed.size()) {
    const std::size_t space = trimmed.find_first_of(" \t", pos);
    const std::string_view token =
        space == std::string_view::npos
            ? trimmed.substr(pos)
            : trimmed.substr(pos, space - pos);
    if (!token.empty()) {
      tokens.push_back(token);
    }
    pos = space == std::string_view::npos ? trimmed.size() : space + 1;
  }
  if (tokens.empty() || tokens.size() > 2) {
    return std::nullopt;
  }
  if (tokens.size() == 1 && tokens[0] != "max") {
    return std::nullopt;  // a finite quota alone (missing period) is malformed
  }
  CgroupCpuMax value;
  value.available = true;
  if (tokens[0] == "max") {
    value.unlimited = true;
  } else {
    const std::optional<std::uint64_t> quota = parseUInt(tokens[0]);
    if (!quota) {
      return std::nullopt;
    }
    value.unlimited = false;
    value.quota_usec = *quota;
  }
  if (tokens.size() == 2) {
    const std::optional<std::uint64_t> period = parseUInt(tokens[1]);
    if (!period || *period == 0) {
      return std::nullopt;
    }
    value.period_usec = *period;
  }
  return value;
}

ProcessCgroupResult ProcessCgroupManager::inspect(
    const ProcessIdentity &identity, std::size_t max_hierarchies) const {
  if (identity.pid <= 0) {
    return {CgroupStatus::InvalidPid, 0, {}, false, CgroupVersion::Unknown, {}};
  }

  // Identity gate (before the read).
  std::optional<ProcessIdentity> current =
      ProcessIdentity::current(identity.pid);
  if (!current) {
    return {CgroupStatus::IdentityUnknown, 0, {}, false, CgroupVersion::Unknown,
            {}};
  }
  if (current->starttime_ticks != identity.starttime_ticks) {
    return {CgroupStatus::ProcessReused, 0, {}, false, CgroupVersion::Unknown,
            {}};
  }

  int file_errno = 0;
  const std::string cgroup_path =
      "/proc/" + std::to_string(identity.pid) + "/cgroup";
  const std::optional<std::string> members =
      readFileContents(cgroup_path, &file_errno);
  if (!members) {
    const int err = file_errno;
    if (err == EACCES || err == EPERM) {
      return {CgroupStatus::PermissionDenied, err, {}, false,
              CgroupVersion::Unknown, {}};
    }
    if (err == ENOENT || err == ESRCH) {
      return {CgroupStatus::ProcessNotFound, err, {}, false,
              CgroupVersion::Unknown, {}};
    }
    return {CgroupStatus::ReadError, err, {}, false, CgroupVersion::Unknown,
            {}};
  }

  // Parse the membership records. Malformed lines are tolerated alongside
  // valid ones; a file with no valid line at all is reported as malformed.
  ProcessCgroupResult result;
  result.status = CgroupStatus::Success;
  std::string_view remaining(*members);
  while (!remaining.empty()) {
    const std::size_t newline = remaining.find('\n');
    const std::string_view line =
        newline == std::string_view::npos
            ? remaining
            : remaining.substr(0, newline);
    remaining = newline == std::string_view::npos
                    ? std::string_view{}
                    : remaining.substr(newline + 1);
    if (line.empty()) {
      continue;
    }
    if (result.hierarchies.size() >= max_hierarchies) {
      result.truncated = true;
      continue;  // keep draining the file, stop collecting records
    }
    const std::optional<ProcessCgroupHierarchy> parsed =
        parseCgroupLine(line);
    if (!parsed) {
      continue;
    }
    result.hierarchies.push_back(std::move(*parsed));
  }
  if (result.hierarchies.empty()) {
    return {CgroupStatus::MalformedData, 0, {}, false, CgroupVersion::Unknown,
            {}};
  }

  const MountDiscovery discovery = discoverCgroupMounts();
  result.version = discovery.version;
  const CgroupMount *v2_mount = nullptr;
  for (const CgroupMount &mount : discovery.mounts) {
    if (mount.fs_type == "cgroup2") {
      v2_mount = &mount;
      break;
    }
  }

  ProcessCgroupHierarchy *unified = nullptr;
  for (ProcessCgroupHierarchy &hierarchy : result.hierarchies) {
    if (hierarchy.hierarchy_id == 0 && v2_mount != nullptr) {
      hierarchy.mount_point = v2_mount->mount_point;
      const std::optional<std::string> absolute = safeAbsolutePath(
          v2_mount->mount_point, v2_mount->root, hierarchy.relative_path);
      if (absolute) {
        hierarchy.absolute_path = *absolute;
        hierarchy.resolvable = true;
        unified = &hierarchy;
      }
      continue;
    }
    const CgroupMount *mount =
        bestV1Mount(discovery.mounts, hierarchy.controllers);
    if (mount == nullptr) {
      continue;
    }
    hierarchy.mount_point = mount->mount_point;
    const std::optional<std::string> absolute = safeAbsolutePath(
        mount->mount_point, mount->root, hierarchy.relative_path);
    if (absolute) {
      hierarchy.absolute_path = *absolute;
      hierarchy.resolvable = true;
    }
  }

  // Read-only v2 resource/metadata snapshot of the selected cgroup directory.
  // Every file is optional: a controller not enabled at this level simply has
  // no control file, and an unreadable (permission-protected) directory yields
  // no values rather than fabricated ones.
  ProcessCgroupResources &resources = result.resources;
  if (unified != nullptr && !unified->absolute_path.empty()) {
    int meta_errno = 0;
    if (const std::optional<std::string> controllers =
            readFileContents(unified->absolute_path + "/cgroup.controllers",
                             &meta_errno)) {
      resources.controllers = splitControllerNames(*controllers);
    }
    meta_errno = 0;
    if (const std::optional<std::string> type =
            readFileContents(unified->absolute_path + "/cgroup.type",
                             &meta_errno)) {
      resources.type = std::string(trimWhitespace(*type));
    }

    const std::string dir = unified->absolute_path;
    resources.readable_file_count +=
        readResourceFile(dir, "cpu.weight", resources.cpu_weight,
                         &parseCgroupValue)
            ? 1
            : 0;
    resources.readable_file_count +=
        readResourceFile(dir, "cpu.max", resources.cpu_max, &parseCgroupCpuMax)
            ? 1
            : 0;
    resources.readable_file_count +=
        readResourceFile(dir, "memory.current", resources.memory_current,
                         &parseCgroupValue)
            ? 1
            : 0;
    resources.readable_file_count +=
        readResourceFile(dir, "memory.max", resources.memory_max,
                         &parseCgroupValue)
            ? 1
            : 0;
    resources.readable_file_count +=
        readResourceFile(dir, "memory.high", resources.memory_high,
                         &parseCgroupValue)
            ? 1
            : 0;
    resources.readable_file_count +=
        readResourceFile(dir, "pids.current", resources.pids_current,
                         &parseCgroupValue)
            ? 1
            : 0;
    resources.readable_file_count +=
        readResourceFile(dir, "pids.max", resources.pids_max,
                         &parseCgroupValue)
            ? 1
            : 0;
  }

  // Identity gate (after the read): discard a stale result if the PID was
  // reused while we were inspecting.
  current = ProcessIdentity::current(identity.pid);
  if (!current) {
    return {CgroupStatus::IdentityUnknown, 0, {}, false, CgroupVersion::Unknown,
            {}};
  }
  if (current->starttime_ticks != identity.starttime_ticks) {
    return {CgroupStatus::ProcessReused, 0, {}, false, CgroupVersion::Unknown,
            {}};
  }
  return result;
}

}  // namespace atm