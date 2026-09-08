#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <string>
#include <vector>

namespace atm {

/// A timestamped sample for time-series data.
struct TimedSample {
  std::chrono::steady_clock::time_point timestamp;
  double value;
};

/// Fixed-size circular/ring buffer for time-series data.
/// When the maximum number of samples is reached, the oldest sample is removed.
/// Memory usage is strictly bounded.
template <typename T>
class ResourceHistory {
 public:
  explicit ResourceHistory(std::size_t max_samples)
      : max_samples_(max_samples) {}

  void addSample(const T& sample) {
    if (buffer_.size() >= max_samples_) {
      buffer_.pop_front();
    }
    buffer_.push_back(sample);
  }

  const std::deque<T>& samples() const { return buffer_; }

  void clear() { buffer_.clear(); }

  std::size_t size() const { return buffer_.size(); }

  std::size_t maxSamples() const { return max_samples_; }

  bool empty() const { return buffer_.empty(); }

 private:
  std::size_t max_samples_;
  std::deque<T> buffer_;
};

/// Configuration for graph rendering.
struct GraphConfig {
  std::size_t width = 40;
  std::size_t height = 8;
  bool dynamic_scale = false;
};

/// Renders a time-series graph from ResourceHistory<TimedSample>.
class GraphRenderer {
 public:
  /// Renders a graph as a multi-line string ready for terminal output.
  static std::string renderText(const ResourceHistory<TimedSample>& history,
                                const GraphConfig& config,
                                const std::string& label,
                                const std::string& unit);

  /// Formats a rate value (bytes/sec) into a human-readable string.
  static std::string formatRate(double bytes_per_second);

 private:
  static double roundScale(double value);
};

}  // namespace atm
