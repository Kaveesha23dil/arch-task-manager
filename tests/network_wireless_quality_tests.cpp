#include <chrono>
#include <cmath>
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
using atm::WirelessConnectionEvent;
using atm::WirelessConnectionEventType;
using atm::WirelessHistorySample;
using atm::WirelessQualitySummary;
using atm::WirelessPresence;
using atm::WirelessStability;

namespace {

/// Returns a quality summary for the given records, using 120 as the default
/// retention bound.
WirelessQualitySummary summarize(const NetworkWirelessInfo &wireless,
                                 std::size_t max_samples = 120) {
  return atm::summarizeWirelessQuality(wireless, max_samples);
}

/// Helper to build a history sample at a given wall-ms offset.
WirelessHistorySample makeSample(int wall_ms, bool valid,
                                 std::optional<double> signal,
                                 WirelessAssociation association =
                                     WirelessAssociation::Associated) {
  WirelessHistorySample s;
  s.timestamp =
      std::chrono::steady_clock::time_point{} + std::chrono::milliseconds(wall_ms);
  s.association = association;
  s.valid = valid;
  s.signal_dbm = signal;
  s.is_mac80211 = true;
  return s;
}

/// Helper to build a history sample with bitrate.
WirelessHistorySample makeBitrateSample(int wall_ms, bool valid,
                                        std::optional<double> signal,
                                        std::optional<double> bitrate,
                                        WirelessAssociation association =
                                            WirelessAssociation::Associated) {
  WirelessHistorySample s = makeSample(wall_ms, valid, signal, association);
  s.bitrate_bps = bitrate;
  return s;
}

/// Helper to build a connection event.
WirelessConnectionEvent makeEvent(WirelessConnectionEventType type,
                                  const std::string &iface = "wlan0",
                                  bool ap_reliable = false) {
  WirelessConnectionEvent e;
  e.type = type;
  e.interface_name = iface;
  e.ap_identity_reliable = ap_reliable;
  e.confident = true;
  e.source = "carrier";
  return e;
}

}  // namespace

// -------------------------------------------------------------------------
// Signal statistics
// -------------------------------------------------------------------------

void test_signal_stats() {
  run("signal stats");

  NetworkWirelessInfo w;
  w.presence = WirelessPresence::Wireless;
  w.history = ResourceHistory<WirelessHistorySample>(16);

  w.history.addSample(makeSample(0, true, -48.0));
  w.history.addSample(makeSample(1000, true, -45.0));
  w.history.addSample(makeSample(2000, true, -42.0));

  const auto q = summarize(w);
  CHECK(q.has_data);
  CHECK(q.signal_sample_count == 3);
  CHECK(q.current_signal_dbm.has_value());
  CHECK(*q.current_signal_dbm == -42.0);
  CHECK(q.min_signal_dbm.has_value());
  CHECK(*q.min_signal_dbm == -48.0);
  CHECK(q.max_signal_dbm.has_value());
  CHECK(*q.max_signal_dbm == -42.0);
  CHECK(q.avg_signal_dbm.has_value());
  CHECK(*q.avg_signal_dbm == -45.0);
}

// -------------------------------------------------------------------------
// Standard deviation (population stddev)
// -------------------------------------------------------------------------

void test_signal_stddev() {
  run("signal stddev");

  NetworkWirelessInfo w;
  w.presence = WirelessPresence::Wireless;
  w.history = ResourceHistory<WirelessHistorySample>(16);

  // Values: -40, -44, -48 → mean -44, variance (16+0+16)/3 = 10.666..., stddev ≈ 3.266
  w.history.addSample(makeSample(0, true, -40.0));
  w.history.addSample(makeSample(1000, true, -44.0));
  w.history.addSample(makeSample(2000, true, -48.0));

  const auto q = summarize(w);
  CHECK(q.signal_stddev_dbm.has_value());
  // Population stddev of [-40, -44, -48] = sqrt((16+0+16)/3) ≈ 3.266
  const double expected = std::sqrt(32.0 / 3.0);
  CHECK(std::abs(*q.signal_stddev_dbm - expected) < 0.01);
}

// -------------------------------------------------------------------------
// Bitrate stats
// -------------------------------------------------------------------------

void test_bitrate_stats() {
  run("bitrate stats");

  NetworkWirelessInfo w;
  w.presence = WirelessPresence::Wireless;
  w.history = ResourceHistory<WirelessHistorySample>(16);

  w.history.addSample(makeBitrateSample(0, true, -45.0, 54000000.0));
  w.history.addSample(makeBitrateSample(1000, true, -42.0, 866700000.0));
  w.history.addSample(makeBitrateSample(2000, true, -40.0, 1500000000.0));

  const auto q = summarize(w);
  CHECK(q.bitrate_sample_count == 3);
  CHECK(q.current_bitrate_bps.has_value());
  CHECK(*q.current_bitrate_bps == 1500000000.0);
  CHECK(q.min_bitrate_bps.has_value());
  CHECK(*q.min_bitrate_bps == 54000000.0);
  CHECK(q.max_bitrate_bps.has_value());
  CHECK(*q.max_bitrate_bps == 1500000000.0);
  CHECK(q.avg_bitrate_bps.has_value());
  const double expected_avg = (54000000.0 + 866700000.0 + 1500000000.0) / 3.0;
  CHECK(std::abs(*q.avg_bitrate_bps - expected_avg) < 1.0);
}

// -------------------------------------------------------------------------
// Coverage
// -------------------------------------------------------------------------

void test_coverage() {
  run("coverage");

  NetworkWirelessInfo w;
  w.presence = WirelessPresence::Wireless;
  w.history = ResourceHistory<WirelessHistorySample>(10);

  for (int i = 0; i < 4; ++i) {
    w.history.addSample(makeSample(i * 1000, true, -45.0));
  }

  const auto q = summarize(w, 10);
  CHECK(q.sample_count == 4);
  CHECK(q.valid_sample_count == 4);
  CHECK(q.coverage == 0.4);
  CHECK(q.valid_coverage == 0.4);
  CHECK(q.valid_with_signal == 1.0);
}

// -------------------------------------------------------------------------
// Missing/invalid metric values → excluded, coverage < 1
// -------------------------------------------------------------------------

void test_missing_metrics_excluded() {
  run("missing metrics excluded");

  NetworkWirelessInfo w;
  w.presence = WirelessPresence::Wireless;
  w.history = ResourceHistory<WirelessHistorySample>(10);

  w.history.addSample(makeSample(0, true, std::nullopt));
  w.history.addSample(makeSample(1000, true, -45.0));
  w.history.addSample(makeSample(2000, true, std::nullopt));

  const auto q = summarize(w);
  CHECK(q.signal_sample_count == 1);
  CHECK(*q.avg_signal_dbm == -45.0);
  CHECK(q.valid_with_signal < 1.0);
}

// -------------------------------------------------------------------------
// Connected/disconnected duration attribution
// -------------------------------------------------------------------------

void test_connectivity_durations() {
  run("connectivity durations");

  NetworkWirelessInfo w;
  w.presence = WirelessPresence::Wireless;
  w.history = ResourceHistory<WirelessHistorySample>(16);

  // 0-30s associated, 30-50s still-associated (no change recorded until 50s),
  // 50-80s disconnected. An interval is attributed to the state recorded at its
  // start (the earlier sample): it persists until the next recorded state.
  w.history.addSample(makeSample(0, true, -45.0, WirelessAssociation::Associated));
  w.history.addSample(makeSample(30000, true, -44.0, WirelessAssociation::Associated));
  w.history.addSample(makeSample(50000, true, -46.0, WirelessAssociation::Disconnected));
  w.history.addSample(makeSample(80000, true, -43.0, WirelessAssociation::Associated));

  const auto q = summarize(w);
  CHECK(q.connected_seconds == 50.0);    // 0-50s
  CHECK(q.disconnected_seconds == 30.0); // 50-80s
  CHECK(q.longest_connected_seconds == 50.0);
  CHECK(q.longest_disconnected_seconds == 30.0);
}

// -------------------------------------------------------------------------
// Longest connected period with a disconnected break
// -------------------------------------------------------------------------

void test_longest_connected_with_break() {
  run("longest connected period with break");

  NetworkWirelessInfo w;
  w.presence = WirelessPresence::Wireless;
  w.history = ResourceHistory<WirelessHistorySample>(16);

  // 0-10s assoc, 10-15s still-assoc, 15-40s disconnected. The disconnected
  // sample at 15s marks the start of the disconnected interval (earlier-state
  // attribution) and breaks the connected run at 15s.
  w.history.addSample(makeSample(0, true, -45.0, WirelessAssociation::Associated));
  w.history.addSample(makeSample(10000, true, -44.0, WirelessAssociation::Associated));
  w.history.addSample(makeSample(15000, true, -46.0, WirelessAssociation::Disconnected));
  w.history.addSample(makeSample(40000, true, -43.0, WirelessAssociation::Associated));

  const auto q = summarize(w);
  CHECK(q.longest_connected_seconds == 15.0);   // 0-15s
  CHECK(q.longest_disconnected_seconds == 25.0); // 15-40s
}

// -------------------------------------------------------------------------
// Long gaps between samples → unobserved, run break
// -------------------------------------------------------------------------

void test_long_gap_unobserved() {
  run("long gap unobserved");

  NetworkWirelessInfo w;
  w.presence = WirelessPresence::Wireless;
  w.history = ResourceHistory<WirelessHistorySample>(16);

  // 0s assoc, 35s assoc (gap > 30s threshold)
  w.history.addSample(makeSample(0, true, -45.0, WirelessAssociation::Associated));
  w.history.addSample(makeSample(35000, true, -43.0, WirelessAssociation::Associated));

  const auto q = summarize(w);
  CHECK(q.connected_seconds == 0.0);   // gap > 30s → unobserved, not connected
  CHECK(q.unobserved_seconds == 35.0);
  CHECK(q.longest_connected_seconds == 0.0);
}

// -------------------------------------------------------------------------
// Disconnection/reconnection counts from events
// -------------------------------------------------------------------------

void test_event_counts() {
  run("event counts");

  NetworkWirelessInfo w;
  w.presence = WirelessPresence::Wireless;
  w.history = ResourceHistory<WirelessHistorySample>(16);
  w.history.addSample(makeSample(0, true, -45.0));
  w.connection_events = ResourceHistory<WirelessConnectionEvent>(64);

  w.connection_events.addSample(makeEvent(WirelessConnectionEventType::Disassociated));
  w.connection_events.addSample(makeEvent(WirelessConnectionEventType::Disassociated));
  w.connection_events.addSample(makeEvent(WirelessConnectionEventType::Reconnected));
  w.connection_events.addSample(makeEvent(WirelessConnectionEventType::Roamed));
  w.connection_events.addSample(
      makeEvent(WirelessConnectionEventType::InterfaceUnavailable));
  w.connection_events.addSample(
      makeEvent(WirelessConnectionEventType::InterfaceUnavailable));

  const auto q = summarize(w);
  CHECK(q.disconnection_count == 2);
  CHECK(q.reconnection_count == 1);
  CHECK(q.roaming_count == 1);
  CHECK(q.interface_unavailable_count == 2);
}

// -------------------------------------------------------------------------
// Roaming count
// -------------------------------------------------------------------------

void test_roaming_count() {
  run("roaming count");

  NetworkWirelessInfo w;
  w.presence = WirelessPresence::Wireless;
  w.history = ResourceHistory<WirelessHistorySample>(16);
  w.history.addSample(makeSample(0, true, -45.0));
  w.connection_events = ResourceHistory<WirelessConnectionEvent>(64);

  w.connection_events.addSample(makeEvent(WirelessConnectionEventType::Roamed));
  w.connection_events.addSample(makeEvent(WirelessConnectionEventType::Roamed));

  const auto q = summarize(w);
  CHECK(q.roaming_count == 2);
}

// -------------------------------------------------------------------------
// Temporary data gaps (invalid ticks)
// -------------------------------------------------------------------------

void test_temporary_gaps() {
  run("temporary data gaps");

  NetworkWirelessInfo w;
  w.presence = WirelessPresence::Wireless;
  w.history = ResourceHistory<WirelessHistorySample>(16);

  w.history.addSample(makeSample(0, true, -45.0));
  w.history.addSample(makeSample(1000, false, std::nullopt));
  w.history.addSample(makeSample(2000, true, -42.0));
  w.history.addSample(makeSample(3000, false, std::nullopt));

  const auto q = summarize(w);
  CHECK(q.temporary_gap_count == 2);
  CHECK(q.valid_sample_count == 2);
}

// -------------------------------------------------------------------------
// Empty history → has_data false
// -------------------------------------------------------------------------

void test_empty_history() {
  run("empty history");

  NetworkWirelessInfo w;
  w.presence = WirelessPresence::Wireless;
  w.history = ResourceHistory<WirelessHistorySample>(16);

  const auto q = summarize(w);
  CHECK(!q.has_data);
  CHECK(q.sample_count == 0);
  CHECK(q.assessment.stability == WirelessStability::Unknown);
}

// -------------------------------------------------------------------------
// Non-wireless interface
// -------------------------------------------------------------------------

void test_non_wireless() {
  run("non-wireless interface");

  NetworkWirelessInfo w;
  w.presence = WirelessPresence::NotWireless;

  const auto q = summarize(w);
  CHECK(!q.has_data);
  const auto rendered = atm::renderWirelessQualitySummary(w, 120);
  CHECK(rendered.find("not a wireless interface") != std::string::npos);
}

// -------------------------------------------------------------------------
// Classification: sufficient data → not Unknown
// -------------------------------------------------------------------------

void test_classification_sufficient_data() {
  run("classification sufficient data");

  NetworkWirelessInfo w;
  w.presence = WirelessPresence::Wireless;
  w.history = ResourceHistory<WirelessHistorySample>(16);
  w.connection_events = ResourceHistory<WirelessConnectionEvent>(64);

  // 10 samples, all valid, associated, steady 1s apart, good signal.
  for (int i = 0; i < 10; ++i) {
    w.history.addSample(makeSample(i * 1000, true, -40.0));
  }
  // A couple of disconnections and reconnections to exercise factors.
  w.connection_events.addSample(makeEvent(WirelessConnectionEventType::Disassociated));
  w.connection_events.addSample(makeEvent(WirelessConnectionEventType::Reconnected));

  const auto q = summarize(w);
  CHECK(q.has_data);
  CHECK(q.assessment.stability != WirelessStability::Unknown);
  CHECK(q.assessment.score.has_value());
  CHECK(*q.assessment.score >= 0.0);
  CHECK(*q.assessment.score <= 100.0);
}

// -------------------------------------------------------------------------
// Classification: insufficient data → Unknown
// -------------------------------------------------------------------------

void test_classification_insufficient_data() {
  run("classification insufficient data");

  NetworkWirelessInfo w;
  w.presence = WirelessPresence::Wireless;
  w.history = ResourceHistory<WirelessHistorySample>(16);

  // Only 2 valid samples, never associated → no determined time.
  w.history.addSample(makeSample(0, true, -45.0, WirelessAssociation::Unknown));
  w.history.addSample(makeSample(1000, true, -43.0, WirelessAssociation::Unknown));

  const auto q = summarize(w);
  CHECK(q.has_data);
  CHECK(q.assessment.stability == WirelessStability::Unknown);
  CHECK(!q.assessment.score.has_value());
}

// -------------------------------------------------------------------------
// Classification boundaries
// -------------------------------------------------------------------------

void test_classification_boundary_excellent() {
  run("classification boundary excellent");

  NetworkWirelessInfo w;
  w.presence = WirelessPresence::Wireless;
  w.history = ResourceHistory<WirelessHistorySample>(16);

  // 20 good samples, steady 1s apart.
  for (int i = 0; i < 20; ++i) {
    w.history.addSample(makeSample(i * 1000, true, -35.0));
  }

  const auto result = summarize(w);
  CHECK(result.assessment.stability != WirelessStability::Unknown);
  CHECK(result.assessment.score.has_value());
  // With very good signal, no disconnections, no outages → should be Good or Excellent.
  CHECK(*result.assessment.score >= 60.0);
}

void test_classification_boundary_poor() {
  run("classification boundary poor");

  NetworkWirelessInfo w;
  w.presence = WirelessPresence::Wireless;
  w.history = ResourceHistory<WirelessHistorySample>(16);

  // 10 samples: half disconnected, very bad signal, many disconnections.
  w.history.addSample(makeSample(0, true, -85.0, WirelessAssociation::Associated));
  w.history.addSample(makeSample(1000, true, -88.0, WirelessAssociation::Disconnected));
  w.history.addSample(makeSample(2000, true, -87.0, WirelessAssociation::Associated));
  w.history.addSample(makeSample(3000, true, -89.0, WirelessAssociation::Disconnected));
  w.history.addSample(makeSample(4000, true, -86.0, WirelessAssociation::Associated));
  w.history.addSample(makeSample(5000, true, -88.0, WirelessAssociation::Disconnected));
  w.history.addSample(makeSample(6000, true, -85.0, WirelessAssociation::Associated));
  w.history.addSample(makeSample(7000, true, -89.0, WirelessAssociation::Disconnected));
  w.history.addSample(makeSample(8000, true, -87.0, WirelessAssociation::Associated));
  w.history.addSample(makeSample(9000, true, -88.0, WirelessAssociation::Disconnected));

  const auto q = summarize(w);
  CHECK(q.assessment.stability != WirelessStability::Unknown);
  CHECK(q.assessment.score.has_value());
  // Poor signal + many disconnects → should be Poor or Fair.
  CHECK(*q.assessment.score < 60.0);
}

// -------------------------------------------------------------------------
// Deterministic score
// -------------------------------------------------------------------------

void test_deterministic_score() {
  run("deterministic score");

  NetworkWirelessInfo w;
  w.presence = WirelessPresence::Wireless;
  w.history = ResourceHistory<WirelessHistorySample>(16);

  for (int i = 0; i < 10; ++i) {
    w.history.addSample(makeSample(i * 1000, true, -45.0));
  }

  const auto q1 = summarize(w);
  const auto q2 = summarize(w);
  CHECK(q1.assessment.score.has_value());
  CHECK(q2.assessment.score.has_value());
  CHECK(*q1.assessment.score == *q2.assessment.score);
}

// -------------------------------------------------------------------------
// UI rendering: wireless and non-wireless interfaces
// -------------------------------------------------------------------------

void test_render_quality_summary_wireless() {
  run("render quality summary wireless");

  NetworkWirelessInfo w;
  w.presence = WirelessPresence::Wireless;
  w.history = ResourceHistory<WirelessHistorySample>(16);

  for (int i = 0; i < 10; ++i) {
    w.history.addSample(makeSample(i * 1000, true, -45.0));
  }

  const auto rendered = atm::renderWirelessQualitySummary(w, 16);
  CHECK(rendered.find("Wireless connection quality summary:") != std::string::npos);
  CHECK(rendered.find("Stability:") != std::string::npos);
  CHECK(rendered.find("Signal:") != std::string::npos);
  CHECK(rendered.find("Connectivity:") != std::string::npos);
  CHECK(rendered.find("Coverage:") != std::string::npos);
}

void test_render_quality_summary_insufficient() {
  run("render quality summary insufficient");

  NetworkWirelessInfo w;
  w.presence = WirelessPresence::Wireless;
  w.history = ResourceHistory<WirelessHistorySample>(16);

  // Only 2 samples, Unknown association → insufficient data.
  w.history.addSample(makeSample(0, true, -45.0, WirelessAssociation::Unknown));
  w.history.addSample(makeSample(1000, true, -43.0, WirelessAssociation::Unknown));

  const auto rendered = atm::renderWirelessQualitySummary(w, 16);
  CHECK(rendered.find("limited") != std::string::npos);
}

void test_render_quality_summary_no_data() {
  run("render quality summary no data");

  NetworkWirelessInfo w;
  w.presence = WirelessPresence::Wireless;
  w.history = ResourceHistory<WirelessHistorySample>(16);

  const auto rendered = atm::renderWirelessQualitySummary(w, 16);
  CHECK(rendered.find("no data") != std::string::npos);
}

// -------------------------------------------------------------------------
// Retention-window behaviour
// -------------------------------------------------------------------------

void test_retention_window() {
  run("retention window");

  NetworkWirelessInfo w;
  w.presence = WirelessPresence::Wireless;
  w.history = ResourceHistory<WirelessHistorySample>(5);

  // Add 5 samples to fill the ring.
  for (int i = 0; i < 5; ++i) {
    w.history.addSample(makeSample(i * 1000, true, -45.0));
  }
  w.last_sample_wall = std::chrono::system_clock::now();

  const auto q = summarize(w, 5);
  CHECK(q.sample_count == 5);
  CHECK(q.coverage == 1.0);
}

// -------------------------------------------------------------------------
// Suspend/resume handling
// -------------------------------------------------------------------------

void test_suspend_resume() {
  run("suspend/resume handling");

  NetworkWirelessInfo w;
  w.presence = WirelessPresence::Wireless;
  w.history = ResourceHistory<WirelessHistorySample>(16);

  // 0-5s associated, 5-100s gap (suspend), 100-105s associated
  w.history.addSample(makeSample(0, true, -45.0, WirelessAssociation::Associated));
  w.history.addSample(makeSample(5000, true, -44.0, WirelessAssociation::Associated));
  w.history.addSample(makeSample(100000, true, -43.0, WirelessAssociation::Associated));
  w.history.addSample(makeSample(105000, true, -42.0, WirelessAssociation::Associated));

  const auto q = summarize(w);
  CHECK(q.unobserved_seconds > 0.0);  // 95s gap is unobserved
  CHECK(q.connected_seconds == 10.0); // 0-5s + 100-105s
}

// -------------------------------------------------------------------------
// Interface disappearance & recreation
// -------------------------------------------------------------------------

void test_interface_recreation() {
  run("interface disappearance & recreation");

  NetworkWirelessInfo w;
  w.presence = WirelessPresence::Wireless;
  w.history = ResourceHistory<WirelessHistorySample>(16);

  // First lifecycle: 5 good samples.
  for (int i = 0; i < 5; ++i) {
    w.history.addSample(makeSample(i * 1000, true, -45.0));
  }
  const auto q1 = summarize(w);
  CHECK(q1.sample_count == 5);

  // Simulate identity recreation: clear history, start fresh.
  w.history = ResourceHistory<WirelessHistorySample>(16);
  for (int i = 0; i < 3; ++i) {
    w.history.addSample(makeSample(i * 1000, true, -50.0));
  }
  const auto q2 = summarize(w);
  CHECK(q2.sample_count == 3);
  CHECK(*q2.avg_signal_dbm == -50.0);
}

// -------------------------------------------------------------------------
// Unknown/unavailable association states → break runs
// -------------------------------------------------------------------------

void test_unknown_state_breaks_run() {
  run("unknown state breaks run");

  NetworkWirelessInfo w;
  w.presence = WirelessPresence::Wireless;
  w.history = ResourceHistory<WirelessHistorySample>(16);

  // 0-10s assoc, 10-15s still-assoc, 15-25s unknown (association undetermined),
  // which is unobserved and breaks the running connected duration at 15s.
  w.history.addSample(makeSample(0, true, -45.0, WirelessAssociation::Associated));
  w.history.addSample(makeSample(10000, true, -44.0, WirelessAssociation::Associated));
  w.history.addSample(makeSample(15000, true, std::nullopt, WirelessAssociation::Unknown));
  w.history.addSample(makeSample(25000, true, -43.0, WirelessAssociation::Associated));

  const auto q = summarize(w);
  CHECK(q.connected_seconds == 15.0);  // 0-15s
  CHECK(q.unobserved_seconds == 10.0); // 15-25s Unknown → unobserved
  CHECK(q.longest_connected_seconds == 15.0);
}

// -------------------------------------------------------------------------
// Good signal + no disconnects → Good or Excellent
// -------------------------------------------------------------------------

void test_good_signal_no_disconnects() {
  run("good signal no disconnects");

  NetworkWirelessInfo w;
  w.presence = WirelessPresence::Wireless;
  w.history = ResourceHistory<WirelessHistorySample>(16);

  // 12 samples, good signal, all associated, steady 1s apart.
  for (int i = 0; i < 12; ++i) {
    w.history.addSample(makeSample(i * 1000, true, -38.0));
  }

  const auto q = summarize(w);
  CHECK(q.assessment.stability != WirelessStability::Unknown);
  CHECK(q.assessment.score.has_value());
  CHECK(*q.assessment.score >= 60.0);  // Good or Excellent
}

// -------------------------------------------------------------------------
// Low-coverage → Unknown classification
// -------------------------------------------------------------------------

void test_low_coverage_unknown() {
  run("low coverage unknown classification");

  NetworkWirelessInfo w;
  w.presence = WirelessPresence::Wireless;
  w.history = ResourceHistory<WirelessHistorySample>(120);

  // Only 3 valid samples (below kWirelessQualityMinValidSamples = 5).
  for (int i = 0; i < 3; ++i) {
    w.history.addSample(makeSample(i * 1000, true, -45.0));
  }

  const auto q = summarize(w, 120);
  CHECK(q.has_data);
  CHECK(q.assessment.stability == WirelessStability::Unknown);
  CHECK(!q.assessment.score.has_value());
}

// -------------------------------------------------------------------------
// Stability name
// -------------------------------------------------------------------------

void test_stability_names() {
  run("stability names");

  CHECK(std::string(atm::wirelessStabilityName(WirelessStability::Unknown)) == "unknown");
  CHECK(std::string(atm::wirelessStabilityName(WirelessStability::Poor)) == "poor");
  CHECK(std::string(atm::wirelessStabilityName(WirelessStability::Fair)) == "fair");
  CHECK(std::string(atm::wirelessStabilityName(WirelessStability::Good)) == "good");
  CHECK(std::string(atm::wirelessStabilityName(WirelessStability::Excellent)) == "excellent");
}

// -------------------------------------------------------------------------
// Single sample → no connectivity durations
// -------------------------------------------------------------------------

void test_single_sample() {
  run("single sample");

  NetworkWirelessInfo w;
  w.presence = WirelessPresence::Wireless;
  w.history = ResourceHistory<WirelessHistorySample>(16);

  w.history.addSample(makeSample(0, true, -45.0));

  const auto q = summarize(w);
  CHECK(q.has_data);
  CHECK(q.sample_count == 1);
  CHECK(q.connected_seconds == 0.0);
  CHECK(q.disconnected_seconds == 0.0);
  CHECK(q.unobserved_seconds == 0.0);
  CHECK(q.span_seconds == 0.0);
}

// -------------------------------------------------------------------------
// main
// -------------------------------------------------------------------------

int main() {
  test_signal_stats();
  test_signal_stddev();
  test_bitrate_stats();
  test_coverage();
  test_missing_metrics_excluded();
  test_connectivity_durations();
  test_longest_connected_with_break();
  test_long_gap_unobserved();
  test_event_counts();
  test_roaming_count();
  test_temporary_gaps();
  test_empty_history();
  test_non_wireless();
  test_classification_sufficient_data();
  test_classification_insufficient_data();
  test_classification_boundary_excellent();
  test_classification_boundary_poor();
  test_deterministic_score();
  test_render_quality_summary_wireless();
  test_render_quality_summary_insufficient();
  test_render_quality_summary_no_data();
  test_retention_window();
  test_suspend_resume();
  test_interface_recreation();
  test_unknown_state_breaks_run();
  test_good_signal_no_disconnects();
  test_low_coverage_unknown();
  test_stability_names();
  test_single_sample();

  std::fprintf(stderr, "PASS: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
