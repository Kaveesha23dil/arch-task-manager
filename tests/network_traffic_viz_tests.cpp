#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "network_traffic_history.hpp"
#include "network_traffic_viz.hpp"

// --- Minimal standalone test harness (no external framework) -------------
namespace {
int g_checks = 0;
int g_failures = 0;

void expect(bool condition, const char *expr, const char *file, int line) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
  }
}
void expectNear(double a, double b, double tol, const char *expr,
                const char *file, int line) {
  ++g_checks;
  if (!(a > b - tol && a < b + tol)) {
    ++g_failures;
    std::fprintf(stderr, "FAIL %s:%d: %s (got %g, want %g +/- %g)\n", file,
                 line, expr, a, b, tol);
  }
}
void run(const char *name) { std::fprintf(stderr, "TEST %s\n", name); }
}  // namespace

#define CHECK(expr) ::expect((expr), #expr, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, tol) \
  ::expectNear((a), (b), (tol), #a " ~= " #b, __FILE__, __LINE__)

// -------------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------------

using atm::NetworkTrafficSeries;
using atm::ResourceHistory;
using atm::TimedSample;

using Clock = std::chrono::steady_clock;

/// Builds a rate/counter ring from (seconds-from-base, value) pairs. Reuses a
/// steady-clock base so timestamps and spans are deterministic in relative
/// terms. Ring capacity mirrors the production default (120 samples).
ResourceHistory<TimedSample> makeRing(
    const std::vector<std::pair<double, double>> &samples) {
  ResourceHistory<TimedSample> ring(atm::kDefaultNetworkTrafficHistorySamples);
  const Clock::time_point base = Clock::now();
  for (const auto &[seconds, value] : samples) {
    const auto offset = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(seconds));
    ring.addSample(TimedSample{base + offset, value});
  }
  return ring;
}

/// One sample pair (rx, tx) for the ring being populated.
using SamplePair = std::pair<double, double>;

/// Builds a fully-populated series with rate + cumulative rings from
/// (seconds, rx, tx) rows.
NetworkTrafficSeries makeSeries(
    const std::vector<std::tuple<double, double, double>> &rows) {
  NetworkTrafficSeries series;
  series.identity = "idx:1";
  series.display_name = "eth0";
  std::vector<std::pair<double, double>> rx_rate, tx_rate, rx_total, tx_total;
  rx_rate.reserve(rows.size());
  tx_rate.reserve(rows.size());
  rx_total.reserve(rows.size());
  tx_total.reserve(rows.size());
  for (const auto &[seconds, rx, tx] : rows) {
    rx_rate.emplace_back(seconds, rx);
    tx_rate.emplace_back(seconds, tx);
    rx_total.emplace_back(seconds, rx);
    tx_total.emplace_back(seconds, tx);
  }
  series.rx_bytes_per_second = makeRing(rx_rate);
  series.tx_bytes_per_second = makeRing(tx_rate);
  series.rx_bytes_total = makeRing(rx_total);
  series.tx_bytes_total = makeRing(tx_total);
  series.last_update = std::chrono::system_clock::now();
  return series;
}

// -------------------------------------------------------------------------
// summarizeNetworkTrafficSeries
// -------------------------------------------------------------------------

void testSummaryEmptySeries() {
  run("summary: empty series");
  NetworkTrafficSeries series;
  series.identity = "idx:1";
  const atm::NetworkTrafficSummary summary = atm::summarizeNetworkTrafficSeries(series);
  CHECK(!summary.has_rate_data);
  CHECK_NEAR(summary.rx_current, 0.0, 1e-9);
  CHECK_NEAR(summary.rx_peak, 0.0, 1e-9);
  CHECK_NEAR(summary.tx_current, 0.0, 1e-9);
  CHECK_NEAR(summary.tx_peak, 0.0, 1e-9);
  CHECK(!summary.rx_window_total.has_value());
  CHECK(!summary.tx_window_total.has_value());
  CHECK(summary.sample_count == 0);
  CHECK_NEAR(summary.span_seconds, 0.0, 1e-9);
}

void testSummaryCurrentPeakTotals() {
  run("summary: current/peak/window totals/span");
  // Rates are the same numbers as the cumulative counters here only for
  // convenience; the summary reads each ring independently.
  NetworkTrafficSeries series;
  series.identity = "idx:1";
  series.display_name = "eth0";
  series.rx_bytes_per_second =
      makeRing({{0.0, 10.0}, {10.0, 20.0}, {20.0, 50.0}, {30.0, 100.0}});
  series.tx_bytes_per_second =
      makeRing({{0.0, 5.0}, {10.0, 5.0}, {20.0, 5.0}, {30.0, 5.0}});
  series.rx_bytes_total =
      makeRing({{0.0, 1000.0}, {10.0, 1500.0}, {20.0, 2100.0}, {30.0, 2900.0}});
  series.tx_bytes_total =
      makeRing({{0.0, 500.0}, {10.0, 500.0}, {20.0, 500.0}, {30.0, 500.0}});
  series.aggregate = false;
  series.membership_changed = false;

  const atm::NetworkTrafficSummary summary = atm::summarizeNetworkTrafficSeries(series);
  CHECK(summary.has_rate_data);
  CHECK_NEAR(summary.rx_current, 100.0, 1e-9);
  CHECK_NEAR(summary.rx_peak, 100.0, 1e-9);
  CHECK_NEAR(summary.tx_current, 5.0, 1e-9);
  CHECK_NEAR(summary.tx_peak, 5.0, 1e-9);
  CHECK(summary.rx_window_total.has_value());
  CHECK_NEAR(*summary.rx_window_total, 1900.0, 1e-9);
  // A delta of exactly zero with enough samples is honest, not "unavailable".
  CHECK(summary.tx_window_total.has_value());
  CHECK_NEAR(*summary.tx_window_total, 0.0, 1e-9);
  CHECK(summary.sample_count == 4);
  CHECK_NEAR(summary.span_seconds, 30.0, 1e-6);
  CHECK(!summary.aggregate);
  CHECK(!summary.membership_changed);
}

void testSummaryWindowTotalReset() {
  run("summary: decreasing cumulative counter -> unavailable");
  NetworkTrafficSeries series;
  series.identity = "idx:1";
  series.rx_bytes_per_second = makeRing({{0.0, 1.0}, {1.0, 2.0}});
  series.tx_bytes_per_second = makeRing({{0.0, 1.0}, {1.0, 2.0}});
  series.rx_bytes_total = makeRing({{0.0, 5000.0}, {1.0, 4000.0}});
  series.tx_bytes_total = makeRing({{0.0, 100.0}, {1.0, 300.0}});
  const atm::NetworkTrafficSummary summary = atm::summarizeNetworkTrafficSeries(series);
  // The RX counters went backwards (reset / wraparound): never shown as bogus.
  CHECK(!summary.rx_window_total.has_value());
  CHECK(summary.tx_window_total.has_value());
  CHECK_NEAR(*summary.tx_window_total, 200.0, 1e-9);
}

void testSummaryAggregateFlags() {
  run("summary: aggregate/membership flags pass through");
  NetworkTrafficSeries series;
  series.identity = std::string(atm::kNetworkTrafficAllIdentity);
  series.aggregate = true;
  series.membership_changed = true;
  const atm::NetworkTrafficSummary summary = atm::summarizeNetworkTrafficSeries(series);
  CHECK(summary.aggregate);
  CHECK(summary.membership_changed);
}

// -------------------------------------------------------------------------
// renderNetworkTrafficChart
// -------------------------------------------------------------------------

void testChartNoData() {
  run("chart: no data");
  NetworkTrafficSeries empty;
  empty.identity = "idx:1";
  atm::NetworkTrafficChartConfig config;
  const std::string output = atm::renderNetworkTrafficChart(
      empty.rx_bytes_per_second, empty.tx_bytes_per_second, config);
  CHECK(output.find("no data") != std::string::npos);
}

void testChartRxDominant() {
  run("chart: RX dominant with readonly scale");
  NetworkTrafficSeries series = makeSeries({
      {0.0, 0.0, 0.0},
      {10.0, 0.0, 0.0},
      {20.0, 100.0, 0.0},
      {30.0, 0.0, 0.0},
      {40.0, 0.0, 0.0},
      {50.0, 0.0, 0.0},
  });
  atm::NetworkTrafficChartConfig config;
  config.data_width = 8;
  config.data_height = 6;
  const std::string output = atm::renderNetworkTrafficChart(
      series.rx_bytes_per_second, series.tx_bytes_per_second, config);

  // Legend explains the glyphs (a11y: not color-dependent).
  CHECK(output.find("RX peak ~") != std::string::npos);
  CHECK(output.find("overlap #/+") != std::string::npos);
  // Shared y-axis: 100 B/s peak with 15% headroom rounds up to 200 B/s.
  CHECK(output.find("200 B/s") != std::string::npos);
  // The RX spike reaches the top row ('~') and fills down its column.
  CHECK(output.find('~') != std::string::npos);
  CHECK(output.find('.') != std::string::npos);
  // The zero-TX line marks only the baseline; no TX peak appears on the top.
  CHECK(output.find('^') != std::string::npos);  // baseline edge of the TX bar
}

void testChartTxDominant() {
  run("chart: TX dominant");
  NetworkTrafficSeries series = makeSeries({
      {0.0, 0.0, 300.0},
      {10.0, 0.0, 300.0},
  });
  atm::NetworkTrafficChartConfig config;
  config.data_width = 4;
  config.data_height = 6;
  const std::string output = atm::renderNetworkTrafficChart(
      series.rx_bytes_per_second, series.tx_bytes_per_second, config);
  // 300 B/s * 1.15 = 345 -> 500 B/s top label; the TX peak sits on the top row.
  CHECK(output.find("500 B/s") != std::string::npos);
  CHECK(output.find('^') != std::string::npos);
  CHECK(output.find(':') != std::string::npos);
}

void testChartOverlap() {
  run("chart: RX and TX overlap");
  NetworkTrafficSeries series = makeSeries({
      {0.0, 100.0, 100.0},
      {10.0, 100.0, 100.0},
  });
  atm::NetworkTrafficChartConfig config;
  config.data_width = 4;
  config.data_height = 6;
  const std::string output = atm::renderNetworkTrafficChart(
      series.rx_bytes_per_second, series.tx_bytes_per_second, config);
  // At the shared peak both directions touch the same row -> '#', and both
  // fills overlap below -> '+'.
  CHECK(output.find('#') != std::string::npos);
  CHECK(output.find('+') != std::string::npos);
}

void testChartTimeAxis() {
  run("chart: relative time axis labels");
  NetworkTrafficSeries series = makeSeries({
      {0.0, 10.0, 2.0},
      {10.0, 20.0, 4.0},
      {20.0, 30.0, 8.0},
      {30.0, 40.0, 16.0},
      {40.0, 50.0, 32.0},
      {50.0, 60.0, 64.0},
      {60.0, 60.0, 64.0},
  });
  atm::NetworkTrafficChartConfig config;
  config.data_width = 12;
  config.data_height = 6;
  const std::string output = atm::renderNetworkTrafficChart(
      series.rx_bytes_per_second, series.tx_bytes_per_second, config);
  CHECK(output.find("-1m00s") != std::string::npos);
  CHECK(output.find("-30s") != std::string::npos);
  CHECK(output.find("Now") != std::string::npos);
}

void testChartDeterministic() {
  run("chart: deterministic output");
  NetworkTrafficSeries series = makeSeries({
      {0.0, 10.0, 5.0},
      {10.0, 50.0, 5.0},
      {20.0, 25.0, 60.0},
      {30.0, 10.0, 5.0},
  });
  atm::NetworkTrafficChartConfig config;
  config.data_width = 8;
  config.data_height = 6;
  const std::string a = atm::renderNetworkTrafficChart(
      series.rx_bytes_per_second, series.tx_bytes_per_second, config);
  const std::string b = atm::renderNetworkTrafficChart(
      series.rx_bytes_per_second, series.tx_bytes_per_second, config);
  CHECK(a == b);
}

// -------------------------------------------------------------------------

int main() {
  testSummaryEmptySeries();
  testSummaryCurrentPeakTotals();
  testSummaryWindowTotalReset();
  testSummaryAggregateFlags();
  testChartNoData();
  testChartRxDominant();
  testChartTxDominant();
  testChartOverlap();
  testChartTimeAxis();
  testChartDeterministic();

  std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}