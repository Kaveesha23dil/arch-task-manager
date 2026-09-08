#pragma once

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

namespace atm {

/// Minimal, thread-safe logger. Writes timestamped lines to stderr so the
/// terminal UI is never polluted. Messages must never contain secrets (the
/// configuration subsystem does not store any).
class Logger {
 public:
  enum class Level { Info, Warn, Error };

  /// Writes one line ("[YYYY-MM-DD HH:MM:SS] LEVEL  message") to stderr.
  static void log(Level level, const std::string &prefix,
                  const std::string &message);

  static void info(const std::string &message) {
    log(Level::Info, "arch-task-manager", message);
  }

  static void warn(const std::string &message) {
    log(Level::Warn, "arch-task-manager", message);
  }

  static void error(const std::string &message) {
    log(Level::Error, "arch-task-manager", message);
  }
};

}  // namespace atm