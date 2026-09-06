#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "cpu_monitor.hpp"

namespace {

using namespace std::chrono_literals;

constexpr std::chrono::seconds kRefreshInterval{1};
constexpr int kPercentPrecision = 1;
constexpr int kPercentWidth = 5;

/// Formats a utilization value as " 34.7" (fixed width so the updating line
/// does not shimmer as the value changes).
std::string formatPercent(double percent) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(kPercentPrecision)
      << std::setw(kPercentWidth) << percent;
  return out.str();
}

void printBanner(double initial) {
  std::cout << "========================================\n"
               "ARCH TASK MANAGER\n"
               "=================\n\n"
            << "CPU Usage: " << formatPercent(initial) << "%\n\n"
            << "Updating every " << kRefreshInterval.count() << " second...\n\n"
            << "========================================\n\n";
}

}  // namespace

int main() {
  atm::CpuMonitor monitor;

  // Record a baseline sample so the printed readings are deltas between two
  // /proc/stat samples rather than raw stat values. The return value is
  // intentionally ignored: the first call only establishes the baseline.
  static_cast<void>(monitor.readUsage());

  // Let at least one refresh interval elapse before the first diff sample,
  // otherwise the two back-to-back reads may observe zero elapsed ticks and
  // report an uncomputable (nullopt) delta.
  std::this_thread::sleep_for(kRefreshInterval);

  const auto first_usage = monitor.readUsage();
  if (!first_usage.has_value()) {
    std::cerr << "ERROR: could not read CPU usage from /proc/stat\n";
    return EXIT_FAILURE;
  }

  printBanner(*first_usage);

  for (;;) {
    std::this_thread::sleep_for(kRefreshInterval);

    const auto usage = monitor.readUsage();
    if (!usage.has_value()) {
      std::cerr << "\nERROR: could not read CPU usage from /proc/stat\n";
      return EXIT_FAILURE;
    }

    std::cout << "\rCPU Usage: " << formatPercent(*usage) << "% "
              << std::flush;
  }
}