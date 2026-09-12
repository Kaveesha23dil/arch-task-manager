#include "network_traffic_history.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace atm {

namespace {

/// Computes one optional rate from two cumulative readouts. A missing counter
/// on either side, zero/non-positive seconds, or a decreasing counter (reset /
/// wraparound) yield "unavailable" for that metric only; the group-level reset
/// decision lives in deriveTrafficRates().
std::optional<double> rateFor(const std::optional<std::uint64_t> &previous,
                              const std::optional<std::uint64_t> &current,
                              double seconds) {
  if (!previous.has_value() || !current.has_value() || !(seconds > 0.0)) {
    return std::nullopt;
  }
  if (*current < *previous) {
    return std::nullopt;
  }
  // Unsigned subtraction is free of signed-overflow and wraparound hazards;
  // the result is promoted to double only afterwards.
  return static_cast<double>(*current - *previous) / seconds;
}

}  // namespace

NetworkTrafficRates deriveTrafficRates(const NetworkTrafficCounters &previous,
                                       const NetworkTrafficCounters &current,
                                       double seconds) {
  NetworkTrafficRates rates;

  if (!std::isfinite(seconds) || !(seconds > 0.0) ||
      previous.identity.empty() || previous.identity != current.identity) {
    return rates;  // no safe window; every rate stays unavailable
  }

  // Counter reset / interface restart / 64-bit wraparound: when any counter
  // that was present in both readouts decreased, the counters belong to a
  // different epoch. Suppressing the whole window (the caller reseeds its
  // baseline) prevents a bogus multi-gigabyte "rate burst" from ever being
  // plotted. A counter present on only one side is a temporary unavailability,
  // not a reset, and only disables that metric.
  const auto decreased = [](const std::optional<std::uint64_t> &a,
                            const std::optional<std::uint64_t> &b) {
    return a.has_value() && b.has_value() && *b < *a;
  };
  if (decreased(previous.rx_bytes, current.rx_bytes) ||
      decreased(previous.tx_bytes, current.tx_bytes) ||
      decreased(previous.rx_packets, current.rx_packets) ||
      decreased(previous.tx_packets, current.tx_packets) ||
      decreased(previous.rx_errors, current.rx_errors) ||
      decreased(previous.tx_errors, current.tx_errors) ||
      decreased(previous.rx_dropped, current.rx_dropped) ||
      decreased(previous.tx_dropped, current.tx_dropped)) {
    return rates;
  }

  rates.rx_bytes_per_second =
      rateFor(previous.rx_bytes, current.rx_bytes, seconds);
  rates.tx_bytes_per_second =
      rateFor(previous.tx_bytes, current.tx_bytes, seconds);
  rates.rx_packets_per_second =
      rateFor(previous.rx_packets, current.rx_packets, seconds);
  rates.tx_packets_per_second =
      rateFor(previous.tx_packets, current.tx_packets, seconds);
  rates.rx_errors_per_second =
      rateFor(previous.rx_errors, current.rx_errors, seconds);
  rates.tx_errors_per_second =
      rateFor(previous.tx_errors, current.tx_errors, seconds);
  rates.rx_dropped_per_second =
      rateFor(previous.rx_dropped, current.rx_dropped, seconds);
  rates.tx_dropped_per_second =
      rateFor(previous.tx_dropped, current.tx_dropped, seconds);
  return rates;
}

NetworkTrafficCounters synthesizeAggregateCounters(
    const std::vector<const NetworkTrafficCounters *> &members,
    const std::string &name) {
  NetworkTrafficCounters aggregate;
  aggregate.identity = std::string(kNetworkTrafficAllIdentity);
  aggregate.name = name;

  constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
  const auto sum = [&members](const auto &extract) -> std::optional<std::uint64_t> {
    if (members.empty()) {
      return std::nullopt;
    }
    std::uint64_t total = 0;
    for (const NetworkTrafficCounters *member : members) {
      const auto value = extract(*member);
      if (!value.has_value()) {
        return std::nullopt;  // any missing member makes the sum unavailable
      }
      if (*value > kMax - total) {
        return std::nullopt;  // overflow is reported, never wrapped
      }
      total += *value;
    }
    return total;
  };

  aggregate.rx_bytes = sum(
      [](const NetworkTrafficCounters &c) { return c.rx_bytes; });
  aggregate.tx_bytes = sum(
      [](const NetworkTrafficCounters &c) { return c.tx_bytes; });
  aggregate.rx_packets = sum(
      [](const NetworkTrafficCounters &c) { return c.rx_packets; });
  aggregate.tx_packets = sum(
      [](const NetworkTrafficCounters &c) { return c.tx_packets; });
  aggregate.rx_errors = sum(
      [](const NetworkTrafficCounters &c) { return c.rx_errors; });
  aggregate.tx_errors = sum(
      [](const NetworkTrafficCounters &c) { return c.tx_errors; });
  aggregate.rx_dropped = sum(
      [](const NetworkTrafficCounters &c) { return c.rx_dropped; });
  aggregate.tx_dropped = sum(
      [](const NetworkTrafficCounters &c) { return c.tx_dropped; });
  return aggregate;
}

namespace {

/// Appends a level/counter sample when the value is available (never a fake
/// zero) and a rate sample when the rate is available (gaps are left as-is so
/// unavailable windows are honest, not zeroed).
void appendLevel(ResourceHistory<TimedSample> &ring,
                 std::chrono::steady_clock::time_point now,
                 const std::optional<std::uint64_t> &value) {
  if (value.has_value()) {
    ring.addSample(TimedSample{now, static_cast<double>(*value)});
  }
}

void appendRate(ResourceHistory<TimedSample> &ring,
                std::chrono::steady_clock::time_point now,
                const std::optional<double> &value) {
  if (value.has_value()) {
    ring.addSample(TimedSample{now, *value});
  }
}

}  // namespace

NetworkTrafficSeries *NetworkTrafficHistory::seriesOrCreate(
    const std::string &identity, std::string display_name, bool aggregate) {
  const auto existing = series_.find(identity);
  if (existing != series_.end()) {
    existing->second.display_name = std::move(display_name);
    return &existing->second;
  }
  if (!aggregate &&
      static_cast<std::size_t>(std::count_if(
          series_.begin(), series_.end(),
          [](const auto &entry) { return !entry.second.aggregate; })) >=
          kMaxTrackedInterfaceSeries) {
    return nullptr;  // series cap reached — do not create a permanent series
  }
  NetworkTrafficSeries series;
  series.identity = identity;
  series.display_name = std::move(display_name);
  series.aggregate = aggregate;
  series.rx_bytes_per_second = ResourceHistory<TimedSample>(max_samples_);
  series.tx_bytes_per_second = ResourceHistory<TimedSample>(max_samples_);
  series.rx_packets_per_second = ResourceHistory<TimedSample>(max_samples_);
  series.tx_packets_per_second = ResourceHistory<TimedSample>(max_samples_);
  series.rx_errors_per_second = ResourceHistory<TimedSample>(max_samples_);
  series.tx_errors_per_second = ResourceHistory<TimedSample>(max_samples_);
  series.rx_dropped_per_second = ResourceHistory<TimedSample>(max_samples_);
  series.tx_dropped_per_second = ResourceHistory<TimedSample>(max_samples_);
  series.rx_bytes_total = ResourceHistory<TimedSample>(max_samples_);
  series.tx_bytes_total = ResourceHistory<TimedSample>(max_samples_);

  const auto [inserted, ok] = series_.emplace(identity, std::move(series));
  if (!aggregate) {
    selection_order_.push_back(identity);
  }
  return &inserted->second;
}

void NetworkTrafficHistory::record(const NetworkInterfaceSnapshot &interfaces) {
  if (history_paused_) {
    return;  // no sampling while paused; no fake samples are created on resume
  }

  const auto now = std::chrono::steady_clock::now();
  const auto wall = std::chrono::system_clock::now();

  double seconds = 0.0;
  if (previous_refresh_.has_value()) {
    seconds =
        std::chrono::duration<double>(now - *previous_refresh_).count();
  }
  previous_refresh_ = now;

  NetworkTrafficSeries *aggregate =
      seriesOrCreate(std::string(kNetworkTrafficAllIdentity),
                     "All interfaces", /*aggregate=*/true);

  std::vector<std::string> present;   // identities sampled this tick
  std::vector<std::string> members;   // non-loopback identities for the aggregate

  // Running aggregate accumulators (overflow-guarded, never wrapped).
  constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t agg_rx_bytes = 0;
  std::uint64_t agg_tx_bytes = 0;
  bool agg_rx_all_present = true;  // every member reported the counter
  bool agg_tx_all_present = true;
  bool agg_rx_overflow = false;
  bool agg_tx_overflow = false;
  bool any_member = false;

  double agg_rx_rate = 0.0, agg_tx_rate = 0.0;
  double agg_rx_packets = 0.0, agg_tx_packets = 0.0;
  double agg_rx_errors = 0.0, agg_tx_errors = 0.0;
  double agg_rx_drops = 0.0, agg_tx_drops = 0.0;
  bool any_rx_rate = false, any_tx_rate = false;
  bool any_rx_packets = false, any_tx_packets = false;
  bool any_rx_errors = false, any_tx_errors = false;
  bool any_rx_drops = false, any_tx_drops = false;

  for (const NetworkInterfaceInfo &info : interfaces.interfaces) {
    if (!info.traffic.has_value()) {
      continue;  // counters temporarily unavailable this refresh
    }
    const std::string identity = info.identity();
    if (identity.empty()) {
      continue;
    }

    NetworkTrafficCounters counters;
    counters.identity = identity;
    counters.name = info.name;
    counters.rx_bytes = info.traffic->rx_bytes;
    counters.tx_bytes = info.traffic->tx_bytes;
    counters.rx_packets = info.traffic->rx_packets;
    counters.tx_packets = info.traffic->tx_packets;
    counters.rx_errors = info.traffic->rx_errors;
    counters.tx_errors = info.traffic->tx_errors;
    counters.rx_dropped = info.traffic->rx_dropped;
    counters.tx_dropped = info.traffic->tx_dropped;

    present.push_back(identity);

    // Safe rate window: no baseline yet, or a counter reset/wrap detected, both
    // yield "no rate this window" while the baseline below is re-seeded.
    NetworkTrafficRates rates;
    const auto baseline = baselines_.find(identity);
    if (baseline != baselines_.end() &&
        baseline->second.identity == identity) {
      rates = deriveTrafficRates(baseline->second, counters, seconds);
    }
    baselines_[identity] = counters;

    NetworkTrafficSeries *const series =
        seriesOrCreate(identity, info.name, /*aggregate=*/false);
    if (series != nullptr) {
      appendRate(series->rx_bytes_per_second, now, rates.rx_bytes_per_second);
      appendRate(series->tx_bytes_per_second, now, rates.tx_bytes_per_second);
      appendRate(series->rx_packets_per_second, now, rates.rx_packets_per_second);
      appendRate(series->tx_packets_per_second, now, rates.tx_packets_per_second);
      appendRate(series->rx_errors_per_second, now, rates.rx_errors_per_second);
      appendRate(series->tx_errors_per_second, now, rates.tx_errors_per_second);
      appendRate(series->rx_dropped_per_second, now, rates.rx_dropped_per_second);
      appendRate(series->tx_dropped_per_second, now, rates.tx_dropped_per_second);
      appendLevel(series->rx_bytes_total, now, counters.rx_bytes);
      appendLevel(series->tx_bytes_total, now, counters.tx_bytes);
      series->last_update = wall;
    }

    if (info.type == NetworkInterfaceType::Loopback) {
      continue;  // excluded from the aggregate, matching the live totals
    }

    members.push_back(identity);
    any_member = true;
    if (!counters.rx_bytes.has_value()) {
      agg_rx_all_present = false;
    } else if (*counters.rx_bytes <= kMax - agg_rx_bytes) {
      agg_rx_bytes += *counters.rx_bytes;
    } else {
      agg_rx_overflow = true;
    }
    if (!counters.tx_bytes.has_value()) {
      agg_tx_all_present = false;
    } else if (*counters.tx_bytes <= kMax - agg_tx_bytes) {
      agg_tx_bytes += *counters.tx_bytes;
    } else {
      agg_tx_overflow = true;
    }

    if (rates.rx_bytes_per_second.has_value()) {
      agg_rx_rate += *rates.rx_bytes_per_second;
      any_rx_rate = true;
    }
    if (rates.tx_bytes_per_second.has_value()) {
      agg_tx_rate += *rates.tx_bytes_per_second;
      any_tx_rate = true;
    }
    if (rates.rx_packets_per_second.has_value()) {
      agg_rx_packets += *rates.rx_packets_per_second;
      any_rx_packets = true;
    }
    if (rates.tx_packets_per_second.has_value()) {
      agg_tx_packets += *rates.tx_packets_per_second;
      any_tx_packets = true;
    }
    if (rates.rx_errors_per_second.has_value()) {
      agg_rx_errors += *rates.rx_errors_per_second;
      any_rx_errors = true;
    }
    if (rates.tx_errors_per_second.has_value()) {
      agg_tx_errors += *rates.tx_errors_per_second;
      any_tx_errors = true;
    }
    if (rates.rx_dropped_per_second.has_value()) {
      agg_rx_drops += *rates.rx_dropped_per_second;
      any_rx_drops = true;
    }
    if (rates.tx_dropped_per_second.has_value()) {
      agg_tx_drops += *rates.tx_dropped_per_second;
      any_tx_drops = true;
    }
  }

  // Aggregate series: updated only when at least one non-loopback interface
  // contributed this tick; an all-missing metric stays unavailable.
  const bool membership_changed = (members != aggregate_members_);
  aggregate_members_ = members;
  if (aggregate != nullptr) {
    aggregate->membership_changed = membership_changed;
    if (any_member) {
      if (agg_rx_all_present && !agg_rx_overflow) {
        aggregate->rx_bytes_total.addSample(
            TimedSample{now, static_cast<double>(agg_rx_bytes)});
      }
      if (agg_tx_all_present && !agg_tx_overflow) {
        aggregate->tx_bytes_total.addSample(
            TimedSample{now, static_cast<double>(agg_tx_bytes)});
      }
      if (any_rx_rate) {
        aggregate->rx_bytes_per_second.addSample(
            TimedSample{now, agg_rx_rate});
      }
      if (any_tx_rate) {
        aggregate->tx_bytes_per_second.addSample(
            TimedSample{now, agg_tx_rate});
      }
      if (any_rx_packets) {
        aggregate->rx_packets_per_second.addSample(
            TimedSample{now, agg_rx_packets});
      }
      if (any_tx_packets) {
        aggregate->tx_packets_per_second.addSample(
            TimedSample{now, agg_tx_packets});
      }
      if (any_rx_errors) {
        aggregate->rx_errors_per_second.addSample(
            TimedSample{now, agg_rx_errors});
      }
      if (any_tx_errors) {
        aggregate->tx_errors_per_second.addSample(
            TimedSample{now, agg_tx_errors});
      }
      if (any_rx_drops) {
        aggregate->rx_dropped_per_second.addSample(
            TimedSample{now, agg_rx_drops});
      }
      if (any_tx_drops) {
        aggregate->tx_dropped_per_second.addSample(
            TimedSample{now, agg_tx_drops});
      }
      aggregate->last_update = wall;
    }
  }

  pruneHistory(present);
}

const NetworkTrafficSeries *NetworkTrafficHistory::seriesFor(
    const std::string &identity) const {
  const auto it = series_.find(identity);
  return it == series_.end() ? nullptr : &it->second;
}

std::vector<std::string> NetworkTrafficHistory::selectableIdentities() const {
  std::vector<std::string> identities;
  identities.reserve(selection_order_.size() + 1);
  identities.push_back(std::string(kNetworkTrafficAllIdentity));
  for (const std::string &identity : selection_order_) {
    if (series_.count(identity) != 0) {
      identities.push_back(identity);
    }
  }
  return identities;
}

std::string NetworkTrafficHistory::displayNameFor(
    const std::string &identity) const {
  const NetworkTrafficSeries *series = seriesFor(identity);
  return series != nullptr ? series->display_name
                           : "All interfaces";  // stale selection falls back
}

bool NetworkTrafficHistory::aggregateAvailable() const {
  return seriesFor(std::string(kNetworkTrafficAllIdentity)) != nullptr;
}

void NetworkTrafficHistory::pruneHistory(
    const std::vector<std::string> &present_identities) {
  std::vector<std::string> vanished;
  for (const auto &[identity, series] : series_) {
    if (series.aggregate) {
      continue;  // the aggregate is always kept (may be empty)
    }
    if (std::find(present_identities.begin(), present_identities.end(),
                  identity) == present_identities.end()) {
      vanished.push_back(identity);
    }
  }
  for (const std::string &identity : vanished) {
    series_.erase(identity);
    const auto order_it =
        std::find(selection_order_.begin(), selection_order_.end(), identity);
    if (order_it != selection_order_.end()) {
      selection_order_.erase(order_it);
    }
    baselines_.erase(identity);
  }
}

void NetworkTrafficHistory::setHistoryMaxSamples(std::size_t max_samples) {
  if (max_samples == 0) {
    max_samples = 1;
  }
  max_samples_ = max_samples;
  for (auto &[identity, series] : series_) {
    (void)identity;
    series.rx_bytes_per_second = ResourceHistory<TimedSample>(max_samples_);
    series.tx_bytes_per_second = ResourceHistory<TimedSample>(max_samples_);
    series.rx_packets_per_second = ResourceHistory<TimedSample>(max_samples_);
    series.tx_packets_per_second = ResourceHistory<TimedSample>(max_samples_);
    series.rx_errors_per_second = ResourceHistory<TimedSample>(max_samples_);
    series.tx_errors_per_second = ResourceHistory<TimedSample>(max_samples_);
    series.rx_dropped_per_second = ResourceHistory<TimedSample>(max_samples_);
    series.tx_dropped_per_second = ResourceHistory<TimedSample>(max_samples_);
    series.rx_bytes_total = ResourceHistory<TimedSample>(max_samples_);
    series.tx_bytes_total = ResourceHistory<TimedSample>(max_samples_);
  }
}

void NetworkTrafficHistory::setHistoryPaused(bool paused) {
  history_paused_ = paused;
}

void NetworkTrafficHistory::clearHistory() {
  series_.clear();
  selection_order_.clear();
  baselines_.clear();
  aggregate_members_.clear();
  previous_refresh_.reset();
}

}  // namespace atm