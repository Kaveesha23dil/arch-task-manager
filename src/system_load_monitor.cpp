#include "system_load_monitor.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <sstream>
#include <system_error>

namespace atm {

namespace {

namespace fs = std::filesystem;

/// Trims ASCII whitespace off both ends of a line.
std::string trim(const std::string &text) {
  const auto not_space = [](unsigned char c) { return c > ' '; };
  const auto first = std::find_if(text.begin(), text.end(), not_space);
  if (first == text.end()) {
    return {};
  }
  auto last = std::find_if(text.rbegin(), text.rend(), not_space).base();
  return std::string(first, last);
}

/// Strict non-negative double parse. Accepts only plain base-10 decimal
/// values; rejects negative values, NaN, infinity and trailing garbage.
bool parseNonNegativeDouble(const std::string &token, double &out) {
  if (token.empty()) {
    return false;
  }
  double value = 0.0;
  const auto *begin = token.data();
  const auto *end = begin + token.size();
  const auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc{} || result.ptr != end) {
    return false;
  }
  if (!std::isfinite(value) || value < 0.0) {
    return false;
  }
  out = value;
  return true;
}

/// Reads the first line of a file. Returns false on open/read failure.
bool readFirstLine(const fs::path &path, std::string &line) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return false;
  }
  return static_cast<bool>(std::getline(file, line));
}

/// Reads one /proc/loadavg-style line from `path` into a snapshot.
bool readLoadAvgFromFile(const fs::path &path, LoadAverageSnapshot &out) {
  std::string line;
  if (!readFirstLine(path, line)) {
    return false;
  }
  return parseLoadAverage(line, out);
}

/// Reads one /proc/uptime-style line from `path` into a snapshot.
bool readUptimeFromFile(const fs::path &path, UptimeSnapshot &out) {
  std::string line;
  if (!readFirstLine(path, line)) {
    return false;
  }
  return parseUptime(line, out);
}

}  // namespace

const char *loadSeverityName(LoadSeverity severity) {
  switch (severity) {
    case LoadSeverity::Normal:
      return "NORMAL";
    case LoadSeverity::Elevated:
      return "ELEVATED";
    case LoadSeverity::High:
      return "HIGH";
    case LoadSeverity::Critical:
      return "CRITICAL";
  }
  return "?";
}

LoadSeverity classifyLoadSeverity(double normalized_load) {
  if (normalized_load >= kLoadSeverityCriticalAt) {
    return LoadSeverity::Critical;
  }
  if (normalized_load >= kLoadSeverityHighAt) {
    return LoadSeverity::High;
  }
  if (normalized_load >= kLoadSeverityElevatedAt) {
    return LoadSeverity::Elevated;
  }
  return LoadSeverity::Normal;
}

std::optional<double> normalizeLoad(double load, std::uint32_t online_cpus) {
  if (online_cpus == 0) {
    return std::nullopt;
  }
  return load / static_cast<double>(online_cpus);
}

bool parseLoadAverage(const std::string &line, LoadAverageSnapshot &out) {
  out = LoadAverageSnapshot{};

  const std::string trimmed = trim(line);
  if (trimmed.empty()) {
    return false;
  }

  std::istringstream parser(trimmed);
  std::string token;

  auto nextDouble = [&parser](double &value) {
    std::string t;
    return parser >> t && parseNonNegativeDouble(t, value);
  };

  if (!nextDouble(out.load1) || !nextDouble(out.load5) ||
      !nextDouble(out.load15)) {
    return false;  // the load-average triple is required
  }
  out.readable = true;

  // Optional running/total field ("7/1023"). A missing or malformed field only
  // leaves its availability flag false; the rest of the line survives.
  if (parser >> token) {
    const std::size_t slash = token.find('/');
    if (slash != std::string::npos && slash > 0 &&
        slash + 1 < token.size()) {
      double running = 0.0;
      double total = 0.0;
      if (parseNonNegativeDouble(token.substr(0, slash), running) &&
          parseNonNegativeDouble(token.substr(slash + 1), total)) {
        out.running = running;
        out.total = total;
        out.running_total_available = true;
      }
    }
  }

  // Optional last-PID field ("19041").
  if (parser >> token) {
    std::uint64_t pid = 0;
    const auto *begin = token.data();
    const auto *end = begin + token.size();
    const auto result = std::from_chars(begin, end, pid);
    if (result.ec == std::errc{} && result.ptr == end) {
      out.last_pid = pid;
      out.last_pid_available = true;
    }
  }
  return true;
}

bool parseUptime(const std::string &line, UptimeSnapshot &out) {
  out = UptimeSnapshot{};

  const std::string trimmed = trim(line);
  if (trimmed.empty()) {
    return false;
  }

  std::istringstream parser(trimmed);
  std::string token;
  if (!(parser >> token) ||
      !parseNonNegativeDouble(token, out.uptime_seconds)) {
    return false;
  }
  out.readable = true;

  // Optional idle field. A missing/malformed idle only leaves it at 0.
  if (parser >> token) {
    double idle = 0.0;
    if (parseNonNegativeDouble(token, idle)) {
      out.idle_seconds = idle;
    }
  }
  return true;
}

SystemLoadMonitor::SystemLoadMonitor(std::filesystem::path root)
    : root_(std::move(root)) {}

SystemLoadSnapshot SystemLoadMonitor::read() {
  SystemLoadSnapshot snapshot;
  readLoadAvgFromFile(root_ / "proc" / "loadavg", snapshot.load);
  readUptimeFromFile(root_ / "proc" / "uptime", snapshot.uptime);

  if (snapshot.uptime.readable) {
    const auto uptime = std::chrono::duration<double>(
        snapshot.uptime.uptime_seconds);
    snapshot.boot_time =
        std::chrono::system_clock::now() -
        std::chrono::duration_cast<std::chrono::system_clock::duration>(uptime);
  }
  return snapshot;
}

}  // namespace atm