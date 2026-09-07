#pragma once

#include <array>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace atm {

/**
 * Formats a byte count with the largest whole prefix on a 1024 base,
 * e.g. formatBytes(80'000'000'000) -> "74.5 GB", formatBytes(0) -> "0 B".
 *
 * Shared by the disk and network monitors so every size in the UI uses the
 * same scheme (B / kB / MB / GB / TB).
 */
[[nodiscard]] inline std::string formatBytes(std::uint64_t bytes) {
  static constexpr std::array kSuffixes = {"B", "kB", "MB", "GB", "TB"};
  double value = static_cast<double>(bytes);
  std::size_t suffix = 0;
  constexpr double kKilobyte = 1024.0;
  while (value >= kKilobyte && suffix + 1 < kSuffixes.size()) {
    value /= kKilobyte;
    ++suffix;
  }
  std::ostringstream out;
  out << std::fixed << std::setprecision(value >= 100.0 ? 0 : 1) << value
      << ' ' << kSuffixes[suffix];
  return out.str();
}

}  // namespace atm