#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>
#include <poll.h>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "cpu_monitor.hpp"
#include "memory_monitor.hpp"
#include "process_monitor.hpp"

namespace {

using namespace std::chrono_literals;

constexpr std::chrono::seconds kRefreshInterval{1};
constexpr int kPercentPrecision = 1;
constexpr int kPercentWidth = 5;
constexpr int kLabelWidth = 21;
constexpr double kBytesPerKilobyte = 1024.0;
constexpr std::size_t kNameColumnWidth = 18;
constexpr std::size_t kMaxNameWidth = 16;

/// Formats a utilization value as " 34.7" (fixed width so the updating view
/// does not shimmer as the value changes).
std::string formatPercent(double percent) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(kPercentPrecision)
      << std::setw(kPercentWidth) << percent;
  return out.str();
}

/// Formats a value given in kibibytes as a human-readable size with the
/// largest whole prefix, e.g. 15872000 -> "15.1 GB", 524288 -> "512 MB".
std::string formatKibibytes(std::uint64_t kibibytes) {
  static constexpr std::array kSuffixes = {"kB", "MB", "GB", "TB"};
  double value = static_cast<double>(kibibytes);
  std::size_t suffix = 0;
  while (value >= kBytesPerKilobyte && suffix + 1 < kSuffixes.size()) {
    value /= kBytesPerKilobyte;
    ++suffix;
  }
  std::ostringstream out;
  out << std::fixed << std::setprecision(value >= 100.0 ? 0 : 1) << value
      << ' ' << kSuffixes[suffix];
  return out.str();
}

/// Truncates a name to kMaxNameWidth characters so columns stay aligned.
std::string fitName(const std::string &name) {
  if (name.size() <= kMaxNameWidth) {
    return name;
  }
  return name.substr(0, kMaxNameWidth);
}

/// Appends "<label>" left-aligned in a fixed-width column followed by
/// "<value>", keeping every label/value column aligned across the output.
void appendLabeled(std::ostringstream &out, const std::string &label,
                   const std::string &value) {
  out << std::left << std::setw(kLabelWidth) << label << value << '\n';
}

/// Renders the banner and the system-wide CPU + memory summary lines.
void renderHeader(std::ostringstream &out, double cpu_usage,
                  const atm::MemoryInfo &memory) {
  out << "========================================\n"
         "ARCH TASK MANAGER\n"
         "=================\n\n";
  appendLabeled(out, "CPU Usage:", formatPercent(cpu_usage) + "%");
  appendLabeled(out, "Memory Usage:",
                formatPercent(memory.usagePercent()) + "%");
}

/// Renders the MEMORY and SWAP detail sections (Step 2).
void renderMemorySections(std::ostringstream &out,
                          const atm::MemoryInfo &memory) {
  out << "\n## MEMORY\n\n";
  appendLabeled(out, "Total:", formatKibibytes(memory.total));
  appendLabeled(out, "Used:", formatKibibytes(memory.used()));
  appendLabeled(out, "Available:", formatKibibytes(memory.available));
  appendLabeled(out, "Free:", formatKibibytes(memory.free));
  appendLabeled(out, "Cached:", formatKibibytes(memory.cached));
  appendLabeled(out, "Buffers:", formatKibibytes(memory.buffers));
  appendLabeled(out, "Usage:", formatPercent(memory.usagePercent()) + "%");

  out << "\n## SWAP\n\n";
  appendLabeled(out, "Total:", formatKibibytes(memory.swap_total));
  appendLabeled(out, "Used:", formatKibibytes(memory.swapUsed()));
  appendLabeled(out, "Free:", formatKibibytes(memory.swap_free));
  appendLabeled(out, "Usage:", formatPercent(memory.swapUsagePercent()) + "%");
}

/// Renders the process table, sorted by `sort`.
void renderProcessTable(std::ostringstream &out,
                        const std::vector<atm::Process> &processes) {
  out << "\n## PROCESSES\n\n"
      << std::right << std::setw(7) << "PID" << "  " << std::left
      << std::setw(kNameColumnWidth) << "NAME" << "  " << std::right
      << std::setw(8) << "CPU" << "  " << std::setw(9) << "RAM" << "  "
      << std::left << std::setw(10) << "STATE" << '\n';

  for (const atm::Process &process : processes) {
    out << std::right << std::setw(7) << process.pid << "  " << std::left
        << std::setw(kNameColumnWidth) << fitName(process.name) << "  "
        << std::right << std::setw(8) << (formatPercent(process.cpu_percent) + "%")
        << "  " << std::setw(9) << formatKibibytes(process.memory_kib) << "  "
        << std::left << std::setw(10)
        << atm::processStateName(process.state) << '\n';
  }
}

/// Renders the total/running/sleeping/stopped/zombie counters.
void renderProcessStats(std::ostringstream &out,
                        const atm::ProcessStats &stats) {
  out << "\n## Process Statistics\n\n";
  appendLabeled(out, "Total:", std::to_string(stats.total));
  appendLabeled(out, "Running:", std::to_string(stats.running));
  appendLabeled(out, "Sleeping:", std::to_string(stats.sleeping));
  appendLabeled(out, "Stopped:", std::to_string(stats.stopped));
  appendLabeled(out, "Zombie:", std::to_string(stats.zombie));
}

/// Renders the full text view (banner + summary + memory + swap + processes +
/// statistics + footer). Each frame is a self-contained 1 s snapshot.
std::string renderFrame(double cpu_usage, const atm::MemoryInfo &memory,
                        const std::vector<atm::Process> &processes,
                        const atm::ProcessStats &stats,
                        atm::ProcessSort sort) {
  std::ostringstream out;
  renderHeader(out, cpu_usage, memory);
  renderMemorySections(out, memory);
  renderProcessTable(out, processes);
  renderProcessStats(out, stats);
  out << "\n---\n\n"
         "Processes: "
      << processes.size() << "\n"
      << "Sort: [1] CPU  [2] Memory  [3] PID  [4] Name"
         " (current: "
      << atm::processSortName(sort) << ")\n"
      << "Updating every " << kRefreshInterval.count() << " second...\n";
  return out.str();
}

/// Clears the terminal and redraws the whole view in place.
void renderView(double cpu_usage, const atm::MemoryInfo &memory,
                const std::vector<atm::Process> &processes,
                const atm::ProcessStats &stats, atm::ProcessSort sort) {
  // ANSI "clear entire screen" + "cursor to home" so the multi-line frame
  // refreshes in place instead of scrolling the terminal.
  std::cout << "\033[2J\033[H";
  std::cout << renderFrame(cpu_usage, memory, processes, stats, sort)
            << std::flush;
}

/// Non-blocking read of the sort selector: the user presses one digit then
/// Enter, and the digit is picked up on the next refresh. Returns std::nullopt
/// when there is no pending input.
std::optional<atm::ProcessSort> readSortSelection() {
  struct pollfd stdin_fd = {STDIN_FILENO, POLLIN, 0};
  const int ready = ::poll(&stdin_fd, 1, 0);
  if (ready <= 0 || (stdin_fd.revents & POLLIN) == 0) {
    return std::nullopt;
  }

  char buffer[16];
  const auto count = ::read(STDIN_FILENO, buffer, sizeof(buffer));
  if (count <= 0) {
    return std::nullopt;  // EOF or error: not a terminal, just ignore input
  }
  for (std::size_t i = 0; i < static_cast<std::size_t>(count); ++i) {
    switch (buffer[i]) {
      case '1':
        return atm::ProcessSort::Cpu;
      case '2':
        return atm::ProcessSort::Memory;
      case '3':
        return atm::ProcessSort::Pid;
      case '4':
        return atm::ProcessSort::Name;
      default:
        break;
    }
  }
  return std::nullopt;
}

}  // namespace

int main() {
  atm::CpuMonitor cpu_monitor;
  atm::MemoryMonitor memory_monitor;
  atm::ProcessMonitor process_monitor;

  atm::ProcessSort sort = atm::ProcessSort::Cpu;

  // Baseline samples so the first printed frame already shows real deltas:
  // CPU usage over the sleep below, and per-process CPU over the same window.
  static_cast<void>(cpu_monitor.readUsage());
  static_cast<void>(process_monitor.read(0));
  std::this_thread::sleep_for(kRefreshInterval);

  const auto first_cpu = cpu_monitor.readUsage();
  if (!first_cpu.has_value()) {
    std::cerr << "ERROR: could not read CPU usage from /proc/stat\n";
    return EXIT_FAILURE;
  }

  const auto first_memory = memory_monitor.read();
  if (!first_memory.has_value()) {
    std::cerr << "Error: Unable to read /proc/meminfo\n";
    return EXIT_FAILURE;
  }

  auto snapshot = process_monitor.read(first_memory->total);
  atm::sortProcesses(snapshot.processes, sort);
  renderView(*first_cpu, *first_memory, snapshot.processes, snapshot.stats,
             sort);

  for (;;) {
    std::this_thread::sleep_for(kRefreshInterval);

    if (const auto selection = readSortSelection()) {
      sort = *selection;
    }

    const auto cpu = cpu_monitor.readUsage();
    if (!cpu.has_value()) {
      std::cerr << "\nERROR: could not read CPU usage from /proc/stat\n";
      return EXIT_FAILURE;
    }

    const auto memory = memory_monitor.read();
    if (!memory.has_value()) {
      std::cerr << "\nError: Unable to read /proc/meminfo\n";
      return EXIT_FAILURE;
    }

    auto next = process_monitor.read(memory->total);
    atm::sortProcesses(next.processes, sort);
    renderView(*cpu, *memory, next.processes, next.stats, sort);
  }
}