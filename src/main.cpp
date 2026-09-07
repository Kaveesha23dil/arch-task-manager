#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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
#include "process_actions.hpp"
#include "process_monitor.hpp"
#include "process_tree.hpp"

namespace {

using namespace std::chrono_literals;

/// Which full-screen view the live loop renders each second.
enum class ViewMode {
  List,  // the flat process table (default)
  Tree,  // the parent/child process tree
};

constexpr std::chrono::seconds kRefreshInterval{1};
constexpr int kPercentPrecision = 1;
constexpr int kPercentWidth = 5;
constexpr int kLabelWidth = 21;
constexpr double kBytesPerKilobyte = 1024.0;
constexpr std::size_t kNameColumnWidth = 18;
constexpr std::size_t kMaxNameWidth = 16;
constexpr int kMinNice = -20;
constexpr int kMaxNice = 19;

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

/// One table row, shared by the live view and the process-selection screen.
std::string formatProcessRow(const atm::Process &process) {
  std::ostringstream out;
  out << std::right << std::setw(7) << process.pid << "  " << std::left
      << std::setw(kNameColumnWidth) << fitName(process.name) << "  "
      << std::right << std::setw(8) << (formatPercent(process.cpu_percent) + "%")
      << "  " << std::setw(9) << formatKibibytes(process.memory_kib) << "  "
      << std::left << std::setw(10) << atm::processStateName(process.state);
  return out.str();
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
      << "    PID  NAME                     CPU        RAM  STATE\n";
  for (const atm::Process &process : processes) {
    out << "  " << formatProcessRow(process) << '\n';
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

/// Renders the process-tree view (banner + summary + tree + tree stats +
/// footer). Each frame is a self-contained 1 s snapshot.
std::string renderTreeFrame(double cpu_usage, const atm::MemoryInfo &memory,
                            const atm::ProcessTree &tree) {
  std::ostringstream out;
  renderHeader(out, cpu_usage, memory);
  out << "\n## PROCESS TREE\n\n"
      << atm::renderProcessTree(tree) << "\n\n"
      << "Total processes: " << tree.stats.total << "\n"
      << "Root processes: " << tree.stats.root_count << "\n"
      << "Maximum tree depth: " << tree.stats.max_depth << "\n"
      << "\n---\n\n"
      << "View: [l] Process List  [t] Process Tree (current: Tree)\n"
      << "Tree order: PID ascending\n"
      << "Manage: press 'm' (then Enter) to control a process by PID\n"
      << "Updating every " << kRefreshInterval.count() << " second...\n";
  return out.str();
}

/// Renders the full text view (banner + summary + memory + swap + processes +
/// statistics + footer). Each frame is a self-contained 1 s snapshot.
std::string renderFrame(double cpu_usage, const atm::MemoryInfo &memory,
                        const std::vector<atm::Process> &processes,
                        const atm::ProcessStats &stats, atm::ProcessSort sort,
                        ViewMode view, const atm::ProcessTree &tree) {
  if (view == ViewMode::Tree) {
    return renderTreeFrame(cpu_usage, memory, tree);
  }

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
      << "View: [l] Process List  [t] Process Tree (current: List)\n"
      << "Manage: press 'm' (then Enter) to control a process by PID\n"
      << "Updating every " << kRefreshInterval.count() << " second...\n";
  return out.str();
}

/// Clears the terminal and redraws the whole view in place.
void renderView(double cpu_usage, const atm::MemoryInfo &memory,
                const std::vector<atm::Process> &processes,
                const atm::ProcessStats &stats, atm::ProcessSort sort,
                ViewMode view, const atm::ProcessTree &tree) {
  // ANSI "clear entire screen" + "cursor to home" so the multi-line frame
  // refreshes in place instead of scrolling the terminal.
  std::cout << "\033[2J\033[H";
  std::cout << renderFrame(cpu_usage, memory, processes, stats, sort, view,
                           tree)
            << std::flush;
}

/// Strips leading/trailing whitespace (including a CR) from a line.
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

/// Strict integer parse (handles an optional leading '-'). Returns false for
/// empty/non-numeric/overflowing text; the flags of `readLine` are not used.
bool parseSignedInteger(const std::string &text, int &out) {
  if (text.empty()) {
    return false;
  }
  errno = 0;
  char *end = nullptr;
  const long value = std::strtol(text.c_str(), &end, 10);
  if (errno == ERANGE || end == text.c_str() || *end != '\0') {
    return false;
  }
  if (value < INT_MIN || value > INT_MAX) {
    return false;
  }
  out = static_cast<int>(value);
  return true;
}

/// Reads raw terminal input without blocking. One complete line at a time is
/// interpreted: digits 1-4 switch the sort order, 'm' enters process-control
/// mode, anything else is ignored. Because the live loop must not lose bytes
/// meant for the (blocking) control prompts, all input funnels through an
/// internal buffer shared with readLine().
class ConsoleInput {
 public:
  enum class Command {
    None,
    SortCpu,
    SortMemory,
    SortPid,
    SortName,
    ViewList,
    ViewTree,
    Manage,
  };

  /// Non-blocking: drains whatever stdin currently has, then returns the next
  /// complete command line, if any.
  Command pollCommand() {
    drainAvailable();
    const auto command = nextCommand();
    if (command == Command::None && readEof()) {
      return Command::None;
    }
    return command;
  }

  /// Blocking: returns the next complete line, consuming any bytes already in
  /// the buffer first. Returns std::nullopt at EOF (so /dev/null input or
  /// Ctrl+D cancels a prompt). Raw poll()+read() is used exclusively so no
  /// second (stdio) buffer can swallow bytes drained via drainAvailable().
  std::optional<std::string> readLine() {
    for (;;) {
      const std::size_t nl = buffer_.find('\n');
      if (nl != std::string::npos) {
        std::string line = buffer_.substr(0, nl);
        buffer_.erase(0, nl + 1);
        return line;
      }
      if (eof_) {
        if (buffer_.empty()) {
          return std::nullopt;
        }
        std::string line = std::move(buffer_);
        buffer_.clear();
        return line;  // EOF mid-line: hand over whatever arrived
      }
      char ch = '\0';
      if (!readCharBlocking(ch)) {
        eof_ = true;
        continue;  // next loop iteration reports EOF / remaining buffer
      }
      buffer_.push_back(ch);
    }
  }

 private:
  std::string buffer_;
  bool eof_ = false;

  bool readCharBlocking(char &ch) {
    struct pollfd stdin_fd = {STDIN_FILENO, POLLIN, 0};
    if (::poll(&stdin_fd, 1, -1) <= 0) {
      return false;
    }
    ssize_t count = ::read(STDIN_FILENO, &ch, 1);
    if (count > 0) {
      return true;
    }
    if (count < 0 && errno == EINTR) {
      return readCharBlocking(ch);
    }
    return false;  // 0 = EOF, -1 with other errno treated as EOF too
  }

  void drainAvailable() {
    struct pollfd stdin_fd = {STDIN_FILENO, POLLIN, 0};
    if (::poll(&stdin_fd, 1, 0) <= 0 ||
        (stdin_fd.revents & POLLIN) == 0) {
      return;
    }
    char chunk[256];
    const auto count = ::read(STDIN_FILENO, chunk, sizeof(chunk));
    if (count > 0) {
      buffer_.append(chunk, static_cast<std::size_t>(count));
    } else if (count == 0) {
      eof_ = true;
    }
  }

  bool readEof() const { return eof_; }

  /// Interprets and consumes the first buffered line as a command.
  Command nextCommand() {
    const std::size_t nl = buffer_.find('\n');
    if (nl == std::string::npos) {
      return Command::None;
    }
    const std::string token = trimWhitespace(buffer_.substr(0, nl));
    buffer_.erase(0, nl + 1);  // always consume the whole line
    if (token == "1") return Command::SortCpu;
    if (token == "2") return Command::SortMemory;
    if (token == "3") return Command::SortPid;
    if (token == "4") return Command::SortName;
    if (token == "l" || token == "L") return Command::ViewList;
    if (token == "t" || token == "T") return Command::ViewTree;
    if (token == "m" || token == "M") return Command::Manage;
    return Command::None;
  }
};

/// Prints a confirmation prompt and waits for y/Y/yes (anything else is a
/// "no"). Never refuses to return on EOF: EOF cancels.
bool confirm(const std::string &prompt, ConsoleInput &input) {
  std::cout << prompt << " [y/N] " << std::flush;
  const std::optional<std::string> line = input.readLine();
  if (!line) {
    std::cout << '\n';
    return false;
  }
  const std::string answer = trimWhitespace(*line);
  return answer == "y" || answer == "Y" || answer == "yes" ||
         answer == "Yes" || answer == "YES";
}

/// Prints the outcome of a failed action with process/action context.
void printActionFailure(const char *action, int pid,
                        const atm::ActionResult &result) {
  switch (result.status) {
    case atm::ActionStatus::InvalidPid:
      std::cout << atm::actionStatusMessage(atm::ActionStatus::InvalidPid)
                << '\n';
      break;
    case atm::ActionStatus::ProcessNotFound:
      std::cout << "Process does not exist.\n";
      break;
    case atm::ActionStatus::PermissionDenied:
      std::cout << "Permission denied.\n"
                   "You do not have permission to control this process.\n";
      break;
    case atm::ActionStatus::InvalidPriority:
      std::cout << "Invalid priority. Choose a value between " << kMinNice
                << " and " << kMaxNice << ".\n";
      break;
    case atm::ActionStatus::Failed:
      std::cout << "Failed to " << action << " process " << pid << ": "
                << std::strerror(result.errno_value) << '\n';
      break;
    case atm::ActionStatus::Success:
      break;
  }
}

/// Shows the current (sorted) process list for PID selection.
void showProcessSelection(const std::vector<atm::Process> &processes,
                          atm::ProcessSort sort) {
  std::cout << "\033[2J\033[H";
  std::cout << "========================================\n"
               "ARCH TASK MANAGER — Process Control\n"
               "========================================\n\n"
            << "Process list (sorted by " << atm::processSortName(sort)
            << "):\n\n"
            << "    PID  NAME                     CPU        RAM  STATE\n";
  for (const atm::Process &process : processes) {
    std::cout << "  " << formatProcessRow(process) << '\n';
  }
  std::cout << std::flush;
}

/// Result of a successful PID selection for the action menu.
struct SelectedProcess {
  int pid = 0;
  std::string name;
};

/// Asks for a PID with the given prompt and validates it: non-numeric / ≤ 0
/// is "Invalid PID.", PID 1 and the monitor's own PID are protected, and a
/// PID absent from `listed` is rejected. Prints each rejection message itself
/// and returns std::nullopt.
std::optional<SelectedProcess> selectPid(ConsoleInput &input,
                                         const std::vector<atm::Process> &listed,
                                         const char *prompt) {
  std::cout << "\n" << prompt << ":\n> " << std::flush;

  const std::optional<std::string> pid_line = input.readLine();
  if (!pid_line) {
    std::cout << "\nInput cancelled.\n";
    return std::nullopt;
  }
  const std::string pid_text = trimWhitespace(*pid_line);
  if (pid_text.empty()) {
    std::cout << "Cancelled.\n";
    return std::nullopt;
  }

  int pid = 0;
  if (!parseSignedInteger(pid_text, pid) || pid <= 0) {
    std::cout << "Invalid PID.\n";
    return std::nullopt;
  }
  if (pid == 1) {
    std::cout << "PID 1 is protected by the application and cannot be managed "
                 "from this interface.\n";
    return std::nullopt;
  }
  if (pid == static_cast<int>(::getpid())) {
    std::cout << "You cannot manage the Task Manager process.\n";
    return std::nullopt;
  }

  // The list is at most one second old; a process absent from it is treated
  // as gone rather than guessing at a pid that might now be a different one.
  for (const atm::Process &process : listed) {
    if (process.pid == pid) {
      return SelectedProcess{pid, process.name};
    }
  }
  std::cout << "Process does not exist (not found in the current process "
               "list).\n";
  return std::nullopt;
}

/// Runs the shared action menu for one selected process (terminate, kill,
/// pause, resume, change priority). ProcessActions performs every syscall —
/// this UI code never re-implements kill(2)/setpriority(2).
void runActionMenu(atm::ProcessActions &actions, ConsoleInput &input, int pid,
                   const std::string &name) {
  std::cout << "\nProcess:\n"
            << name << "\nPID: " << pid << "\n\n"
            << "Actions:\n"
               "[1] Terminate\n"
               "[2] Kill\n"
               "[3] Pause\n"
               "[4] Resume\n"
               "[5] Change Priority\n"
               "[6] Cancel\n\n"
            << "Select action:\n> " << std::flush;

  const std::optional<std::string> action_line = input.readLine();
  if (!action_line) {
    std::cout << "\nInput cancelled.\n";
    return;
  }
  const std::string action_text = trimWhitespace(*action_line);

  atm::ActionResult result{atm::ActionStatus::Failed, 0};
  bool executed = false;

  switch (action_text == "1" ? 1 : action_text == "2" ? 2
                       : action_text == "3"          ? 3
                       : action_text == "4"          ? 4
                       : action_text == "5"          ? 5
                       : action_text == "6"          ? 6
                                                     : 0) {
    case 1:  // Terminate (SIGTERM)
      if (confirm("Terminate process " + std::to_string(pid) + "?", input)) {
        result = actions.terminate(pid);
        if (result.success()) {
          std::cout << "Process " << pid
                    << " termination signal sent successfully.\n";
        }
        executed = true;
      } else {
        std::cout << "Cancelled.\n";
      }
      break;

    case 2:  // Kill (SIGKILL) — destructive, always requires confirmation.
      std::cout << "\nWARNING:\nYou are about to forcefully kill process:\n\n"
                << "PID: " << pid << "\nName: " << name << "\n\n";
      if (confirm("Continue?", input)) {
        result = actions.kill(pid);
        if (result.success()) {
          std::cout << "Process " << pid << " killed.\n";
        }
        executed = true;
      } else {
        std::cout << "Cancelled.\n";
      }
      break;

    case 3:  // Pause (SIGSTOP)
      if (confirm("Pause process " + std::to_string(pid) + "?", input)) {
        result = actions.pause(pid);
        if (result.success()) {
          std::cout << "Process " << pid << " paused.\n";
        }
        executed = true;
      } else {
        std::cout << "Cancelled.\n";
      }
      break;

    case 4:  // Resume (SIGCONT)
      if (confirm("Resume process " + std::to_string(pid) + "?", input)) {
        result = actions.resume(pid);
        if (result.success()) {
          std::cout << "Process " << pid << " resumed.\n";
        }
        executed = true;
      } else {
        std::cout << "Cancelled.\n";
      }
      break;

    case 5: {  // Change priority.
      const std::optional<int> current = actions.currentPriority(pid);
      if (!current.has_value()) {
        std::cout << "Process does not exist. "
                     "It may have disappeared before the operation "
                     "completed.\n";
        break;
      }
      std::cout << "\nCurrent priority: " << *current << "\n"
                << "Lower values = higher scheduling priority; "
                   "higher values = lower.\n"
                << "Raising priority beyond your limit needs root.\n\n"
                << "Enter new priority (" << kMinNice << " to " << kMaxNice
                << "):\n> " << std::flush;

      const std::optional<std::string> priority_line = input.readLine();
      if (!priority_line) {
        std::cout << "\nInput cancelled.\n";
        break;
      }
      int priority = 0;
      if (!parseSignedInteger(trimWhitespace(*priority_line), priority) ||
          priority < kMinNice || priority > kMaxNice) {
        std::cout << "Invalid priority. Choose a value between " << kMinNice
                  << " and " << kMaxNice << ".\n";
        break;
      }
      result = actions.setPriority(pid, priority);
      if (result.success()) {
        std::cout << "Process " << pid << " priority set to " << priority
                  << ".\n";
      }
      executed = true;
      break;
    }

    case 6:
      std::cout << "Cancelled.\n";
      break;

    default:
      std::cout << "Invalid action.\n";
      break;
  }

  if (executed && !result.success()) {
    printActionFailure(
        action_text == "1" ? "terminate"
            : action_text == "2" ? "kill"
            : action_text == "3" ? "pause"
            : action_text == "5" ? "set priority for"
                                  : "resume",
        pid, result);
  }

  std::cout << "\nRefreshing process list...\n";
  // Give the user a moment to read the outcome before the next frame clears
  // the screen.
  std::this_thread::sleep_for(1500ms);
}

/// Runs one complete "select a process, choose an action" interaction from the
/// flat process-list view.
void runProcessControl(atm::ProcessActions &actions, ConsoleInput &input,
                       const std::vector<atm::Process> &listed,
                       atm::ProcessSort sort) {
  showProcessSelection(listed, sort);
  const auto selected =
      selectPid(input, listed, "Select PID (blank to cancel)");
  if (selected.has_value()) {
    runActionMenu(actions, input, selected->pid, selected->name);
  }
}

/// Manages a process chosen from the tree view. The tree only identifies the
/// selected PID; validation and the action menu are the same shared flow used
/// by the flat table, and ProcessActions performs the actual syscalls.
void manageFromTree(atm::ProcessActions &actions, ConsoleInput &input,
                    const atm::ProcessTree &tree,
                    const std::vector<atm::Process> &listed) {
  std::cout << "\033[2J\033[H";
  std::cout << "========================================\n"
               "ARCH TASK MANAGER — Process Tree Control\n"
               "========================================\n\n"
            << atm::renderProcessTree(tree) << "\n\n";

  const auto selected = selectPid(input, listed, "Enter PID to manage (blank to cancel)");
  if (selected.has_value()) {
    runActionMenu(actions, input, selected->pid, selected->name);
  }
}

}  // namespace

int main() {
  atm::CpuMonitor cpu_monitor;
  atm::MemoryMonitor memory_monitor;
  atm::ProcessMonitor process_monitor;
  atm::ProcessActions actions;
  ConsoleInput input;

  atm::ProcessSort sort = atm::ProcessSort::Cpu;
  ViewMode view = ViewMode::List;

  // Choose the starting view. EOF (e.g. /dev/null stdin) defaults to List.
  std::cout << "Select view:\n"
               "[1] Process List\n"
               "[2] Process Tree\n\n"
               "Enter a number, or press Enter for the default (Process List):\n> "
            << std::flush;
  const std::optional<std::string> view_choice = input.readLine();
  if (view_choice && trimWhitespace(*view_choice) == "2") {
    view = ViewMode::Tree;
  }

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
  atm::ProcessTree tree = atm::buildProcessTree(snapshot.processes);
  renderView(*first_cpu, *first_memory, snapshot.processes, snapshot.stats,
             sort, view, tree);

  for (;;) {
    std::this_thread::sleep_for(kRefreshInterval);

    switch (input.pollCommand()) {
      case ConsoleInput::Command::SortCpu:
        sort = atm::ProcessSort::Cpu;
        break;
      case ConsoleInput::Command::SortMemory:
        sort = atm::ProcessSort::Memory;
        break;
      case ConsoleInput::Command::SortPid:
        sort = atm::ProcessSort::Pid;
        break;
      case ConsoleInput::Command::SortName:
        sort = atm::ProcessSort::Name;
        break;
      case ConsoleInput::Command::ViewList:
        view = ViewMode::List;
        break;
      case ConsoleInput::Command::ViewTree:
        view = ViewMode::Tree;
        break;
      case ConsoleInput::Command::Manage:
        if (view == ViewMode::Tree) {
          manageFromTree(actions, input, tree, snapshot.processes);
        } else {
          runProcessControl(actions, input, snapshot.processes, sort);
        }
        // Refresh immediately so the effect of the action is visible without
        // waiting for the next 1 s tick.
        {
          const auto cpu = cpu_monitor.readUsage();
          const auto memory = memory_monitor.read();
          if (cpu.has_value() && memory.has_value()) {
            snapshot = process_monitor.read(memory->total);
            atm::sortProcesses(snapshot.processes, sort);
            tree = atm::buildProcessTree(snapshot.processes);
            renderView(*cpu, *memory, snapshot.processes, snapshot.stats,
                       sort, view, tree);
          }
        }
        continue;
      case ConsoleInput::Command::None:
        break;
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

    snapshot = process_monitor.read(memory->total);
    atm::sortProcesses(snapshot.processes, sort);
    tree = atm::buildProcessTree(snapshot.processes);
    renderView(*cpu, *memory, snapshot.processes, snapshot.stats, sort, view,
               tree);
  }
}