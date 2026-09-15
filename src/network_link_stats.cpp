#include "network_link_stats.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace atm {

const char *networkLinkStatMetricName(NetworkLinkStatMetric metric) {
  static const char *const kNames[kNetworkLinkStatMetricCount] = {
      "rx_errors",         "tx_errors",       "rx_dropped",
      "tx_dropped",        "rx_missed_errors", "tx_fifo_errors",
      "rx_fifo_errors",    "collisions",      "rx_over_errors",
      "rx_frame_errors",   "rx_length_errors", "rx_crc_errors",
      "rx_compressed",     "tx_compressed",   "tx_carrier_errors",
      "tx_heartbeat_errors", "tx_window_errors", "multicast",
      "rx_nohandler",
  };
  const std::size_t index = static_cast<std::size_t>(metric);
  if (index >= kNetworkLinkStatMetricCount) {
    return "unknown";
  }
  return kNames[index];
}

const char *networkLinkStatStateName(NetworkLinkStatState state) {
  switch (state) {
    case NetworkLinkStatState::Unknown:
      return "unknown";
    case NetworkLinkStatState::Valid:
      return "valid";
    case NetworkLinkStatState::Stale:
      return "stale";
    case NetworkLinkStatState::Unavailable:
      return "unavailable";
  }
  return "unknown";
}

namespace {

std::string trimWhitespace(const std::string &text) {
  std::size_t begin = 0;
  while (begin < text.size() &&
         (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r')) {
    ++begin;
  }
  std::size_t end = text.size();
  while (end > begin &&
         (text[end - 1] == ' ' || text[end - 1] == '\t' ||
          text[end - 1] == '\r' || text[end - 1] == '\n')) {
    --end;
  }
  return text.substr(begin, end - begin);
}

bool parseCounter(const std::string &text, std::uint64_t &value) {
  const std::string trimmed = trimWhitespace(text);
  if (trimmed.empty()) {
    return false;
  }
  constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t parsed = 0;
  for (const char c : trimmed) {
    if (c < '0' || c > '9') {
      return false;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (parsed > (kMax - digit) / 10) {
      return false;
    }
    parsed = parsed * 10 + digit;
  }
  value = parsed;
  return true;
}

std::optional<std::uint64_t> readCounterFile(
    const std::filesystem::path &file) {
  std::ifstream in(file);
  if (!in) {
    return std::nullopt;
  }
  std::string text;
  if (!std::getline(in, text)) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  if (!parseCounter(text, value)) {
    return std::nullopt;
  }
  return value;
}

std::optional<double> counterRate(
    const std::optional<std::uint64_t> &previous,
    const std::optional<std::uint64_t> &current, double seconds) {
  if (!previous.has_value() || !current.has_value()) {
    return std::nullopt;
  }
  if (*current < *previous) {
    return std::nullopt;
  }
  const double delta = static_cast<double>(*current - *previous);
  return delta / seconds;
}

const std::optional<double> &optionalRate(
    NetworkLinkStatMetric metric, const NetworkLinkStatRates &rates) {
  switch (metric) {
    case NetworkLinkStatMetric::RxErrors:
      return rates.rx_errors_per_second;
    case NetworkLinkStatMetric::TxErrors:
      return rates.tx_errors_per_second;
    case NetworkLinkStatMetric::RxDropped:
      return rates.rx_dropped_per_second;
    case NetworkLinkStatMetric::TxDropped:
      return rates.tx_dropped_per_second;
    case NetworkLinkStatMetric::RxMissedErrors:
      return rates.rx_missed_errors_per_second;
    case NetworkLinkStatMetric::TxFifoErrors:
      return rates.tx_fifo_errors_per_second;
    case NetworkLinkStatMetric::RxFifoErrors:
      return rates.rx_fifo_errors_per_second;
    case NetworkLinkStatMetric::Collisions:
      return rates.collisions_per_second;
    case NetworkLinkStatMetric::RxCrcErrors:
      return rates.rx_crc_errors_per_second;
    case NetworkLinkStatMetric::RxFrameErrors:
      return rates.rx_frame_errors_per_second;
    case NetworkLinkStatMetric::TxCarrierErrors:
      return rates.tx_carrier_errors_per_second;
    default:
      break;
  }
  static const std::optional<double> kNoRate;
  return kNoRate;
}

std::string formatTimestamp(std::chrono::system_clock::time_point tp) {
  if (tp == std::chrono::system_clock::time_point{}) {
    return "N/A";
  }
  const std::time_t time = std::chrono::system_clock::to_time_t(tp);
  std::tm local{};
  if (::localtime_r(&time, &local) == nullptr) {
    return "N/A";
  }
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local);
  return buffer;
}

std::string padRight(const std::string &text, std::size_t width) {
  return text.size() < width ? text + std::string(width - text.size(), ' ')
                             : text;
}

std::string padLeft(const std::string &text, std::size_t width) {
  return text.size() < width ? std::string(width - text.size(), ' ') + text
                             : text;
}

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

std::string formatScaleLabel(double value) {
  std::ostringstream out;
  if (std::trunc(value) == value) {
    out << static_cast<long long>(value);
  } else {
    out << std::fixed << std::setprecision(1) << value;
  }
  return out.str();
}

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

}  // namespace

NetworkLinkStatRead readNetworkLinkStats(const std::filesystem::path &root,
                                         const std::string &iface) {
  NetworkLinkStatRead result;
  const std::filesystem::path statistics =
      root / "sys" / "class" / "net" / iface / "statistics";

  std::error_code ec;
  const std::filesystem::file_status status =
      std::filesystem::status(statistics, ec);
  if (ec) {
    if (ec == std::errc::permission_denied) {
      result.access_denied = true;
    }
    result.read_ok = false;
    return result;
  }
  if (!std::filesystem::exists(status) ||
      !std::filesystem::is_directory(status)) {
    result.read_ok = false;
    return result;
  }

  std::error_code list_ec;
  std::filesystem::directory_iterator(statistics, list_ec);
  if (list_ec) {
    if (list_ec == std::errc::permission_denied) {
      result.access_denied = true;
    }
    result.read_ok = false;
    return result;
  }

  bool any_available = false;
  for (std::size_t i = 0; i < kNetworkLinkStatMetricCount; ++i) {
    const std::optional<std::uint64_t> value =
        readCounterFile(statistics / networkLinkStatMetricName(
                                         static_cast<NetworkLinkStatMetric>(i)));
    if (value.has_value()) {
      any_available = true;
      result.counters[i] = value;
    }
  }
  result.read_ok = true;
  result.any_available = any_available;
  return result;
}

NetworkLinkStatRates deriveNetworkLinkStatRates(
    const NetworkLinkStatSample &previous, const NetworkLinkStatSample &current,
    double seconds) {
  NetworkLinkStatRates rates;
  if (!previous.valid || !current.valid) {
    return rates;
  }
  if (!(seconds > 0.0) || !std::isfinite(seconds)) {
    return rates;
  }

  const auto rate = [&](std::size_t metric) {
    return counterRate(previous.counters[metric], current.counters[metric],
                       seconds);
  };
  const auto combined = [&](std::size_t a,
                            std::size_t b) -> std::optional<double> {
    const std::optional<double> first = counterRate(
        previous.counters[a], current.counters[a], seconds);
    const std::optional<double> second = counterRate(
        previous.counters[b], current.counters[b], seconds);
    if (!first.has_value() || !second.has_value()) {
      return std::nullopt;
    }
    return *first + *second;
  };

  const std::size_t rx_errors =
      static_cast<std::size_t>(NetworkLinkStatMetric::RxErrors);
  const std::size_t tx_errors =
      static_cast<std::size_t>(NetworkLinkStatMetric::TxErrors);
  const std::size_t rx_dropped =
      static_cast<std::size_t>(NetworkLinkStatMetric::RxDropped);
  const std::size_t tx_dropped =
      static_cast<std::size_t>(NetworkLinkStatMetric::TxDropped);
  const std::size_t rx_missed =
      static_cast<std::size_t>(NetworkLinkStatMetric::RxMissedErrors);
  const std::size_t tx_fifo =
      static_cast<std::size_t>(NetworkLinkStatMetric::TxFifoErrors);
  const std::size_t rx_fifo =
      static_cast<std::size_t>(NetworkLinkStatMetric::RxFifoErrors);
  const std::size_t collisions =
      static_cast<std::size_t>(NetworkLinkStatMetric::Collisions);
  const std::size_t rx_crc =
      static_cast<std::size_t>(NetworkLinkStatMetric::RxCrcErrors);
  const std::size_t rx_frame =
      static_cast<std::size_t>(NetworkLinkStatMetric::RxFrameErrors);
  const std::size_t tx_carrier =
      static_cast<std::size_t>(NetworkLinkStatMetric::TxCarrierErrors);

  rates.rx_errors_per_second = rate(rx_errors);
  rates.tx_errors_per_second = rate(tx_errors);
  rates.rx_dropped_per_second = rate(rx_dropped);
  rates.tx_dropped_per_second = rate(tx_dropped);
  rates.combined_errors_per_second = combined(rx_errors, tx_errors);
  rates.combined_drops_per_second = combined(rx_dropped, tx_dropped);
  rates.collisions_per_second = rate(collisions);
  rates.rx_missed_errors_per_second = rate(rx_missed);
  rates.rx_fifo_errors_per_second = rate(rx_fifo);
  rates.tx_fifo_errors_per_second = rate(tx_fifo);
  rates.rx_crc_errors_per_second = rate(rx_crc);
  rates.rx_frame_errors_per_second = rate(rx_frame);
  rates.tx_carrier_errors_per_second = rate(tx_carrier);
  return rates;
}

NetworkLinkStats updateNetworkLinkStats(
    const NetworkLinkStats &previous, const NetworkLinkStatRead &read,
    const std::chrono::steady_clock::time_point &now,
    const std::chrono::system_clock::time_point &wall_clock,
    std::size_t max_samples) {
  NetworkLinkStats next = previous;
  next.present = true;

  const bool fresh = read.read_ok && read.any_available;
  if (fresh) {
    next.state = NetworkLinkStatState::Valid;
  } else if (read.access_denied) {
    next.state = NetworkLinkStatState::Stale;
  } else {
    next.state = NetworkLinkStatState::Unavailable;
  }

  const NetworkLinkStatSample *last_valid = nullptr;
  for (auto it = previous.history.samples().rbegin();
       it != previous.history.samples().rend(); ++it) {
    if (it->valid) {
      last_valid = &*it;
      break;
    }
  }

  NetworkLinkStatSample sample;
  sample.timestamp = now;
  sample.wall_clock = wall_clock;
  sample.valid = fresh;
  sample.any_available = read.any_available;

  if (fresh) {
    for (std::size_t i = 0; i < kNetworkLinkStatMetricCount; ++i) {
      sample.counters[i] = read.counters[i];
    }
    if (last_valid != nullptr) {
      const double seconds =
          std::chrono::duration<double>(now - last_valid->timestamp).count();
      if (seconds > 0.0) {
        sample.rates = deriveNetworkLinkStatRates(*last_valid, sample, seconds);
        for (std::size_t i = 0; i < kNetworkLinkStatMetricCount; ++i) {
          if (sample.counters[i].has_value() &&
              last_valid->counters[i].has_value() &&
              *sample.counters[i] < *last_valid->counters[i]) {
            ++sample.discontinuity_count;
            ++next.discontinuity_count;
          }
        }
      }
    }
    next.last_update = wall_clock;
  } else {
    for (std::size_t i = 0; i < kNetworkLinkStatMetricCount; ++i) {
      sample.counters[i] =
          last_valid != nullptr ? last_valid->counters[i] : std::nullopt;
    }
  }

  ResourceHistory<NetworkLinkStatSample> retained(max_samples);
  for (const NetworkLinkStatSample &existing : previous.history.samples()) {
    retained.addSample(existing);
  }
  retained.addSample(sample);
  next.history = std::move(retained);
  return next;
}

NetworkLinkStatSummary summarizeNetworkLinkStats(const NetworkLinkStats &stats,
                                                 std::size_t max_samples) {
  NetworkLinkStatSummary summary;
  summary.discontinuity_count = stats.discontinuity_count;

  const auto &samples = stats.history.samples();
  summary.sample_count = samples.size();
  if (!samples.empty()) {
    summary.has_data = true;
    if (max_samples > 0) {
      summary.coverage = std::min(
          1.0, static_cast<double>(samples.size()) /
                   static_cast<double>(max_samples));
    }
    summary.span_seconds = std::chrono::duration<double>(
                               samples.back().timestamp - samples.front().timestamp)
                               .count();
  }

  std::size_t valid_count = 0;
  const NetworkLinkStatSample *first_valid = nullptr;
  const NetworkLinkStatSample *last_valid = nullptr;
  for (const NetworkLinkStatSample &sample : samples) {
    if (sample.valid) {
      ++valid_count;
      if (first_valid == nullptr) {
        first_valid = &sample;
      }
      last_valid = &sample;
      summary.last_update = sample.wall_clock;
    }
  }
  summary.valid_sample_count = valid_count;
  summary.stale_sample_count = samples.size() - valid_count;
  summary.has_valid_data = valid_count > 0;

  if (!samples.empty()) {
    const NetworkLinkStatSample &newest = samples.back();
    for (std::size_t i = 0; i < kNetworkLinkStatMetricCount; ++i) {
      summary.current[i] = newest.counters[i];
    }
    summary.rates = newest.rates;
  }

  if (first_valid != nullptr && last_valid != nullptr &&
      first_valid != last_valid) {
    for (std::size_t i = 0; i < kNetworkLinkStatMetricCount; ++i) {
      const std::optional<std::uint64_t> &first = first_valid->counters[i];
      const std::optional<std::uint64_t> &last = last_valid->counters[i];
      if (first.has_value() && last.has_value() && *last >= *first) {
        summary.window_delta[i] = *last - *first;
      }
    }
  }
  return summary;
}

std::string formatNetworkLinkStatRate(const std::optional<double> &rate) {
  if (!rate.has_value() || !std::isfinite(*rate) || *rate < 0.0) {
    return "unavailable";
  }
  const double value = *rate;
  const int precision = value >= 100.0 ? 0 : (value >= 1.0 ? 1 : 2);
  char buffer[64];
  const std::to_chars_result result =
      std::to_chars(buffer, buffer + sizeof(buffer), value,
                    std::chars_format::fixed, precision);
  if (result.ec != std::errc()) {
    return "unavailable";
  }
  std::string text(buffer, result.ptr);
  const std::size_t dot = text.find('.');
  if (dot != std::string::npos) {
    std::size_t end = text.size();
    while (end > dot + 1 && text[end - 1] == '0') {
      --end;
    }
    if (end == dot + 1) {
      end = dot;
    }
    text.erase(end);
  }
  text += "/s";
  return text;
}

std::string renderNetworkLinkStats(const NetworkLinkStats &stats,
                                   std::size_t max_samples) {
  const NetworkLinkStatSummary summary =
      summarizeNetworkLinkStats(stats, max_samples);
  constexpr std::size_t kNameWidth = 24;
  constexpr std::size_t kValueWidth = 24;

  std::ostringstream out;
  out << "  State: " << networkLinkStatStateName(stats.state) << '\n';
  out << "  Last update: " << formatTimestamp(summary.last_update) << '\n';
  out << "  Coverage: " << summary.sample_count << '/' << max_samples << " ("
      << std::fixed << std::setprecision(1) << (summary.coverage * 100.0)
      << "%)\n";
  out << "  Discontinuities (counter resets): " << summary.discontinuity_count
      << '\n';

  out << "  Cumulative counters (events since interface creation):\n";
  for (std::size_t i = 0; i < kNetworkLinkStatMetricCount; ++i) {
    const std::string value = summary.current[i].has_value()
                                  ? std::to_string(*summary.current[i])
                                  : "unavailable";
    out << "    "
        << padRight(networkLinkStatMetricName(
                        static_cast<NetworkLinkStatMetric>(i)),
                    kNameWidth)
        << padLeft(value, kValueWidth) << '\n';
  }

  out << "  Current interval rates (events/s):\n";
  const NetworkLinkStatMetric kRateMetrics[] = {
      NetworkLinkStatMetric::RxErrors,
      NetworkLinkStatMetric::TxErrors,
      NetworkLinkStatMetric::RxDropped,
      NetworkLinkStatMetric::TxDropped,
      NetworkLinkStatMetric::RxMissedErrors,
      NetworkLinkStatMetric::RxFifoErrors,
      NetworkLinkStatMetric::TxFifoErrors,
      NetworkLinkStatMetric::RxCrcErrors,
      NetworkLinkStatMetric::RxFrameErrors,
      NetworkLinkStatMetric::TxCarrierErrors,
      NetworkLinkStatMetric::Collisions,
  };
  for (const NetworkLinkStatMetric metric : kRateMetrics) {
    out << "    "
        << padRight(networkLinkStatMetricName(metric), kNameWidth)
        << padLeft(formatNetworkLinkStatRate(optionalRate(metric, summary.rates)),
                   kValueWidth)
        << '\n';
  }
  out << "    " << padRight("errors (rx + tx)", kNameWidth)
      << padLeft(formatNetworkLinkStatRate(summary.rates.combined_errors_per_second),
                 kValueWidth)
      << '\n';
  out << "    " << padRight("drops (rx + tx)", kNameWidth)
      << padLeft(formatNetworkLinkStatRate(summary.rates.combined_drops_per_second),
                 kValueWidth)
      << '\n';

  out << "  Retained-window deltas (first vs last valid sample):\n";
  for (std::size_t i = 0; i < kNetworkLinkStatMetricCount; ++i) {
    const std::string value = summary.window_delta[i].has_value()
                                  ? std::to_string(*summary.window_delta[i])
                                  : "unavailable";
    out << "    "
        << padRight(networkLinkStatMetricName(
                        static_cast<NetworkLinkStatMetric>(i)),
                    kNameWidth)
        << padLeft(value, kValueWidth) << '\n';
  }
  return out.str();
}

std::string renderNetworkLinkStatTrend(
    const NetworkLinkStats &stats, NetworkLinkStatMetric metric,
    const NetworkLinkStatChartConfig &config) {
  const std::size_t cols = config.data_width > 0 ? config.data_width : 1;
  const std::size_t rows = config.data_height > 0 ? config.data_height : 1;
  const std::string metric_name = networkLinkStatMetricName(metric);

  const auto &samples = stats.history.samples();
  bool any_value = false;
  for (const NetworkLinkStatSample &sample : samples) {
    if (optionalRate(metric, sample.rates).has_value()) {
      any_value = true;
      break;
    }
  }
  if (samples.empty() || !any_value) {
    return metric_name + " (no data)\n";
  }

  const std::chrono::steady_clock::time_point origin = samples.front().timestamp;
  const double span =
      std::max(0.0, std::chrono::duration<double>(samples.back().timestamp -
                                                  origin)
                        .count());

  std::vector<std::optional<double>> column(cols);
  if (span <= 0.0) {
    for (auto it = samples.rbegin(); it != samples.rend(); ++it) {
      const std::optional<double> value = optionalRate(metric, it->rates);
      if (value.has_value()) {
        column.back() = value;
        break;
      }
    }
  } else {
    for (std::size_t c = 0; c < cols; ++c) {
      const double w_start =
          span * static_cast<double>(c) / static_cast<double>(cols);
      const double w_end =
          span * static_cast<double>(c + 1) / static_cast<double>(cols);
      for (auto it = samples.rbegin(); it != samples.rend(); ++it) {
        const std::optional<double> value = optionalRate(metric, it->rates);
        if (!value.has_value()) {
          continue;
        }
        const double time = std::chrono::duration<double>(it->timestamp - origin)
                                .count();
        if (time >= w_start && time <= w_end) {
          column[c] = value;
          break;
        }
      }
    }
  }

  double peak = 0.0;
  for (const std::optional<double> &value : column) {
    if (value.has_value()) {
      peak = std::max(peak, *value);
    }
  }
  double y_max = roundScale(peak * 1.15);
  if (y_max <= 0.0) {
    y_max = 1.0;
  }

  const auto boundaryRow = [rows, y_max](double value) -> std::size_t {
    const double normalized = std::clamp(value / y_max, 0.0, 1.0);
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
  out << metric_name
      << " (events/s)  peak ~ flow .  (blank column = no measured rate)\n";

  const std::string top_label = formatScaleLabel(y_max);
  const std::string mid_label = formatScaleLabel(y_max / 2.0);
  const std::string bottom_label = formatScaleLabel(0.0);
  const std::size_t label_w =
      std::max({top_label.size(), mid_label.size(), bottom_label.size()});
  const auto paddedLabel = [label_w](const std::string &text) {
    return text.size() < label_w ? std::string(label_w - text.size(), ' ') + text
                                 : text;
  };

  std::vector<std::vector<char>> grid(rows, std::vector<char>(cols, ' '));
  for (std::size_t c = 0; c < cols; ++c) {
    if (!boundary[c].has_value()) {
      continue;
    }
    for (std::size_t r = *boundary[c]; r < rows; ++r) {
      grid[r][c] = r == *boundary[c] ? '~' : '.';
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

NetworkLinkStatsMonitor::NetworkLinkStatsMonitor(
    std::filesystem::path root, std::size_t history_max_samples)
    : root_(std::move(root)),
      history_max_samples_(std::max<std::size_t>(history_max_samples, 1)) {}

void NetworkLinkStatsMonitor::update(
    const NetworkInterfaceSnapshot &snapshot) {
  if (history_paused_) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  const auto wall_clock = std::chrono::system_clock::now();

  std::vector<std::string> present;
  present.reserve(snapshot.interfaces.size());

  for (const NetworkInterfaceInfo &info : snapshot.interfaces) {
    const std::string identity = info.identity();
    present.push_back(identity);

    auto it = tracked_.find(identity);
    if (it == tracked_.end()) {
      NetworkLinkStats fresh;
      fresh.identity = identity;
      fresh.name = info.name;
      fresh.first_seen = now;
      it = tracked_.emplace(identity, std::move(fresh)).first;
    } else {
      it->second.name = info.name;
      it->second.present = true;
    }

    const NetworkLinkStatRead read =
        readNetworkLinkStats(root_, info.name);
    it->second = updateNetworkLinkStats(it->second, read, now, wall_clock,
                                        history_max_samples_);
  }

  for (auto &kv : tracked_) {
    if (std::find(present.begin(), present.end(), kv.first) == present.end()) {
      kv.second.present = false;
    }
  }

  evictOverflow();
}

const NetworkLinkStats *NetworkLinkStatsMonitor::tracked(
    const std::string &identity) const {
  const auto it = tracked_.find(identity);
  return it == tracked_.end() ? nullptr : &it->second;
}

void NetworkLinkStatsMonitor::setHistoryMaxSamples(std::size_t max_samples) {
  if (max_samples == 0) {
    max_samples = 1;
  }
  history_max_samples_ = max_samples;
}

void NetworkLinkStatsMonitor::reset() { tracked_.clear(); }

void NetworkLinkStatsMonitor::evictOverflow() {
  if (tracked_.size() <= kMaxTrackedLinkStatsInterfaces) {
    return;
  }
  std::vector<std::string> gone;
  gone.reserve(tracked_.size());
  for (const auto &kv : tracked_) {
    if (!kv.second.present) {
      gone.push_back(kv.first);
    }
  }
  std::sort(gone.begin(), gone.end(),
            [&](const std::string &a, const std::string &b) {
              return tracked_.at(a).first_seen < tracked_.at(b).first_seen;
            });
  std::size_t excess = tracked_.size() - kMaxTrackedLinkStatsInterfaces;
  for (const std::string &identity : gone) {
    if (excess == 0) {
      break;
    }
    tracked_.erase(identity);
    --excess;
  }
}

}  // namespace atm