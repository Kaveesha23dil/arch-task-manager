#include <chrono>
#include <cstdio>
#include <optional>
#include <string>

#include "network_wireless.hpp"
#include "resource_history.hpp"

// --- Minimal standalone test harness (mirrors the sibling test files) -------
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
void run(const char *name) { std::fprintf(stderr, "TEST %s\n", name); }
}  // namespace

#define CHECK(expr) ::expect((expr), #expr, __FILE__, __LINE__)

using atm::NetworkWirelessInfo;
using atm::ResourceHistory;
using atm::WirelessAssociation;
using atm::WirelessHistorySample;
using atm::WirelessHistorySummary;
using atm::WirelessPresence;

// -------------------------------------------------------------------------
// Step 50 metric formatters: canonical bits/s, MHz, channel
// -------------------------------------------------------------------------

void test_format_bitrate() {
  run("formatWirelessBitrate");

  CHECK(atm::formatWirelessBitrate(std::nullopt) == "N/A");
  CHECK(atm::formatWirelessBitrate(std::optional<double>(-1.0)) == "N/A");

  CHECK(atm::formatWirelessBitrate(std::optional<double>(0.0)) == "0 b/s");
  CHECK(atm::formatWirelessBitrate(std::optional<double>(999.0)) == "999 b/s");
  CHECK(atm::formatWirelessBitrate(std::optional<double>(1000.0)) == "1 Kb/s");
  CHECK(atm::formatWirelessBitrate(std::optional<double>(54000000.0)) ==
        "54 Mb/s");
  CHECK(atm::formatWirelessBitrate(std::optional<double>(5400000.0)) ==
        "5.4 Mb/s");
  CHECK(atm::formatWirelessBitrate(std::optional<double>(866700000.0)) ==
        "866.7 Mb/s");
  CHECK(atm::formatWirelessBitrate(std::optional<double>(1500000000.0)) ==
        "1.5 Gb/s");
  CHECK(atm::formatWirelessBitrate(std::optional<double>(65000000.0)) ==
        "65 Mb/s");
}

void test_format_frequency_and_channel() {
  run("formatWirelessFrequency + channel");

  CHECK(atm::formatWirelessFrequency(std::nullopt) == "N/A");
  CHECK(atm::formatWirelessFrequency(std::optional<double>(2412.0)) == "2412 MHz");
  CHECK(atm::formatWirelessFrequency(std::optional<double>(5180.0)) == "5180 MHz");
  CHECK(atm::formatWirelessFrequency(std::optional<double>(2437.0)) == "2437 MHz");
  CHECK(atm::formatWirelessFrequency(std::optional<double>(2422.0)) == "2422 MHz");

  CHECK(atm::formatWirelessChannel(std::nullopt) == "N/A");
  CHECK(atm::formatWirelessChannel(std::optional<int>(6)) == "6");
  CHECK(atm::formatWirelessChannel(std::optional<int>(36)) == "36");
}

// -------------------------------------------------------------------------
// summarizeWirelessHistory: pure statistics over the retained ring
// -------------------------------------------------------------------------

WirelessHistorySample makeSample(int delta_ms, bool valid,
                                 std::optional<double> signal,
                                 std::optional<int> quality,
                                 bool is_mac80211 = true) {
  WirelessHistorySample sample;
  sample.timestamp =
      std::chrono::steady_clock::time_point{} +
      std::chrono::milliseconds(delta_ms);
  sample.association = WirelessAssociation::Associated;
  sample.valid = valid;
  sample.is_mac80211 = is_mac80211;
  sample.signal_dbm = signal;
  sample.link_quality = quality;
  return sample;
}

void test_summary_over_ring() {
  run("summary over ring");

  NetworkWirelessInfo wireless;
  wireless.presence = WirelessPresence::Wireless;
  wireless.history = ResourceHistory<WirelessHistorySample>(16);

  // Negative dBm preserved; newest last; one invalid gap sample stays nullopt.
  wireless.history.addSample(makeSample(0, true, -48, 50));
  wireless.history.addSample(makeSample(1000, true, -45, 55));
  wireless.history.addSample(
      makeSample(2000, false, std::nullopt, std::nullopt));  // gap
  wireless.history.addSample(makeSample(3000, true, -42, 58));
  wireless.history.addSample(makeSample(4000, true, -42, 60));

  const WirelessHistorySummary summary =
      atm::summarizeWirelessHistory(wireless, 16);

  CHECK(summary.has_data);
  CHECK(summary.sample_count == 5);
  CHECK(summary.valid_sample_count == 4);
  CHECK(summary.signal_sample_count == 4);  // invalid sample contributes nothing
  CHECK(summary.link_quality_sample_count == 4);

  CHECK(summary.current_signal_dbm.has_value());
  CHECK(*summary.current_signal_dbm == -42.0);
  CHECK(summary.min_signal_dbm.has_value());
  CHECK(*summary.min_signal_dbm == -48.0);
  CHECK(summary.max_signal_dbm.has_value());
  CHECK(*summary.max_signal_dbm == -42.0);
  CHECK(summary.avg_signal_dbm.has_value());
  CHECK(*summary.avg_signal_dbm == -44.25);

  CHECK(summary.current_link_quality.has_value());
  CHECK(*summary.current_link_quality == 60);
  CHECK(summary.min_link_quality.has_value());
  CHECK(*summary.min_link_quality == 50);
  CHECK(summary.max_link_quality.has_value());
  CHECK(*summary.max_link_quality == 60);
  CHECK(summary.avg_link_quality.has_value());
  CHECK(*summary.avg_link_quality == 55.75);

  CHECK(summary.coverage == 5.0 / 16.0);
  CHECK(summary.valid_coverage == 4.0 / 16.0);
  CHECK(!summary.history_complete);
  CHECK(summary.span_seconds == 4.0);
}

void test_summary_empty_and_complete() {
  run("summary empty + complete");

  NetworkWirelessInfo empty;
  empty.presence = WirelessPresence::Wireless;
  empty.history = ResourceHistory<WirelessHistorySample>(4);
  const WirelessHistorySummary none =
      atm::summarizeWirelessHistory(empty, 4);
  CHECK(!none.has_data);
  CHECK(!none.current_signal_dbm.has_value());
  CHECK(!none.min_signal_dbm.has_value());

  // A full ring reports history_complete and a coverage of exactly 1.0.
  NetworkWirelessInfo full;
  full.presence = WirelessPresence::Wireless;
  full.history = ResourceHistory<WirelessHistorySample>(4);
  for (int i = 0; i < 4; ++i) {
    full.history.addSample(makeSample(i * 250, true, -40.0, 60));
  }
  const WirelessHistorySummary complete =
      atm::summarizeWirelessHistory(full, 4);
  CHECK(complete.history_complete);
  CHECK(complete.coverage == 1.0);
  CHECK(complete.valid_coverage == 1.0);
  CHECK(complete.sample_count == 4);
  CHECK(*complete.min_signal_dbm == -40.0);
  CHECK(*complete.max_signal_dbm == -40.0);
  CHECK(complete.span_seconds == 0.75);
}

void test_summary_bitrate_frequency_channel() {
  run("summary bitrate/frequency/channel pass-through");

  NetworkWirelessInfo wireless;
  wireless.presence = WirelessPresence::Wireless;
  wireless.history = ResourceHistory<WirelessHistorySample>(4);

  // In production bitrate/frequency/channel stay nullopt; the model must still
  // surface deterministic fixtures end-to-end (first-class export fields).
  WirelessHistorySample sample = makeSample(0, true, -45, 52);
  sample.bitrate_bps = 866700000.0;
  sample.frequency_mhz = 5180.0;
  sample.channel = 36;
  wireless.history.addSample(sample);

  const WirelessHistorySummary summary =
      atm::summarizeWirelessHistory(wireless, 16);
  CHECK(summary.bitrate_sample_count == 1);
  CHECK(summary.current_bitrate_bps.has_value());
  CHECK(*summary.current_bitrate_bps == 866700000.0);
  CHECK(*summary.min_bitrate_bps == 866700000.0);
  CHECK(*summary.max_bitrate_bps == 866700000.0);
  CHECK(*summary.avg_bitrate_bps == 866700000.0);

  CHECK(summary.frequency_sample_count == 1);
  CHECK(summary.current_frequency_mhz.has_value());
  CHECK(*summary.current_frequency_mhz == 5180.0);
  CHECK(*summary.min_frequency_mhz == 5180.0);
  CHECK(*summary.max_frequency_mhz == 5180.0);

  CHECK(summary.channel_sample_count == 1);
  CHECK(summary.current_channel.has_value());
  CHECK(*summary.current_channel == 36);

  CHECK(atm::formatWirelessBitrate(summary.current_bitrate_bps) == "866.7 Mb/s");
  CHECK(atm::formatWirelessFrequency(summary.current_frequency_mhz) == "5180 MHz");
  CHECK(atm::formatWirelessChannel(summary.current_channel) == "36");
}

void test_summary_transition_count() {
  run("summary transition count");

  NetworkWirelessInfo wireless;
  wireless.presence = WirelessPresence::Wireless;
  wireless.history = ResourceHistory<WirelessHistorySample>(4);
  wireless.history.addSample(makeSample(0, true, -45, 52));
  wireless.transitions =
      ResourceHistory<atm::WirelessTransitionEvent>(atm::kMaxWirelessTransitions);
  wireless.transitions.addSample(atm::WirelessTransitionEvent{
      {}, {}, WirelessAssociation::Associated, WirelessAssociation::Disconnected});
  wireless.last_sample_wall = std::chrono::system_clock::now();

  const WirelessHistorySummary summary =
      atm::summarizeWirelessHistory(wireless, 16);
  CHECK(summary.transition_count == 1);
  CHECK(summary.last_update == wireless.last_sample_wall);
}

int main() {
  test_format_bitrate();
  test_format_frequency_and_channel();
  test_summary_over_ring();
  test_summary_empty_and_complete();
  test_summary_bitrate_frequency_channel();
  test_summary_transition_count();

  std::fprintf(stderr, "PASS: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}