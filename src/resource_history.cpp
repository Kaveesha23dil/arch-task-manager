#include "resource_history.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace atm {

double GraphRenderer::roundScale(double value) {
  if (value <= 0.0) {
    return 0.0;
  }
  const double magnitude = std::pow(10.0, std::floor(std::log10(value)));
  const double normalized = value / magnitude;
  double rounded;
  if (normalized <= 1.0) {
    rounded = 1.0;
  } else if (normalized <= 2.0) {
    rounded = 2.0;
  } else if (normalized <= 5.0) {
    rounded = 5.0;
  } else {
    rounded = 10.0;
  }
  return rounded * magnitude;
}

std::string GraphRenderer::formatRate(double bytes_per_second) {
  static constexpr std::array kSuffixes = {"B/s", "kB/s", "MB/s", "GB/s", "TB/s"};
  double value = std::abs(bytes_per_second);
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

std::string GraphRenderer::renderText(const ResourceHistory<TimedSample>& history,
                                      const GraphConfig& config,
                                      const std::string& label,
                                      const std::string& unit) {
  const std::size_t rows = config.height > 0 ? config.height : 1;
  const std::size_t cols = config.width;

  const auto& samples = history.samples();

  std::ostringstream out;

  if (history.empty()) {
    out << label << "  (no data)\n";
    for (std::size_t i = 0; i < rows; ++i) {
      out << "       |" << std::string(cols, ' ') << '\n';
    }
    out << "       +" << std::string(cols, '-') << '\n';
    return out.str();
  }

  // Extract values.
  std::vector<double> values;
  values.reserve(samples.size());
  for (const auto& sample : samples) {
    values.push_back(sample.value);
  }

  // Find min and max.
  double max_val = values[0];
  for (double v : values) {
    if (v > max_val) max_val = v;
  }

  // Determine Y-axis range.
  double y_max;
  if (config.dynamic_scale) {
    y_max = max_val <= 0.0 ? 1.0 : roundScale(max_val * 1.15);
    if (y_max <= 0.0) y_max = 1.0;
  } else {
    y_max = 100.0;
  }

  // Header with current value.
  std::ostringstream header_out;
  header_out << std::fixed << std::setprecision(1) << values.back() << ' '
             << unit;
  const std::string current_label = header_out.str();

  // Y-axis label width.
  std::string y_top_label, y_bottom_label;
  {
    std::ostringstream top;
    if (config.dynamic_scale) {
      top << std::fixed << std::setprecision(y_max >= 100.0 ? 0 : 1) << y_max
          << ' ' << unit;
    } else {
      top << std::fixed << std::setprecision(0) << y_max << ' ' << unit;
    }
    y_top_label = top.str();

    std::ostringstream bot;
    bot << std::fixed << std::setprecision(0) << 0.0 << ' ' << unit;
    y_bottom_label = bot.str();
  }
  const std::size_t label_w = std::max(y_top_label.size(), y_bottom_label.size());

  if (label.empty()) {
    out << current_label << '\n';
  } else {
    out << label << "  " << current_label << '\n';
  }

  // Initialize grid with spaces.
  std::vector<std::vector<char>> grid(rows, std::vector<char>(cols, ' '));

  const double range = y_max;  // y_min = 0

  // Map samples to columns.
  for (std::size_t col = 0; col < cols; ++col) {
    std::size_t sample_idx;
    if (cols == 1) {
      sample_idx = values.size() - 1;
    } else if (values.size() <= cols) {
      sample_idx = col * (values.size() - 1) / (cols - 1);
    } else {
      sample_idx = col * (values.size() - 1) / (cols - 1);
    }
    if (sample_idx >= values.size()) {
      sample_idx = values.size() - 1;
    }

    const double val = values[sample_idx];
    double normalized;
    if (range <= 0.0) {
      normalized = 1.0;
    } else {
      normalized = std::clamp(val / range, 0.0, 1.0);
    }

    const std::size_t row_from_top =
        static_cast<std::size_t>(normalized * static_cast<double>(rows - 1));
    const std::size_t clamped = std::min(row_from_top, rows - 1);
    const std::size_t bottom_row = rows - 1 - clamped;

    // Draw from the bottom up to the mapped row. The `r < rows` guard prevents
    // the unsigned counter from wrapping to a huge value when r reaches 0.
    for (std::size_t r = rows - 1; r >= bottom_row && r < rows; --r) {
      if (r == bottom_row) {
        grid[r][col] = '~';
      } else if (grid[r][col] == ' ') {
        grid[r][col] = '.';
      }
    }
  }

  // Render the grid.
  for (std::size_t i = 0; i < rows; ++i) {
    std::string y_label;
    if (i == 0) {
      y_label = y_top_label;
      if (y_label.size() < label_w) {
        y_label.insert(y_label.begin(), label_w - y_label.size(), ' ');
      }
    } else if (i + 1 == rows) {
      y_label = y_bottom_label;
      if (y_label.size() < label_w) {
        y_label.insert(y_label.begin(), label_w - y_label.size(), ' ');
      }
    } else {
      y_label = std::string(label_w, ' ');
    }
    out << y_label << " |";
    for (std::size_t c = 0; c < cols; ++c) {
      out << grid[i][c];
    }
    out << '\n';
  }

  // Rule line.
  out << std::string(label_w, ' ') << " +" << std::string(cols, '-') << '\n';

  // Footer with time range.
  if (samples.size() >= 2) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        samples.back().timestamp - samples.front().timestamp);
    out << std::string(label_w, ' ') << "  " << elapsed.count() << "s        Now\n";
  } else {
    out << std::string(label_w, ' ') << "  Now\n";
  }

  return out.str();
}

}  // namespace atm
