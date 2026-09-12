#include "network_monitor.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "format_bytes.hpp"

namespace atm {

namespace {

/// Strips leading/trailing ASCII whitespace (including a CR).
std::string trimWhitespace(const std::string &text) {
  std::size_t begin = 0;
  while (begin < text.size() &&
         (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r')) {
    ++begin;
  }
  std::size_t end = text.size();
  while (end > begin &&
         (text[end - 1] == ' ' || text[end - 1] == '\t' ||
          text[end - 1] == '\r')) {
    --end;
  }
  return text.substr(begin, end - begin);
}

}  // namespace

std::string readInterfaceState(const std::string &name) {
  // RAII: the file is closed when `file` goes out of scope.
  std::ifstream file("/sys/class/net/" + name + "/operstate");
  std::string state;
  if (!(file >> state)) {
    return "unknown";  // missing or unreadable — valid per the kernel ABI
  }
  return state;
}

std::vector<NetworkInterfaceStats> parseNetworkStatsText(std::istream &input) {
  std::vector<NetworkInterfaceStats> interfaces;
  std::string line;

  // The kernel file always starts with two header lines ("Inter-| Receive ..."
  // and " face |bytes    packets ..."). Skipping them is part of the parser
  // contract and keeps parseNetworkStatsText byte-for-byte identical to the
  // old file-reading implementation so any input behaves the same way.
  std::getline(input, line);
  std::getline(input, line);
  while (std::getline(input, line)) {
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) {
      continue;  // not an interface data line
    }
    const std::string name = trimWhitespace(line.substr(0, colon));

    // Everything after ':' is a fixed column layout; only the whitelisted
    // offsets below are consumed. Requiring 16 values rejects truncated
    // lines from exotic drivers instead of misreading partially.
    std::istringstream parser(line.substr(colon + 1));
    std::uint64_t fields[16] = {};
    bool complete = true;
    for (std::uint64_t &field : fields) {
      if (!(parser >> field)) {
        complete = false;
        break;
      }
    }
    if (!complete || name.empty()) {
      continue;  // malformed line — skip this interface
    }

    // Layout (see /proc/net/dev column header):
    //   0 rx_bytes  1 rx_packets  2 rx_errs  3 rx_drop  4 rx_fifo
    //   5 rx_frame  6 rx_compressed  7 rx_multicast
    //   8 tx_bytes  9 tx_packets 10 tx_errs 11 tx_drop ...
    NetworkInterfaceStats stats;
    stats.name = name;
    stats.rx_bytes = fields[0];
    stats.rx_packets = fields[1];
    stats.rx_errors = fields[2];
    stats.rx_dropped = fields[3];
    stats.tx_bytes = fields[8];
    stats.tx_packets = fields[9];
    stats.tx_errors = fields[10];
    stats.tx_dropped = fields[11];
    interfaces.push_back(std::move(stats));
  }
  return interfaces;
}

std::vector<NetworkInterfaceStats> readNetworkStats() {
  // RAII: the file is closed when `file` goes out of scope.
  std::ifstream file("/proc/net/dev");
  if (!file.is_open()) {
    return {};
  }
  std::vector<NetworkInterfaceStats> interfaces = parseNetworkStatsText(file);
  for (NetworkInterfaceStats &stats : interfaces) {
    stats.state = readInterfaceState(stats.name);
    stats.loopback = (stats.name == "lo");
  }
  return interfaces;
}

std::string formatNetworkRate(std::uint64_t bytes_per_second) {
  return formatBytes(bytes_per_second) + "/s";
}

NetworkSnapshot NetworkMonitor::read() {
  NetworkSnapshot snapshot;
  const auto now = std::chrono::steady_clock::now();

  std::vector<NetworkInterfaceStats> current = readNetworkStats();

  // Diff against the previous sample, using the *real* elapsed time (never an
  // assumed 1 s). The first call has no baseline and reports zero rates.
  if (previous_time_.has_value() && !current.empty()) {
    const double seconds =
        std::chrono::duration<double>(now - *previous_time_).count();
    if (seconds > 0.0) {
      const auto old = previous_;
      for (NetworkInterfaceStats &stats : current) {
        const auto previous = old.find(stats.name);
        if (previous == old.end()) {
          continue;  // new interface — baseline first, rate from next tick
        }
        if (stats.rx_bytes < previous->second.rx_bytes ||
            stats.tx_bytes < previous->second.tx_bytes) {
          // Counter reset / interface restart: drop this window instead of
          // reporting a huge bogus delta. The baseline below adopts the
          // current (post-reset) values.
          continue;
        }
        stats.rx_bytes_per_second = static_cast<std::uint64_t>(
            static_cast<double>(stats.rx_bytes - previous->second.rx_bytes) /
            seconds);
        stats.tx_bytes_per_second = static_cast<std::uint64_t>(
            static_cast<double>(stats.tx_bytes - previous->second.tx_bytes) /
            seconds);
      }
    }
  }

  // Baseline for the next read. Interfaces that disappeared simply drop out;
  // reappeared or new ones start fresh. A reset counter is re-seeded here.
  previous_time_ = now;
  previous_.clear();
  previous_.reserve(current.size());
  for (const NetworkInterfaceStats &stats : current) {
    previous_.emplace(stats.name, Counters{stats.rx_bytes, stats.tx_bytes});
  }

  // Order: non-loopback interfaces by name ascending, loopback always last so
  // it is never mistaken for the machine's real connection.
  std::sort(current.begin(), current.end(),
            [](const NetworkInterfaceStats &a, const NetworkInterfaceStats &b) {
              if (a.loopback != b.loopback) {
                return !a.loopback;  // non-loopback first
              }
              return a.name < b.name;
            });

  for (const NetworkInterfaceStats &stats : current) {
    if (!stats.loopback) {
      snapshot.total_rx_bytes_per_second += stats.rx_bytes_per_second;
      snapshot.total_tx_bytes_per_second += stats.tx_bytes_per_second;
    }
  }
  snapshot.interfaces = std::move(current);
  return snapshot;
}

}  // namespace atm