#include "network_traffic_viz.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace atm {

namespace {

using Clock = std::chrono::steady_clock;

constexpr char kRxPeakGlyph = '~';
constexpr char kRxFillGlyph = '.';
constexpr char kTxPeakGlyph = '^';
constexpr char kTxFillGlyph = ':';
constexpr char kOverlapPeakGlyph = '#';
constexpr char kOverlapFillGlyph = '+';

// Wireless signal chart glyphs (same peak/flow conventions as the traffic
// chart; a blank column is the honest way to draw a gap — never interpolated).
constexpr char kSignalPeakGlyph = '~';
constexpr char kSignalFillGlyph = '.';

/// Same 1/2/5 rounding the rest of the app uses to keep y-axis labels
/// human-friendly (mirrors GraphRenderer::roundScale).
double roundScale(double value) {
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

/// Seconds of one timestamp relative to a shared origin. Both series are
/// measured from the same origin so the two time axes line up exactly.
double toSeconds(const Clock::time_point &timestamp,
                 const Clock::time_point &origin) {
  return std::chrono::duration<double>(timestamp - origin).count();
}

/// Value of a rate ring at a target time on the shared x-axis, using the
/// nearest recorded sample (peaks are preserved instead of being flattened by
/// interpolation). O(log n) per lookup via binary search.
double valueAt(const std::vector<double> &times, const std::vector<double> &values,
               double target) {
  if (times.empty()) {
    return 0.0;
  }
  if (times.size() == 1) {
    return values.front();
  }
  if (target <= times.front()) {
    return values.front();
  }
  if (target >= times.back()) {
    return values.back();
  }
  const auto it = std::upper_bound(times.begin(), times.end(), target);
  const std::size_t hi = static_cast<std::size_t>(it - times.begin());
  const std::size_t lo = hi - 1;
  return times[hi] - target < target - times[lo] ? values[hi] : values[lo];
}

/// "-3s", "-1m30s", "-2h05m" style relative span for the x-axis footers.
std::string formatRelativeSpan(double seconds) {
  long long total = static_cast<long long>(std::llround(seconds));
  if (total < 1) {
    return "<1s";
  }
  const long long hours = total / 3600;
  const long long minutes = (total % 3600) / 60;
  const long long secs = total % 60;
  std::ostringstream out;
  out << '-';
  if (hours > 0) {
    out << hours << 'h' << std::setfill('0') << std::setw(2) << minutes << 'm'
        << std::setfill(' ') << std::setw(2) << secs << 's';
  } else if (minutes > 0) {
    out << minutes << 'm' << std::setfill('0') << std::setw(2) << secs << 's'
        << std::setfill(' ');
  } else {
    out << secs << 's';
  }
  return out.str();
}

/// dBm y-axis label: integral values print without a decimal point, fractional
/// ones with a single digit. Never locale-dependent.
std::string formatDbmLabel(double value) {
  std::ostringstream out;
  if (std::trunc(value) == value) {
    out << static_cast<long long>(value);
  } else {
    out << std::fixed << std::setprecision(1) << value;
  }
  return out.str();
}

}  // namespace

NetworkTrafficSummary summarizeNetworkTrafficSeries(
    const NetworkTrafficSeries &series) {
  NetworkTrafficSummary summary;

  const auto currentAndPeak = [](const ResourceHistory<TimedSample> &ring,
                                 double &current, double &peak) {
    for (const TimedSample &sample : ring.samples()) {
      current = sample.value;
      peak = std::max(peak, sample.value);
    }
  };
  currentAndPeak(series.rx_bytes_per_second, summary.rx_current,
                 summary.rx_peak);
  currentAndPeak(series.tx_bytes_per_second, summary.tx_current,
                 summary.tx_peak);
  summary.has_rate_data = !series.rx_bytes_per_second.empty() ||
                          !series.tx_bytes_per_second.empty();

  // Total transferred during the retained window (last - first cumulative
  // sample). A counter reset inside the window makes the delta negative and is
  // reported as unavailable — never a bogus value.
  const auto windowTotal = [](const ResourceHistory<TimedSample> &ring)
      -> std::optional<double> {
    if (ring.size() < 2) {
      return std::nullopt;
    }
    const double delta = ring.samples().back().value - ring.samples().front().value;
    return delta >= 0.0 ? std::optional<double>(delta) : std::nullopt;
  };
  summary.rx_window_total = windowTotal(series.rx_bytes_total);
  summary.tx_window_total = windowTotal(series.tx_bytes_total);

  summary.sample_count = series.rx_bytes_total.size();
  if (series.rx_bytes_total.size() >= 2) {
    summary.span_seconds = std::chrono::duration<double>(
        series.rx_bytes_total.samples().back().timestamp -
        series.rx_bytes_total.samples().front().timestamp)
                               .count();
  }
  summary.last_update = series.last_update;
  summary.aggregate = series.aggregate;
  summary.membership_changed = series.membership_changed;
  return summary;
}

std::string renderNetworkTrafficChart(
    const ResourceHistory<TimedSample> &rx_rates,
    const ResourceHistory<TimedSample> &tx_rates,
    const NetworkTrafficChartConfig &config) {
  const std::size_t cols = config.data_width > 0 ? config.data_width : 1;
  const std::size_t rows = config.data_height > 0 ? config.data_height : 1;

  const auto &rx_samples = rx_rates.samples();
  const auto &tx_samples = tx_rates.samples();

  std::ostringstream out;
  if (rx_samples.empty() && tx_samples.empty()) {
    out << "RX/TX (no data)\n";
    return out.str();
  }

  // Compact the rings into per-series time/value arrays relative to a shared
  // origin (the earliest sample of either series) so both line up on one axis.
  Clock::time_point origin = tx_samples.empty() ? rx_samples.front().timestamp
                                                : tx_samples.front().timestamp;
  if (!rx_samples.empty() && rx_samples.front().timestamp < origin) {
    origin = rx_samples.front().timestamp;
  }

  std::vector<double> rx_times, rx_values, tx_times, tx_values;
  rx_times.reserve(rx_samples.size());
  rx_values.reserve(rx_samples.size());
  for (const TimedSample &sample : rx_samples) {
    rx_times.push_back(toSeconds(sample.timestamp, origin));
    rx_values.push_back(sample.value);
  }
  tx_times.reserve(tx_samples.size());
  tx_values.reserve(tx_samples.size());
  for (const TimedSample &sample : tx_samples) {
    tx_times.push_back(toSeconds(sample.timestamp, origin));
    tx_values.push_back(sample.value);
  }

  const double window_end =
      std::max(rx_times.empty() ? 0.0 : rx_times.back(),
               tx_times.empty() ? 0.0 : tx_times.back());
  const double span = std::max(window_end, 0.0);  // >= 0, 0 when one sample

  // Map each column to a target time (column center) and read both series.
  std::vector<double> rx_col(cols, 0.0);
  std::vector<double> tx_col(cols, 0.0);
  double plot_peak = 0.0;
  for (std::size_t c = 0; c < cols; ++c) {
    const double target =
        cols == 1 ? 0.0 : span * (static_cast<double>(c) + 0.5) /
                                  static_cast<double>(cols);
    rx_col[c] = valueAt(rx_times, rx_values, target);
    tx_col[c] = valueAt(tx_times, tx_values, target);
    plot_peak = std::max(plot_peak, std::max(rx_col[c], tx_col[c]));
  }

  // Shared dynamic y-axis so RX and TX are directly comparable.
  const double y_max = roundScale(plot_peak * 1.15);
  const double range = y_max > 0.0 ? y_max : 1.0;

  // Per-column boundary row (row index grows downward; row[height-1] is the
  // baseline). A series occupies rows [boundary .. baseline]; nullopt = empty.
  const auto boundaryRow = [rows, range](double value) -> std::optional<std::size_t> {
    double normalized = std::clamp(value / range, 0.0, 1.0);
    std::size_t from_top = static_cast<std::size_t>(
        std::llround(normalized * static_cast<double>(rows - 1)));
    from_top = std::min(from_top, rows - 1);
    return rows - 1 - from_top;
  };
  std::vector<std::optional<std::size_t>> rx_boundary(cols);
  std::vector<std::optional<std::size_t>> tx_boundary(cols);
  for (std::size_t c = 0; c < cols; ++c) {
    rx_boundary[c] = rx_times.empty() ? std::nullopt : boundaryRow(rx_col[c]);
    tx_boundary[c] = tx_times.empty() ? std::nullopt : boundaryRow(tx_col[c]);
  }

  // Legend.
  out << "RX peak "<< kRxPeakGlyph << " flow " << kRxFillGlyph
      << "   TX peak " << kTxPeakGlyph << " flow " << kTxFillGlyph
      << "   overlap " << kOverlapPeakGlyph << '/' << kOverlapFillGlyph
      << "   (same scale, B/s)\n";

  // Y-axis labels.
  std::string top_label = GraphRenderer::formatRate(range);
  const std::string mid_label = GraphRenderer::formatRate(range / 2.0);
  const std::string bottom_label = GraphRenderer::formatRate(0.0);
  const std::size_t label_w =
      std::max({top_label.size(), mid_label.size(), bottom_label.size()});

  auto paddedLabel = [label_w](const std::string &text) {
    return text.size() < label_w ? std::string(label_w - text.size(), ' ') + text
                                 : text;
  };

  // Plot grid.
  std::vector<std::vector<char>> grid(rows, std::vector<char>(cols, ' '));
  for (std::size_t c = 0; c < cols; ++c) {
    for (std::size_t r = 0; r < rows; ++r) {
      const bool rx_here = rx_boundary[c].has_value() && r >= *rx_boundary[c];
      const bool tx_here = tx_boundary[c].has_value() && r >= *tx_boundary[c];
      const bool rx_edge = rx_boundary[c].has_value() && r == *rx_boundary[c];
      const bool tx_edge = tx_boundary[c].has_value() && r == *tx_boundary[c];
      if (rx_here && tx_here) {
        if (rx_edge && tx_edge) {
          grid[r][c] = kOverlapPeakGlyph;
        } else if (rx_edge) {
          grid[r][c] = kRxPeakGlyph;
        } else if (tx_edge) {
          grid[r][c] = kTxPeakGlyph;
        } else {
          grid[r][c] = kOverlapFillGlyph;
        }
      } else if (rx_here) {
        grid[r][c] = rx_edge ? kRxPeakGlyph : kRxFillGlyph;
      } else if (tx_here) {
        grid[r][c] = tx_edge ? kTxPeakGlyph : kTxFillGlyph;
      }
    }
  }

  for (std::size_t r = 0; r < rows; ++r) {
    if (r == 0) {
      out << paddedLabel(top_label) << " |";
    } else if (r + 1 == rows) {
      out << paddedLabel(bottom_label) << " |";
    } else if (r == rows / 2) {
      out << paddedLabel(mid_label) << " |";
    } else {
      out << std::string(label_w, ' ') << " |";
    }
    for (const char cell : grid[r]) {
      out << cell;
    }
    out << '\n';
  }

  // Baseline and relative time axis.
  out << std::string(label_w, ' ') << " +" << std::string(cols, '-') << '\n';

  const std::size_t now_x = label_w + 1 + cols;
  const std::string left = formatRelativeSpan(span);
  const std::string mid = formatRelativeSpan(span / 2.0);
  const std::size_t left_x = label_w + 2;
  // Place the middle label centered between the end of the left label and the
  // right "Now", clamped so the three tokens never overlap.
  std::string footer(now_x + 4, ' ');
  const std::size_t left_end = left_x + left.size();
  const std::size_t now_width = 3;
  const std::size_t mid_center = left_end + (now_x - 1 - left_end) / 2;
  std::size_t mid_x = mid.size() / 2 <= mid_center
                          ? mid_center - mid.size() / 2
                          : left_end;
  if (mid_x < left_end) {
    mid_x = left_end;
  }
  if (mid_x + mid.size() > now_x) {
    mid_x = now_x >= mid.size() && now_x - mid.size() >= left_end
                ? now_x - mid.size()
                : left_end;
  }
  footer.replace(left_x, left.size(), left);
  if (mid_x + mid.size() <= now_x) {
    footer.replace(mid_x, mid.size(), mid);
  }
  footer.replace(now_x, now_width, "Now");
  out << footer << '\n';

  return out.str();
}

std::string renderWirelessSignalChart(
    const ResourceHistory<WirelessHistorySample> &history,
    const WirelessHistoryChartConfig &config) {
  const std::size_t cols = config.data_width > 0 ? config.data_width : 1;
  const std::size_t rows = config.data_height > 0 ? config.data_height : 1;

  const auto &samples = history.samples();
  if (samples.empty()) {
    return "Signal (dBm)  (no data)\n";
  }

  // Only samples that actually carried a signal value plot anything; a gap
  // (unavailable/stale tick, valid == false) contributes nothing and is never
  // interpolated into a misleading line or a fabricated reading.
  std::optional<double> data_min, data_max;
  for (const WirelessHistorySample &sample : samples) {
    if (sample.signal_dbm.has_value()) {
      const double value = *sample.signal_dbm;
      if (!data_min.has_value() || value < *data_min) {
        data_min = value;
      }
      if (!data_max.has_value() || value > *data_max) {
        data_max = value;
      }
    }
  }
  if (!data_min.has_value()) {
    return "Signal (dBm)  (no data)\n";
  }

  // The whole retained span is the x-axis, so gaps at the edges stay honest.
  const Clock::time_point origin = samples.front().timestamp;
  const double span =
      std::max(0.0, toSeconds(samples.back().timestamp, origin));

  // One value per column: the LAST measurement whose timestamp falls inside the
  // column's time window; a column with none stays blank.
  std::vector<std::optional<double>> column(cols);
  if (span <= 0.0) {
    for (auto it = samples.rbegin(); it != samples.rend(); ++it) {
      if (it->signal_dbm.has_value()) {
        column.back() = *it->signal_dbm;
        break;
      }
    }
  } else {
    for (std::size_t c = 0; c < cols; ++c) {
      const double w_start = span * static_cast<double>(c) /
                             static_cast<double>(cols);
      const double w_end = span * static_cast<double>(c + 1) /
                           static_cast<double>(cols);
      for (auto it = samples.rbegin(); it != samples.rend(); ++it) {
        if (!it->signal_dbm.has_value()) {
          continue;
        }
        const double time = toSeconds(it->timestamp, origin);
        if (time >= w_start && time <= w_end) {
          column[c] = *it->signal_dbm;
          break;
        }
      }
    }
  }

  // Padded, human-friendly dBm scale: negative signals are kept negative, the
  // padding (>= 3 dBm) keeps a single reading from begging for a huge scale,
  // and the step snaps to the app's usual 1/2/5 series.
  double lo = *data_min;
  double hi = *data_max;
  const double pad = std::max(3.0, (hi - lo) * 0.10);
  lo -= pad;
  hi += pad;
  double step = roundScale(
      (hi - lo) /
      static_cast<double>(rows > 1 ? rows - 1 : 1));
  if (step <= 0.0) {
    step = 1.0;
  }
  lo = std::floor(lo / step) * step;
  hi = std::ceil(hi / step) * step;
  if (hi <= lo) {
    hi = lo + step;
  }
  const double range = hi - lo;
  const double mid_value = (lo + hi) / 2.0;

  // Per-column boundary row: rows [boundary .. baseline] are filled; a blank
  // column has no boundary at all.
  const auto boundaryRow = [rows, lo, range](double value) -> std::size_t {
    const double normalized =
        std::clamp((value - lo) / range, 0.0, 1.0);
    std::size_t from_top = static_cast<std::size_t>(
        std::llround(normalized * static_cast<double>(rows - 1)));
    from_top = std::min(from_top, rows - 1);
    return rows - 1 - from_top;
  };
  std::vector<std::optional<std::size_t>> boundary(cols);
  for (std::size_t c = 0; c < cols; ++c) {
    if (column[c].has_value()) {
      boundary[c] = boundaryRow(*column[c]);
    }
  }

  std::ostringstream out;
  out << "Signal dBm  peak " << kSignalPeakGlyph << " flow " << kSignalFillGlyph
      << "  (blank column = no measurement in that window)\n";

  const std::string top_label = formatDbmLabel(hi);
  const std::string mid_label = formatDbmLabel(mid_value);
  const std::string bottom_label = formatDbmLabel(lo);
  const std::size_t label_w =
      std::max({top_label.size(), mid_label.size(), bottom_label.size()});
  const auto paddedLabel = [label_w](const std::string &text) {
    return text.size() < label_w ? std::string(label_w - text.size(), ' ') + text
                                 : text;
  };

  // Plot grid. A blank column (no measurement) is all spaces — never a flat
  // line invented across a gap.
  std::vector<std::vector<char>> grid(rows, std::vector<char>(cols, ' '));
  for (std::size_t c = 0; c < cols; ++c) {
    if (!boundary[c].has_value()) {
      continue;
    }
    for (std::size_t r = *boundary[c]; r < rows; ++r) {
      grid[r][c] = r == *boundary[c] ? kSignalPeakGlyph : kSignalFillGlyph;
    }
  }

  for (std::size_t r = 0; r < rows; ++r) {
    if (r == 0) {
      out << paddedLabel(top_label) << " |";
    } else if (r + 1 == rows) {
      out << paddedLabel(bottom_label) << " |";
    } else if (r == rows / 2) {
      out << paddedLabel(mid_label) << " |";
    } else {
      out << std::string(label_w, ' ') << " |";
    }
    for (const char cell : grid[r]) {
      out << cell;
    }
    out << '\n';
  }

  // Baseline and relative time axis (same layout as the traffic chart).
  out << std::string(label_w, ' ') << " +" << std::string(cols, '-') << '\n';

  const std::size_t now_x = label_w + 1 + cols;
  const std::string left = formatRelativeSpan(span);
  const std::string mid = formatRelativeSpan(span / 2.0);
  const std::size_t left_x = label_w + 2;
  std::string footer(now_x + 4, ' ');
  const std::size_t left_end = left_x + left.size();
  const std::size_t now_width = 3;
  const std::size_t mid_center = left_end + (now_x - 1 - left_end) / 2;
  std::size_t mid_x = mid.size() / 2 <= mid_center
                          ? mid_center - mid.size() / 2
                          : left_end;
  if (mid_x < left_end) {
    mid_x = left_end;
  }
  if (mid_x + mid.size() > now_x) {
    mid_x = now_x >= mid.size() && now_x - mid.size() >= left_end
                ? now_x - mid.size()
                : left_end;
  }
  footer.replace(left_x, left.size(), left);
  if (mid_x + mid.size() <= now_x) {
    footer.replace(mid_x, mid.size(), mid);
  }
  footer.replace(now_x, now_width, "Now");
  out << footer << '\n';

  return out.str();
}

}  // namespace atm