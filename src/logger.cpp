#include "logger.hpp"

#include <ctime>

namespace atm {

void Logger::log(Level level, const std::string &prefix,
                 const std::string &message) {
  static std::mutex mutex;
  std::lock_guard<std::mutex> lock(mutex);

  const char *level_text = "INFO";
  switch (level) {
    case Level::Info:  level_text = "INFO";  break;
    case Level::Warn:  level_text = "WARN";  break;
    case Level::Error: level_text = "ERROR"; break;
  }

  std::time_t now = std::time(nullptr);
  std::tm tm_buf{};
  if (localtime_r(&now, &tm_buf) != nullptr) {
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_buf);
    std::fprintf(stderr, "[%s] %-5s %s: %s\n", stamp, level_text,
                 prefix.c_str(), message.c_str());
  } else {
    std::fprintf(stderr, "[unavailable timestamp] %-5s %s: %s\n", level_text,
                 prefix.c_str(), message.c_str());
  }
  std::fflush(stderr);
}

}  // namespace atm