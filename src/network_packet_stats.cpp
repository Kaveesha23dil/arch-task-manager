#include "network_packet_stats.hpp"

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

const char *networkPacketMtuSizeClassName(std::optional<int> mtu) {
  if (!mtu.has_value()) {
    return "unknown";
  }
  if (*mtu == kStandardEthernetMtu) {
    return "standard";
  }
  if (*mtu < 0) {
    return "unknown";  // the kernel never exposes negative MTUs; be defensive
  }
  return *mtu < kStandardEthernetMtu ? "below_standard" : "above_standard";
}

const char *networkPacketMtuStateName(NetworkPacketMtuState state) {
  switch (state) {
    case NetworkPacketMtuState::Unknown:
      return "unknown";
    case NetworkPacketMtuState::Valid:
      return "valid";
    case NetworkPacketMtuState::Stale:
      return "stale";
    case NetworkPacketMtuState::Unavailable:
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

std::optional<int> parseMtu(const std::string &text) {
  const std::string trimmed = trimWhitespace(text);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  long long value = 0;
  for (const char c : trimmed) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    const long long digit = static_cast<long long>(c - '0');
    const long long kMax = static_cast<long long>(std::numeric_limits<int>::max());
    if (value > (kMax - digit) / 10) {
      return std::nullopt;  // overflow the int range
    }
    value = value * 10 + digit;
  }
  if (value < 0) {
    return std::nullopt;  // "-1" is the kernel's "no value" sentinel
  }
  return static_cast<int>(value);
}

std::optional<std::uint64_t> deltaLocal(
    const std::optional<std::uint64_t> &previous,
    const std::optional<std::uint64_t> &current) {
  if (!previous.has_value() || !current.has_value()) {
    return std::nullopt;
  }
  if (*current < *previous) {
    return std::nullopt;  // reset/wraparound — the delta is not trustworthy
  }
  return *current - *previous;
}

std::optional<double> rateOver(
    const std::optional<std::uint64_t> &previous,
    const std::optional<std::uint64_t> &current, double seconds) {
  const std::optional<std::uint64_t> delta = deltaLocal(previous, current);
  if (!delta.has_value()) {
    return std::nullopt;
  }
  if (!(seconds > 0.0) || !std::isfinite(seconds)) {
    return std::nullopt;
  }
  return static_cast<double>(*delta) / seconds;
}

std::optional<double> framesPerByteRatio(
    const std::optional<std::uint64_t> &bytes_previous,
    const std::optional<std::uint64_t> &bytes_current,
    const std::optional<std::uint64_t> &packets_previous,
    const std::optional<std::uint64_t> &packets_current) {
  const std::optional<std::uint64_t> bytes = deltaLocal(bytes_previous, bytes_current);
  const std::optional<std::uint64_t> packets =
      deltaLocal(packets_previous, packets_current);
  if (!bytes.has_value() || !packets.has_value() || *packets == 0) {
    return std::nullopt;  // no frames in the window: the ratio is meaningless
  }
  const double value = static_cast<double>(*bytes) / static_cast<double>(*packets);
  return std::isfinite(value) ? std::optional<double>(value) : std::nullopt;
}

std::optional<double> sizesRatio(
    const std::optional<std::uint64_t> &prev_bytes,
    const std::optional<std::uint64_t> &curr_bytes,
    const std::optional<std::uint64_t> &prev_packets,
    const std::optional<std::uint64_t> &curr_packets) {
  return framesPerByteRatio(prev_bytes, curr_bytes, prev_packets, curr_packets);
}

/// Newest retained sample with a fresh counter reading (the baseline for
/// per-window rate and frame-size math). Stale ticks are skipped so rates are
/// computed over real elapsed time, not over preserved copies.
const NetworkPacketMtuSample *lastCounterBaseline(
    const NetworkPacketMtuStats &stats) {
  for (auto it = stats.history.samples().rbegin();
       it != stats.history.samples().rend(); ++it) {
    if (it->counters_available) {
      return &*it;
    }
  }
  return nullptr;
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

const std::optional<double> &optionalRateMetric(
    NetworkPacketMtuTrendMetric metric, const NetworkPacketMtuSample &sample) {
  switch (metric) {
    case NetworkPacketMtuTrendMetric::RxPacketRate:
      return sample.rates.rx_packets_per_second;
    case NetworkPacketMtuTrendMetric::TxPacketRate:
      return sample.rates.tx_packets_per_second;
    case NetworkPacketMtuTrendMetric::CombinedAvgFrameSize:
      if (sample.estimate.combined_bytes_per_frame.has_value()) {
        return sample.estimate.combined_bytes_per_frame;
      }
      if (sample.estimate.rx_bytes_per_frame.has_value()) {
        return sample.estimate.rx_bytes_per_frame;
      }
      return sample.estimate.tx_bytes_per_frame;
  }
  static const std::optional<double> kNone;
  return kNone;
}

}  // namespace

NetworkPacketMtuRead readNetworkInterfaceMtu(const std::filesystem::path &root,
                                             const std::string &iface) {
  NetworkPacketMtuRead result;
  const std::filesystem::path file = root / "sys" / "class" / "net" / iface / "mtu";

  std::error_code ec;
  const std::filesystem::file_status status = std::filesystem::status(file, ec);
  if (ec) {
    if (ec == std::errc::permission_denied) {
      result.mtu_access_denied = true;
    }
    result.mtu_read_ok = false;
    return result;
  }
  if (!std::filesystem::exists(status) ||
      !std::filesystem::is_regular_file(status)) {
    result.mtu_read_ok = false;
    return result;
  }

  std::ifstream in(file);
  if (!in) {
    // Status already succeeded above (the file exists), so a failed open is a
    // permission problem: mark it honestly as access-denied when the mode bits
    // grant no read access to anyone.
    const auto perms = status.permissions();
    using std::filesystem::perms;
    const bool any_read = (perms & perms::owner_read) != perms::none ||
                          (perms & perms::group_read) != perms::none ||
                          (perms & perms::others_read) != perms::none;
    if (!any_read) {
      result.mtu_access_denied = true;
    }
    result.mtu_read_ok = false;
    return result;
  }
  std::string text;
  if (!std::getline(in, text)) {
    result.mtu_read_ok = true;  // file exists but empty: clean "no value"
    return result;
  }
  result.mtu = parseMtu(text);
  result.mtu_read_ok = true;
  return result;
}

NetworkPacketMtuRates deriveNetworkPacketMtuRates(
    const NetworkPacketMtuSample &previous, const NetworkPacketMtuSample &current,
    double seconds) {
  NetworkPacketMtuRates rates;
  if (!previous.counters_available || !current.counters_available) {
    return rates;
  }
  if (!(seconds > 0.0) || !std::isfinite(seconds)) {
    return rates;
  }
  rates.rx_packets_per_second = rateOver(previous.rx_packets, current.rx_packets, seconds);
  rates.tx_packets_per_second = rateOver(previous.tx_packets, current.tx_packets, seconds);
  rates.rx_bytes_per_second = rateOver(previous.rx_bytes, current.rx_bytes, seconds);
  rates.tx_bytes_per_second = rateOver(previous.tx_bytes, current.tx_bytes, seconds);
  if (rates.rx_packets_per_second.has_value() &&
      rates.tx_packets_per_second.has_value()) {
    rates.combined_packets_per_second =
        *rates.rx_packets_per_second + *rates.tx_packets_per_second;
  }
  if (rates.rx_bytes_per_second.has_value() &&
      rates.tx_bytes_per_second.has_value()) {
    rates.combined_bytes_per_second =
        *rates.rx_bytes_per_second + *rates.tx_bytes_per_second;
  }
  return rates;
}

NetworkPacketSizeEstimate deriveNetworkPacketSizeEstimate(
    const NetworkPacketMtuSample &previous, const NetworkPacketMtuSample &current) {
  NetworkPacketSizeEstimate estimate;
  if (!previous.counters_available || !current.counters_available) {
    return estimate;
  }
  estimate.rx_bytes_per_frame = sizesRatio(
      previous.rx_bytes, current.rx_bytes, previous.rx_packets,
      current.rx_packets);
  estimate.tx_bytes_per_frame = sizesRatio(
      previous.tx_bytes, current.tx_bytes, previous.tx_packets,
      current.tx_packets);
  const std::optional<std::uint64_t> rx_bytes =
      deltaLocal(previous.rx_bytes, current.rx_bytes);
  const std::optional<std::uint64_t> tx_bytes =
      deltaLocal(previous.tx_bytes, current.tx_bytes);
  const std::optional<std::uint64_t> rx_packets =
      deltaLocal(previous.rx_packets, current.rx_packets);
  const std::optional<std::uint64_t> tx_packets =
      deltaLocal(previous.tx_packets, current.tx_packets);
  if (rx_bytes.has_value() && tx_bytes.has_value() && rx_packets.has_value() &&
      tx_packets.has_value() && (*rx_packets + *tx_packets) > 0) {
    const double combined =
        static_cast<double>(*rx_bytes + *tx_bytes) /
        static_cast<double>(*rx_packets + *tx_packets);
    if (std::isfinite(combined)) {
      estimate.combined_bytes_per_frame = combined;
    }
  }
  return estimate;
}

NetworkPacketMtuStats updateNetworkPacketMtu(
    const NetworkPacketMtuStats &previous, const NetworkPacketMtuRead &read,
    const std::chrono::steady_clock::time_point &now,
    const std::chrono::system_clock::time_point &wall_clock,
    std::size_t max_samples) {
  NetworkPacketMtuStats next = previous;
  next.present = true;

  const bool fresh_mtu = read.mtu_read_ok && read.mtu.has_value();
  const bool fresh_counters = read.counters_available;
  const bool fresh_any = fresh_mtu || fresh_counters;
  if (fresh_any) {
    next.state = NetworkPacketMtuState::Valid;
  } else if (read.mtu_access_denied) {
    next.state = NetworkPacketMtuState::Stale;
  } else {
    next.state = NetworkPacketMtuState::Unavailable;
  }

  const auto &old_samples = previous.history.samples();
  const NetworkPacketMtuSample *last_sample =
      old_samples.empty() ? nullptr : &old_samples.back();
  const NetworkPacketMtuSample *baseline = lastCounterBaseline(previous);

  const std::optional<int> previous_mtu_field =
      last_sample != nullptr ? last_sample->mtu : std::nullopt;

  NetworkPacketMtuSample sample;
  sample.timestamp = now;
  sample.wall_clock = wall_clock;
  sample.mtu_available = fresh_mtu;
  sample.counters_available = fresh_counters;
  sample.valid = fresh_any;
  sample.mtu = fresh_mtu ? read.mtu
                         : (previous_mtu_field.has_value() ? previous_mtu_field
                                                           : read.mtu);
  if (fresh_mtu && previous_mtu_field.has_value() &&
      *read.mtu != *previous_mtu_field) {
    sample.mtu_changed = true;
    sample.previous_mtu = previous_mtu_field;
    ++next.mtu_change_count;
    if (next.mtu_events.size() < kNetworkPacketMtuMaxEvents) {
      NetworkPacketMtuChangeEvent event;
      event.wall_clock = wall_clock;
      event.previous_mtu = previous_mtu_field;
      event.new_mtu = read.mtu;
      next.mtu_events.push_back(event);
    }
  }

  if (fresh_counters) {
    sample.rx_packets = read.rx_packets;
    sample.tx_packets = read.tx_packets;
    sample.rx_bytes = read.rx_bytes;
    sample.tx_bytes = read.tx_bytes;
  } else if (last_sample != nullptr) {
    sample.rx_packets = last_sample->rx_packets;
    sample.tx_packets = last_sample->tx_packets;
    sample.rx_bytes = last_sample->rx_bytes;
    sample.tx_bytes = last_sample->tx_bytes;
  }

  if (fresh_counters && baseline != nullptr) {
    const double seconds =
        std::chrono::duration<double>(now - baseline->timestamp).count();
    if (seconds > 0.0) {
      sample.rates = deriveNetworkPacketMtuRates(*baseline, sample, seconds);
      sample.estimate =
          deriveNetworkPacketSizeEstimate(*baseline, sample);
      const std::optional<std::uint64_t> pairs[4][2] = {
          {baseline->rx_packets, sample.rx_packets},
          {baseline->tx_packets, sample.tx_packets},
          {baseline->rx_bytes, sample.rx_bytes},
          {baseline->tx_bytes, sample.tx_bytes},
      };
      for (const auto &pair : pairs) {
        if (pair[0].has_value() && pair[1].has_value() &&
            *pair[1] < *pair[0]) {
          ++sample.counter_discontinuity_count;
          ++next.discontinuity_count;
        }
      }
    }
  }

  if (fresh_any) {
    next.last_update = wall_clock;
  }

  ResourceHistory<NetworkPacketMtuSample> retained(max_samples);
  for (const NetworkPacketMtuSample &existing : old_samples) {
    retained.addSample(existing);
  }
  retained.addSample(sample);
  next.history = std::move(retained);
  return next;
}

NetworkPacketMtuSummary summarizeNetworkPacketMtu(
    const NetworkPacketMtuStats &stats, std::size_t max_samples) {
  NetworkPacketMtuSummary summary;
  summary.mtu_change_count = stats.mtu_change_count;
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

  const NetworkPacketMtuSample *first_counters = nullptr;
  const NetworkPacketMtuSample *last_counters = nullptr;
  for (const NetworkPacketMtuSample &sample : samples) {
    if (sample.valid) {
      ++summary.valid_sample_count;
      summary.last_update = sample.wall_clock;
    } else {
      ++summary.stale_sample_count;
    }
    if (sample.mtu_available) {
      ++summary.mtu_valid_count;
    }
    if (sample.counters_available) {
      ++summary.counter_valid_count;
      if (first_counters == nullptr) {
        first_counters = &sample;
      }
      last_counters = &sample;
    }
  }
  summary.has_valid_data = summary.valid_sample_count > 0;
  summary.has_mtu_data = summary.mtu_valid_count > 0;
  summary.has_counter_data = summary.counter_valid_count > 0;

  if (!samples.empty()) {
    const NetworkPacketMtuSample &newest = samples.back();
    summary.current_mtu = newest.mtu;
    summary.current_rx_packets = newest.rx_packets;
    summary.current_tx_packets = newest.tx_packets;
    summary.current_rx_bytes = newest.rx_bytes;
    summary.current_tx_bytes = newest.tx_bytes;
    summary.rates = newest.rates;
    summary.estimate = newest.estimate;

    // Previous retained MTU: the most recent distinct value before the current
    // one, so a recent change still reports what the value was changed from.
    std::optional<int> previous_seen;
    for (auto it = samples.rbegin(); it != samples.rend(); ++it) {
      if (!it->mtu.has_value()) {
        continue;
      }
      if (!summary.current_mtu.has_value()) {
        summary.current_mtu = it->mtu;
        continue;
      }
      if (*it->mtu != *summary.current_mtu) {
        previous_seen = it->mtu;
        break;
      }
    }
    summary.previous_mtu = previous_seen;

    std::optional<int> min_mtu;
    std::optional<int> max_mtu;
    for (const NetworkPacketMtuSample &sample : samples) {
      if (!sample.mtu.has_value()) {
        continue;
      }
      if (!min_mtu.has_value() || *sample.mtu < *min_mtu) {
        min_mtu = sample.mtu;
      }
      if (!max_mtu.has_value() || *sample.mtu > *max_mtu) {
        max_mtu = sample.mtu;
      }
    }
    summary.min_mtu = min_mtu;
    summary.max_mtu = max_mtu;
  }

  if (first_counters != nullptr && last_counters != nullptr &&
      first_counters != last_counters) {
    summary.window_rx_packets = deltaLocal(
        first_counters->rx_packets, last_counters->rx_packets);
    summary.window_tx_packets = deltaLocal(
        first_counters->tx_packets, last_counters->tx_packets);
    summary.window_rx_bytes = deltaLocal(
        first_counters->rx_bytes, last_counters->rx_bytes);
    summary.window_tx_bytes = deltaLocal(
        first_counters->tx_bytes, last_counters->tx_bytes);
    summary.window_estimate.rx_bytes_per_frame = framesPerByteRatio(
        first_counters->rx_bytes, last_counters->rx_bytes,
        first_counters->rx_packets, last_counters->rx_packets);
    summary.window_estimate.tx_bytes_per_frame = framesPerByteRatio(
        first_counters->tx_bytes, last_counters->tx_bytes,
        first_counters->tx_packets, last_counters->tx_packets);
  }

  return summary;
}

std::string formatNetworkPacketMtuRate(const std::optional<double> &rate) {
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

std::string formatNetworkPacketMtuThroughput(
    const std::optional<double> &bytes_per_second) {
  if (!bytes_per_second.has_value() || !std::isfinite(*bytes_per_second) ||
      *bytes_per_second < 0.0) {
    return "unavailable";
  }
  const double value = *bytes_per_second;
  double scaled = value;
  int index = 0;
  static const char *const kUnits[] = {"B/s", "KB/s", "MB/s", "GB/s", "TB/s"};
  while (scaled >= 1000.0 && index < 4) {
    scaled /= 1000.0;
    ++index;
  }
  const int precision = scaled >= 100.0 ? 0 : (scaled >= 1.0 ? 1 : 2);
  char buffer[64];
  const std::to_chars_result result =
      std::to_chars(buffer, buffer + sizeof(buffer), scaled,
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
  text += ' ';
  text += kUnits[index];
  return text;
}

std::string formatNetworkPacketSizeEstimate(
    const std::optional<double> &bytes_per_frame) {
  if (!bytes_per_frame.has_value() || !std::isfinite(*bytes_per_frame) ||
      *bytes_per_frame < 0.0) {
    return "unavailable";
  }
  char buffer[64];
  const std::to_chars_result result =
      std::to_chars(buffer, buffer + sizeof(buffer), *bytes_per_frame,
                    std::chars_format::fixed, 1);
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
  text += " bytes/frame";
  return text;
}

std::string renderNetworkPacketMtu(const NetworkPacketMtuStats &stats,
                                   std::size_t max_samples) {
  const NetworkPacketMtuSummary summary =
      summarizeNetworkPacketMtu(stats, max_samples);
  constexpr std::size_t kNameWidth = 26;
  constexpr std::size_t kValueWidth = 24;

  std::ostringstream out;
  out << "  State: " << networkPacketMtuStateName(stats.state) << '\n';
  out << "  Last update: " << formatTimestamp(summary.last_update) << '\n';
  out << "  Coverage: " << summary.sample_count << '/' << max_samples << " ("
      << std::fixed << std::setprecision(1) << (summary.coverage * 100.0)
      << "%)\n";

  const char *const size_class = networkPacketMtuSizeClassName(summary.current_mtu);
  if (summary.current_mtu.has_value()) {
    out << "  Configured MTU: " << *summary.current_mtu << " bytes";
    if (std::string(size_class) == "standard") {
      out << " (standard Ethernet MTU)";
    } else if (std::string(size_class) == "above_standard") {
      out << " (large configured MTU)";
      out << "\n    This is the configured MTU, not a verified jumbo-frame"
             " capability.";
    } else if (std::string(size_class) == "below_standard") {
      out << " (below the standard Ethernet MTU)";
    }
    out << '\n';
    if (summary.previous_mtu.has_value()) {
      out << "  MTU changes from previous value: " << *summary.current_mtu
          << " (was " << *summary.previous_mtu << ')';
      if (!stats.history.samples().empty() &&
          stats.history.samples().back().mtu_changed) {
        out << "  [changed this tick]";
      }
      out << '\n';
    }
  } else {
    out << "  Configured MTU: unavailable\n";
  }
  out << "  MTU changes recorded: " << summary.mtu_change_count << '\n';
  out << "  Counter discontinuities (resets): " << summary.discontinuity_count
      << '\n';

  out << "  Cumulative counters (since interface creation):\n";
  out << "    "
      << padRight("rx packets", kNameWidth)
      << padLeft(summary.current_rx_packets.has_value()
                     ? std::to_string(*summary.current_rx_packets)
                     : "unavailable",
                 kValueWidth)
      << '\n';
  out << "    "
      << padRight("tx packets", kNameWidth)
      << padLeft(summary.current_tx_packets.has_value()
                     ? std::to_string(*summary.current_tx_packets)
                     : "unavailable",
                 kValueWidth)
      << '\n';
  out << "    "
      << padRight("rx bytes", kNameWidth)
      << padLeft(summary.current_rx_bytes.has_value()
                     ? std::to_string(*summary.current_rx_bytes)
                     : "unavailable",
                 kValueWidth)
      << '\n';
  out << "    "
      << padRight("tx bytes", kNameWidth)
      << padLeft(summary.current_tx_bytes.has_value()
                     ? std::to_string(*summary.current_tx_bytes)
                     : "unavailable",
                 kValueWidth)
      << '\n';

  out << "  Interval rates (current window):\n";
  out << "    " << padRight("rx packets/s", kNameWidth)
      << padLeft(formatNetworkPacketMtuRate(summary.rates.rx_packets_per_second),
                 kValueWidth)
      << '\n';
  out << "    " << padRight("tx packets/s", kNameWidth)
      << padLeft(formatNetworkPacketMtuRate(summary.rates.tx_packets_per_second),
                 kValueWidth)
      << '\n';
  out << "    " << padRight("rx bytes/s", kNameWidth)
      << padLeft(formatNetworkPacketMtuThroughput(summary.rates.rx_bytes_per_second),
                 kValueWidth)
      << '\n';
  out << "    " << padRight("tx bytes/s", kNameWidth)
      << padLeft(formatNetworkPacketMtuThroughput(summary.rates.tx_bytes_per_second),
                 kValueWidth)
      << '\n';

  out << "  Estimated average frame size (current window; estimate only, no "
         "size distribution):\n";
  out << "    " << padRight("rx bytes/frame", kNameWidth)
      << padLeft(formatNetworkPacketSizeEstimate(
                     summary.estimate.rx_bytes_per_frame),
                 kValueWidth)
      << '\n';
  out << "    " << padRight("tx bytes/frame", kNameWidth)
      << padLeft(formatNetworkPacketSizeEstimate(
                     summary.estimate.tx_bytes_per_frame),
                 kValueWidth)
      << '\n';
  out << "    " << padRight("combined bytes/frame", kNameWidth)
      << padLeft(formatNetworkPacketSizeEstimate(
                     summary.estimate.combined_bytes_per_frame),
                 kValueWidth)
      << '\n';

  out << "  Retained-window totals (first vs last fresh sample):\n";
  out << "    " << padRight("rx packets", kNameWidth)
      << padLeft(summary.window_rx_packets.has_value()
                     ? std::to_string(*summary.window_rx_packets)
                     : "unavailable",
                 kValueWidth)
      << '\n';
  out << "    " << padRight("tx packets", kNameWidth)
      << padLeft(summary.window_tx_packets.has_value()
                     ? std::to_string(*summary.window_tx_packets)
                     : "unavailable",
                 kValueWidth)
      << '\n';
  out << "    " << padRight("rx bytes", kNameWidth)
      << padLeft(summary.window_rx_bytes.has_value()
                     ? std::to_string(*summary.window_rx_bytes)
                     : "unavailable",
                 kValueWidth)
      << '\n';
  out << "    " << padRight("tx bytes", kNameWidth)
      << padLeft(summary.window_tx_bytes.has_value()
                     ? std::to_string(*summary.window_tx_bytes)
                     : "unavailable",
                 kValueWidth)
      << '\n';
  out << "    " << padRight("rx bytes/frame (window)", kNameWidth)
      << padLeft(formatNetworkPacketSizeEstimate(
                     summary.window_estimate.rx_bytes_per_frame),
                 kValueWidth)
      << '\n';
  out << "    " << padRight("tx bytes/frame (window)", kNameWidth)
      << padLeft(formatNetworkPacketSizeEstimate(
                     summary.window_estimate.tx_bytes_per_frame),
                 kValueWidth)
      << '\n';
  return out.str();
}

std::string renderNetworkPacketMtuChanges(const NetworkPacketMtuStats &stats) {
  std::ostringstream out;
  if (stats.mtu_events.empty()) {
    out << "  no MTU changes recorded\n";
    return out.str();
  }
  out << "  MTU change timeline (most recent "
      << stats.mtu_events.size() << "):\n";
  for (const NetworkPacketMtuChangeEvent &event : stats.mtu_events) {
    out << "    " << formatTimestamp(event.wall_clock) << "  "
        << (event.previous_mtu.has_value() ? std::to_string(*event.previous_mtu)
                                           : "unavailable")
        << " -> "
        << (event.new_mtu.has_value() ? std::to_string(*event.new_mtu)
                                      : "unavailable")
        << '\n';
  }
  return out.str();
}

std::string renderNetworkPacketMtuTrend(
    const NetworkPacketMtuStats &stats, NetworkPacketMtuTrendMetric metric,
    const NetworkPacketMtuChartConfig &config) {
  const std::size_t cols = config.data_width > 0 ? config.data_width : 1;
  const std::size_t rows = config.data_height > 0 ? config.data_height : 1;

  const char *label = "";
  const char *units = "";
  switch (metric) {
    case NetworkPacketMtuTrendMetric::RxPacketRate:
      label = "rx packets/s";
      units = "packets/s";
      break;
    case NetworkPacketMtuTrendMetric::TxPacketRate:
      label = "tx packets/s";
      units = "packets/s";
      break;
    case NetworkPacketMtuTrendMetric::CombinedAvgFrameSize:
      label = "avg frame size";
      units = "bytes";
      break;
  }

  const auto &samples = stats.history.samples();
  bool any_value = false;
  for (const NetworkPacketMtuSample &sample : samples) {
    if (optionalRateMetric(metric, sample).has_value()) {
      any_value = true;
      break;
    }
  }
  if (samples.empty() || !any_value) {
    return std::string(label) + " (no data)\n";
  }

  const std::chrono::steady_clock::time_point origin = samples.front().timestamp;
  const double span =
      std::max(0.0, std::chrono::duration<double>(samples.back().timestamp -
                                                  origin)
                        .count());

  std::vector<std::optional<double>> column(cols);
  if (span <= 0.0) {
    for (auto it = samples.rbegin(); it != samples.rend(); ++it) {
      const std::optional<double> value = optionalRateMetric(metric, *it);
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
        const std::optional<double> value = optionalRateMetric(metric, *it);
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
  out << label << " (" << units
      << ")  peak ~ flow .  (blank column = no measured value)\n";

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

NetworkPacketMtuMonitor::NetworkPacketMtuMonitor(
    std::filesystem::path root, std::size_t history_max_samples)
    : root_(std::move(root)),
      history_max_samples_(std::max<std::size_t>(history_max_samples, 1)) {}

void NetworkPacketMtuMonitor::update(
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
      NetworkPacketMtuStats fresh;
      fresh.identity = identity;
      fresh.name = info.name;
      fresh.first_seen = now;
      it = tracked_.emplace(identity, std::move(fresh)).first;
    } else {
      it->second.name = info.name;
      it->second.present = true;
    }

    NetworkPacketMtuRead read = readNetworkInterfaceMtu(root_, info.name);
    if (info.traffic.has_value()) {
      read.counters_available = true;
      read.rx_packets = info.traffic->rx_packets;
      read.tx_packets = info.traffic->tx_packets;
      read.rx_bytes = info.traffic->rx_bytes;
      read.tx_bytes = info.traffic->tx_bytes;
    }
    it->second = updateNetworkPacketMtu(it->second, read, now, wall_clock,
                                        history_max_samples_);
  }

  for (auto &kv : tracked_) {
    if (std::find(present.begin(), present.end(), kv.first) == present.end()) {
      kv.second.present = false;
    }
  }

  evictOverflow();
}

const NetworkPacketMtuStats *NetworkPacketMtuMonitor::tracked(
    const std::string &identity) const {
  const auto it = tracked_.find(identity);
  return it == tracked_.end() ? nullptr : &it->second;
}

void NetworkPacketMtuMonitor::setHistoryMaxSamples(std::size_t max_samples) {
  if (max_samples == 0) {
    max_samples = 1;
  }
  history_max_samples_ = max_samples;
}

void NetworkPacketMtuMonitor::reset() { tracked_.clear(); }

void NetworkPacketMtuMonitor::evictOverflow() {
  if (tracked_.size() <= kMaxTrackedPacketMtuInterfaces) {
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
  std::size_t excess = tracked_.size() - kMaxTrackedPacketMtuInterfaces;
  for (const std::string &identity : gone) {
    if (excess == 0) {
      break;
    }
    tracked_.erase(identity);
    --excess;
  }
}

}  // namespace atm