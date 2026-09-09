#include "process_resources.hpp"

#include <charconv>
#include <sstream>
#include <string>
#include <vector>

namespace atm {

namespace {

/// Parses a leading unsigned 64-bit integer; returns std::nullopt when the
/// view is empty or does not begin with a digit.
std::optional<std::uint64_t> parseU64(std::string_view view) {
  if (view.empty()) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  const char *begin = view.data();
  const char *end = view.data() + view.size();
  const auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc{} || result.ptr == begin) {
    return std::nullopt;
  }
  return value;
}

/// Parses a leading signed integer; returns std::nullopt on empty/invalid.
std::optional<long> parseInt(std::string_view view) {
  if (view.empty()) {
    return std::nullopt;
  }
  long value = 0;
  const char *begin = view.data();
  const char *end = view.data() + view.size();
  const auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc{} || result.ptr == begin) {
    return std::nullopt;
  }
  return value;
}

/// Parses a single limit value (a non-negative integer or the literal
/// "unlimited") into a ResourceLimit side.
void assignLimitValue(ResourceLimit &limit, bool is_soft, std::string_view word) {
  auto *value = is_soft ? &limit.soft : &limit.hard;
  bool *unlimited = is_soft ? &limit.soft_unlimited : &limit.hard_unlimited;
  if (word == "unlimited") {
    *unlimited = true;
  } else if (const auto v = parseU64(word); v) {
    *value = v;
  }
}

}  // namespace

ProcessResourceLimits parseProcessLimits(std::string_view contents) {
  ProcessResourceLimits limits;
  std::istringstream lines{std::string(contents)};
  std::string line;
  while (std::getline(lines, line)) {
    // Tokenize the line; a /proc/<pid>/limits row is:
    //   <name...> <soft> <hard> [units]
    // The name is a variable-length run of words, the soft/hard values follow
    // it (numbers or "unlimited"), and an optional units word trails. Find the
    // first token that is a number or "unlimited" — everything before it is
    // the limit name.
    std::vector<std::string> tokens;
    {
      std::istringstream head(line);
      std::string token;
      while (head >> token) {
        tokens.push_back(token);
      }
    }
    std::size_t value_at = std::string::npos;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
      if (tokens[i] == "unlimited" || parseU64(tokens[i]).has_value()) {
        value_at = i;
        break;
      }
    }
    if (value_at == std::string::npos || value_at + 1 >= tokens.size()) {
      continue;  // header line, or a row with no usable value pair
    }
    std::string name;
    for (std::size_t i = 0; i < value_at; ++i) {
      if (!name.empty()) {
        name += ' ';
      }
      name += tokens[i];
    }
    if (name != "Max open files" && name != "Max processes" &&
        name != "Max stack size" && name != "Max locked memory" &&
        name != "Max address space" && name != "Max core file size" &&
        name != "Max pending signals" &&
        name != "Max POSIX message queues" && name != "Max msgqueue size" &&
        name != "Max realtime priority" && name != "Max realtime timeout") {
      continue;  // not a limit we surface (cpu time, file size, data size, ...)
    }
    ResourceLimit pair;
    assignLimitValue(pair, true, tokens[value_at]);
    assignLimitValue(pair, false, tokens[value_at + 1]);
    if (pair.empty()) {
      continue;
    }
    if (name == "Max open files") {
      limits.open_files = pair;
    } else if (name == "Max processes") {
      limits.max_processes = pair;
    } else if (name == "Max stack size") {
      limits.max_stack_size = pair;
    } else if (name == "Max locked memory") {
      limits.locked_memory = pair;
    } else if (name == "Max address space") {
      limits.address_space = pair;
    } else if (name == "Max core file size") {
      limits.core_file_size = pair;
    } else if (name == "Max pending signals") {
      limits.pending_signals = pair;
    } else if (name == "Max POSIX message queues" ||
               name == "Max msgqueue size") {
      limits.posix_message_queues = pair;
    } else if (name == "Max realtime priority") {
      limits.realtime_priority = pair;
    } else if (name == "Max realtime timeout") {
      limits.realtime_timeout = pair;
    }
  }
  return limits;
}

ProcessIoCounters parseProcessIo(std::string_view contents) {
  ProcessIoCounters counters;
  std::istringstream lines{std::string(contents)};
  std::string line;
  while (std::getline(lines, line)) {
    std::istringstream head(line);
    std::string key;
    if (!(head >> key)) {
      continue;
    }
    std::uint64_t value = 0;
    if (key == "rchar:" && head >> value) {
      counters.read_bytes = value;
      counters.available = true;
    } else if (key == "wchar:" && head >> value) {
      counters.write_bytes = value;
      counters.available = true;
    } else if (key == "syscr:" && head >> value) {
      counters.read_syscalls = value;
      counters.available = true;
    } else if (key == "syscw:" && head >> value) {
      counters.write_syscalls = value;
      counters.available = true;
    } else if (key == "cancelled_write_bytes:" && head >> value) {
      counters.cancelled_write_bytes = value;
      counters.available = true;
    }
  }
  return counters;
}

std::optional<ProcessStatParse>
parseProcessStat(std::string_view line) {
  const std::size_t open = line.find('(');
  const std::size_t close = line.rfind(')');
  if (open == std::string_view::npos || close == std::string_view::npos ||
      close < open) {
    return std::nullopt;  // malformed — no well-formed comm
  }

  std::vector<std::string> fields;
  {
    std::istringstream tail{std::string(line.substr(close + 1))};
    std::string field;
    while (tail >> field) {
      fields.push_back(field);
    }
  }
  // Indices 0..12 map to kernel fields 3..15; stime is the last required one,
  // so anything shorter is a truncated (orphaned) line.
  if (fields.size() < 13) {
    return std::nullopt;
  }

  ProcessStatParse data;
  data.comm = std::string(line.substr(open + 1, close - open - 1));
  if (!fields[0].empty()) {
    data.state = fields[0].front();
  }
  data.ppid = static_cast<int>(parseInt(fields[1]).value_or(0));
  data.utime = parseU64(fields[11]).value_or(0);
  data.stime = parseU64(fields[12]).value_or(0);
  if (fields.size() > 17) {
    data.num_threads =
        static_cast<std::uint32_t>(parseU64(fields[17]).value_or(0));
  }
  if (fields.size() > 19) {
    data.starttime_ticks = parseU64(fields[19]).value_or(0);
  }
  return data;
}

ProcessStatusParse parseProcessStatus(std::string_view contents) {
  ProcessStatusParse data;
  std::istringstream lines{std::string(contents)};
  std::string line;
  while (std::getline(lines, line)) {
    std::istringstream head(line);
    std::string key;
    if (!(head >> key)) {
      continue;
    }
    if (key == "State:") {
      std::string value;
      if (head >> value && !value.empty()) {
        data.state = value.front();
      }
    } else if (key == "Uid:") {
      unsigned long value = 0;
      if (head >> value) {
        data.uid = static_cast<std::uint32_t>(value);  // real UID
      }
    } else if (key == "Threads:") {
      std::uint32_t value = 0;
      if (head >> value) {
        data.threads = value;
      }
    } else if (key == "VmSize:") {
      std::uint64_t value = 0;
      std::string unit;
      if (head >> value >> unit) {
        data.vm_size_kib = value;
      }
    } else if (key == "VmRSS:") {
      std::uint64_t value = 0;
      std::string unit;
      if (head >> value >> unit) {
        data.vm_rss_kib = value;
      }
    } else if (key == "RssShmem:" || key == "RssFile:") {
      std::uint64_t value = 0;
      std::string unit;
      if (head >> value >> unit) {
        data.shared_kib += value;  // resident shared = RssShmem + RssFile
      }
    } else if (key == "voluntary_ctxt_switches:") {
      std::uint64_t value = 0;
      if (head >> value) {
        data.voluntary_context_switches = value;
      }
    } else if (key == "nonvoluntary_ctxt_switches:") {
      std::uint64_t value = 0;
      if (head >> value) {
        data.nonvoluntary_context_switches = value;
      }
    }
  }
  return data;
}

IoRates computeIoRates(bool same_process, const ProcessIoCounters &previous,
                       const ProcessIoCounters &current,
                       double elapsed_seconds) {
  IoRates rates;
  // A new/restarted process (possibly a reused PID) has no valid historical
  // deltas; the first sample only establishes a baseline.
  if (!same_process || !previous.available || !current.available) {
    return rates;
  }
  // Invalid or non-positive elapsed time cannot yield a meaningful rate.
  if (!(elapsed_seconds > 0.0)) {
    return rates;
  }
  const auto delta = [](std::uint64_t a, std::uint64_t b) -> std::uint64_t {
    return b >= a ? b - a : 0;  // counter reset -> 0, never negative
  };
  rates.read_rate = static_cast<double>(delta(previous.read_bytes,
                                              current.read_bytes)) /
                    elapsed_seconds;
  rates.write_rate = static_cast<double>(delta(previous.write_bytes,
                                               current.write_bytes)) /
                     elapsed_seconds;
  return rates;
}

}  // namespace atm
