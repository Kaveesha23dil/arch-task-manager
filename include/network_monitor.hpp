#pragma once

#include <chrono>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace atm {

/// Per-interface network counters (cumulative, since boot) plus the derived
/// transfer rates for the last measured window.
struct NetworkInterfaceStats {
  std::string name;  // e.g. "wlan0", "enp3s0", "lo", "docker0" (no /dev prefix)

  std::uint64_t rx_bytes = 0;      // total received bytes (cumulative)
  std::uint64_t tx_bytes = 0;      // total transmitted bytes (cumulative)
  std::uint64_t rx_packets = 0;
  std::uint64_t tx_packets = 0;
  std::uint64_t rx_errors = 0;
  std::uint64_t tx_errors = 0;
  std::uint64_t rx_dropped = 0;
  std::uint64_t tx_dropped = 0;

  std::uint64_t rx_bytes_per_second = 0;  // derived from two samples
  std::uint64_t tx_bytes_per_second = 0;

  std::string state;  // operstate: up/down/unknown/dormant/notpresent/...
  bool loopback = false;  // true for "lo" — never shown as a real link
};

/// One complete network snapshot, produced by NetworkMonitor::read().
struct NetworkSnapshot {
  std::vector<NetworkInterfaceStats> interfaces;  // non-loopback by name, lo last
  std::uint64_t total_rx_bytes_per_second = 0;    // non-loopback total
  std::uint64_t total_tx_bytes_per_second = 0;
};

/// Parses /proc/net/dev into per-interface cumulative counters. The two
/// header lines are skipped, every data line is validated, and the state
/// column is filled from /sys/class/net/<name>/operstate. Returns an empty
/// vector when the file is unavailable; malformed lines are skipped.
[[nodiscard]] std::vector<NetworkInterfaceStats> readNetworkStats();

/// Parses the counter columns of /proc/net/dev from an arbitrary text stream
/// (two header lines, then one line per interface). Fills `name` and the eight
/// cumulative counters (rx/tx bytes, packets, errors, drops); the `state` and
/// `loopback` fields are left untouched because they need sysfs, which
/// readNetworkStats() applies afterwards. Lines are validated: a line without
/// ':', sixteen non-numeric fields, or an empty interface name is skipped.
/// Exposes the exact behavior of readNetworkStats() for hermetic unit tests.
[[nodiscard]] std::vector<NetworkInterfaceStats> parseNetworkStatsText(
    std::istream &input);

/// Reads /sys/class/net/<name>/operstate. Returns "unknown" when the file
/// is missing or unreadable so a vanished interface never breaks a read.
[[nodiscard]] std::string readInterfaceState(const std::string &name);

/// Formats a bytes-per-second rate consistently with the shared byte
/// formatter, e.g. 4'500'000 -> "4.3 MB/s", 0 -> "0 B/s".
[[nodiscard]] std::string formatNetworkRate(std::uint64_t bytes_per_second);

/**
 * Monitors network interfaces by comparing successive samples of
 * /proc/net/dev. Like CpuMonitor and DiskMonitor it keeps the previous
 * sample; each read yields the transfer rates of the elapsed window compared
 * to that sample, so the first read only records a baseline and returns zero
 * rates. The refresh cadence is owned by the application's main loop — this
 * class never starts its own thread.
 */
class NetworkMonitor {
 public:
  NetworkMonitor() = default;
  ~NetworkMonitor() = default;

  // Monitors hold diffing state; copying/moving one would duplicate baselines.
  NetworkMonitor(const NetworkMonitor &) = delete;
  NetworkMonitor &operator=(const NetworkMonitor &) = delete;

  [[nodiscard]] NetworkSnapshot read();

 private:
  struct Counters {
    std::uint64_t rx_bytes = 0;
    std::uint64_t tx_bytes = 0;
  };

  std::optional<std::chrono::steady_clock::time_point> previous_time_;
  std::unordered_map<std::string, Counters> previous_;
};

}  // namespace atm