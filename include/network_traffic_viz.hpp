#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>

#include "network_traffic_history.hpp"
#include "resource_history.hpp"

namespace atm {

/// Compact throughput summary of one NetworkTrafficSeries, derived strictly
/// from the canonical ring buffers (rate rings for current/peak, cumulative
/// rings for window totals). Nothing is re-derived from raw counters here and
/// the renderer never touches the network.
struct NetworkTrafficSummary {
  bool has_rate_data = false;
  double rx_current = 0.0;
  double rx_peak = 0.0;
  double tx_current = 0.0;
  double tx_peak = 0.0;
  /// Total transferred during the retained window, from the cumulative
  /// counters (last - first). std::nullopt when there is not enough data or a
  /// counter reset makes the delta unreliable.
  std::optional<double> rx_window_total;
  std::optional<double> tx_window_total;
  std::size_t sample_count = 0;  // cumulative-ring sample count
  double span_seconds = 0.0;     // retained-window span (seconds)
  std::chrono::system_clock::time_point last_update{};
  bool aggregate = false;
  bool membership_changed = false;
};

/// Derives the display summary for one series. Never a fake zero: window
/// totals are unavailable (not zero) when they cannot be trusted.
[[nodiscard]] NetworkTrafficSummary summarizeNetworkTrafficSeries(
    const NetworkTrafficSeries &series);

/// Chart geometry for the overlaid RX/TX throughput graph.
struct NetworkTrafficChartConfig {
  std::size_t data_width = 40;   // plot columns
  std::size_t data_height = 6;   // plot rows (y-axis scale labels included)
};

/// Renders the combined RX/TX throughput chart for one series (aggregate or a
/// tracked interface) as monochrome-safe ASCII. RX and TX share one y-axis
/// scale so the directions can be compared directly; distinct glyphs separate
/// the two directions and the overlap regions:
///
///   RX peak '~' / RX flow '.' | TX peak '^' / TX flow ':' | overlap '#' / '+'
///
/// The block includes a legend line, readable y-axis scale labels, and a
/// relative time axis ("-<span>s ... Now"). Deterministic and bounded (O(width
/// log n) per render, no copies of the history arrays).
///
/// When neither ring holds a sample a short "(no data)" marker is returned.
[[nodiscard]] std::string renderNetworkTrafficChart(
    const ResourceHistory<TimedSample> &rx_rates,
    const ResourceHistory<TimedSample> &tx_rates,
    const NetworkTrafficChartConfig &config);

}  // namespace atm