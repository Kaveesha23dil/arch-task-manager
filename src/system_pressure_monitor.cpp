#include "system_pressure_monitor.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <sstream>
#include <system_error>

namespace atm {

namespace {

namespace fs = std::filesystem;

/// Report        every PSI `total` in microseconds (kernel convention).
inline constexpr std::uint64_t kUsecPerSecond = 1'000'000;

/// Trims ASCII whitespace off both ends of a string.
std::string trim(const std::string &text) {
  const auto not_space = [](unsigned char c) { return c > ' '; };
  const auto first = std::find_if(text.begin(), text.end(), not_space);
  if (first == text.end()) {
    return {};
  }
  auto last = std::find_if(text.rbegin(), text.rend(), not_space).base();
  return std::string(first, last);
}

/// Strict double parse for a PSI average. Accepts only plain base-10 decimal
/// values; rejects negative values, NaN, infinity and trailing garbage.
bool parseDoubleToken(const std::string &token, double &out) {
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

/// Strict unsigned parse for a PSI `total`. Overflow and a leading minus are
/// rejected by std::from_chars.
bool parseTotalToken(const std::string &token, std::uint64_t &out) {
  if (token.empty()) {
    return false;
  }
  std::uint64_t value = 0;
  const auto *begin = token.data();
  const auto *end = begin + token.size();
  const auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc{} || result.ptr != end) {
    return false;
  }
  out = value;
  return true;
}

/// Reads one /proc/pressure/<resource> file into a category, distinguishing
/// "file missing" (PSI unsupported) from "exists but cannot be opened".
void readCategoryFile(const fs::path &path, PressureCategoryData &out) {
  std::error_code ec;
  const fs::file_status st = fs::status(path, ec);
  if (ec) {
    out.status =
        ec == std::errc::no_such_file_or_directory
            ? PressureReadStatus::Unsupported
            : PressureReadStatus::Unreadable;
    return;
  }
  if (!fs::exists(st)) {
    out.status = PressureReadStatus::Unsupported;
    return;
  }

  std::ifstream file(path);
  if (!file.is_open()) {
    out.status = PressureReadStatus::Unreadable;
    return;
  }
  const std::size_t lines = parsePressureFile(file, out.some, out.full);
  out.status =
      lines > 0 ? PressureReadStatus::Read : PressureReadStatus::Empty;
}

}  // namespace

const char *pressureCategoryName(PressureCategory category) {
  switch (category) {
    case PressureCategory::Cpu:
      return "CPU";
    case PressureCategory::Memory:
      return "Memory";
    case PressureCategory::Io:
      return "I/O";
  }
  return "?";
}

const char *pressureSeverityName(PressureSeverity severity) {
  switch (severity) {
    case PressureSeverity::Normal:
      return "NORMAL";
    case PressureSeverity::Elevated:
      return "ELEVATED";
    case PressureSeverity::High:
      return "HIGH";
    case PressureSeverity::Critical:
      return "CRITICAL";
  }
  return "?";
}

const char *pressureReadStatusName(PressureReadStatus status) {
  switch (status) {
    case PressureReadStatus::Unsupported:
      return "unsupported";
    case PressureReadStatus::Unreadable:
      return "unreadable";
    case PressureReadStatus::Empty:
      return "no data";
    case PressureReadStatus::Read:
      return "read";
  }
  return "?";
}

PressureSeverity classifyPressureSeverity(double pressure_percent) {
  if (pressure_percent >= kPressureSeverityCriticalAt) {
    return PressureSeverity::Critical;
  }
  if (pressure_percent >= kPressureSeverityHighAt) {
    return PressureSeverity::High;
  }
  if (pressure_percent >= kPressureSeverityElevatedAt) {
    return PressureSeverity::Elevated;
  }
  return PressureSeverity::Normal;
}

bool parsePressureLine(const std::string &line, bool &is_full,
                       PressureMetric &out) {
  const std::string trimmed = trim(line);
  if (trimmed.empty()) {
    return false;
  }

  std::istringstream parser(trimmed);
  std::string token;
  if (!(parser >> token)) {
    return false;
  }
  if (token == "some") {
    is_full = false;
  } else if (token == "full") {
    is_full = true;
  } else {
    return false;  // unknown/unsupported line type — ignored, not fatal
  }

  out = PressureMetric{};
  out.line_present = true;

  std::string kv;
  while (parser >> kv) {
    const std::size_t eq = kv.find('=');
    if (eq == std::string::npos || eq == 0) {
      continue;  // "key" or "=value": not a parseable key=token pair
    }
    const std::string key = kv.substr(0, eq);
    const std::string value = kv.substr(eq + 1);

    if (key == "avg10") {
      double v = 0.0;
      if (parseDoubleToken(value, v)) {
        out.avg10 = v;
      }
    } else if (key == "avg60") {
      double v = 0.0;
      if (parseDoubleToken(value, v)) {
        out.avg60 = v;
      }
    } else if (key == "avg300") {
      double v = 0.0;
      if (parseDoubleToken(value, v)) {
        out.avg300 = v;
      }
    } else if (key == "total") {
      std::uint64_t v = 0;
      if (parseTotalToken(value, v)) {
        out.total = v;
      }
    }
    // Unknown future keys are ignored for forward compatibility.
  }
  return true;
}

std::size_t parsePressureFile(std::istream &in, PressureMetric &some,
                              PressureMetric &full) {
  some = PressureMetric{};
  full = PressureMetric{};

  std::string line;
  std::size_t recognized = 0;
  while (std::getline(in, line)) {
    bool is_full = false;
    PressureMetric metric;
    if (parsePressureLine(line, is_full, metric)) {
      (is_full ? full : some) = metric;
      ++recognized;
    }
  }
  return recognized;
}

const PressureCategoryData &SystemPressureSnapshot::category(
    PressureCategory c) const {
  switch (c) {
    case PressureCategory::Cpu:
      return cpu;
    case PressureCategory::Memory:
      return memory;
    case PressureCategory::Io:
      return io;
  }
  return cpu;
}

bool SystemPressureSnapshot::anyAvailable() const {
  return cpu.some.line_present || cpu.full.line_present ||
         memory.some.line_present || memory.full.line_present ||
         io.some.line_present || io.full.line_present;
}

bool SystemPressureSnapshot::hasLine(PressureCategory c, bool full) const {
  const PressureMetric &m =
      full ? category(c).full : category(c).some;
  return m.line_present;
}

std::optional<double> SystemPressureSnapshot::headlineAvg10(
    PressureCategory c) const {
  const PressureCategoryData &d = category(c);
  if (d.some.avg10.has_value()) {
    return d.some.avg10;
  }
  return d.full.avg10;
}

std::optional<PressureSeverity> SystemPressureSnapshot::severity(
    PressureCategory c) const {
  const std::optional<double> avg = headlineAvg10(c);
  if (!avg.has_value()) {
    return std::nullopt;
  }
  return classifyPressureSeverity(*avg);
}

SystemPressureMonitor::SystemPressureMonitor(std::filesystem::path root)
    : root_(std::move(root)) {}

SystemPressureSnapshot SystemPressureMonitor::read() {
  SystemPressureSnapshot snapshot;
  readCategoryFile(root_ / "proc" / "pressure" / "cpu", snapshot.cpu);
  readCategoryFile(root_ / "proc" / "pressure" / "memory", snapshot.memory);
  readCategoryFile(root_ / "proc" / "pressure" / "io", snapshot.io);

  // --- Stalled-time rates from cumulative total deltas ---------------------
  const auto now = std::chrono::steady_clock::now();
  const bool have_previous = previous_read_.has_value();
  const double elapsed_seconds =
      have_previous
          ? std::chrono::duration<double>(now - *previous_read_).count()
          : 0.0;

  const PressureCategory categories[] = {
      PressureCategory::Cpu, PressureCategory::Memory, PressureCategory::Io};
  for (const PressureCategory category : categories) {
    PressureCategoryData *data = nullptr;
    switch (category) {
      case PressureCategory::Cpu:
        data = &snapshot.cpu;
        break;
      case PressureCategory::Memory:
        data = &snapshot.memory;
        break;
      case PressureCategory::Io:
        data = &snapshot.io;
        break;
    }
    for (bool full : {false, true}) {
      PressureMetric &metric = full ? data->full : data->some;
      if (!metric.total.has_value()) {
        continue;  // metric vanished this refresh — baseline kept in case it
                   // reappears with a (possibly larger) total
      }

      const auto key = std::make_pair(category, full);
      const auto it = previous_total_.find(key);
      if (!have_previous || it == previous_total_.end()) {
        // First observation of this cumulative counter: anchor the baseline
        // so the next refresh can produce a delta.
        previous_total_[key] = *metric.total;
        continue;
      }

      if (*metric.total > it->second && elapsed_seconds > 0.0) {
        // Normal increase; a long elapsed time (suspend/resume, slow refresh)
        // simply yields a proportionally smaller rate.
        metric.rate_usec_per_second =
            static_cast<double>(*metric.total - it->second) / elapsed_seconds;
      } else if (*metric.total == it->second && elapsed_seconds > 0.0) {
        metric.rate_usec_per_second = 0.0;  // no stalled time since last read
      }
      // Counter decreased or reset: no negative delta; re-anchor the baseline.

      it->second = *metric.total;
    }
  }
  previous_read_ = now;

  return snapshot;
}

}  // namespace atm