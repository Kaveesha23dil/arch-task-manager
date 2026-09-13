#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "alert_manager.hpp"
#include "network_interface_details.hpp"
#include "network_traffic_alert.hpp"
#include "network_traffic_history.hpp"

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
void expectNear(double value, double target, double tolerance,
                const char *expr, const char *file, int line) {
  ++g_checks;
  if (!(std::abs(value - target) <= tolerance)) {
    ++g_failures;
    std::fprintf(stderr, "FAIL %s:%d: %s (%.9g vs %.9g)\n", file, line, expr,
                 value, target);
  }
}
void run(const char *name) { std::fprintf(stderr, "TEST %s\n", name); }
}  // namespace

#define CHECK(expr) ::expect((expr), #expr, __FILE__, __LINE__)
#define CHECK_NEAR(value, target, tolerance) \
  ::expectNear((value), (target), (tolerance), #value " ~= " #target, __FILE__, __LINE__)

// -------------------------------------------------------------------------
// History helpers
// -------------------------------------------------------------------------

namespace {
using atm::NetworkInterfaceInfo;
using atm::NetworkInterfaceSnapshot;
using atm::NetworkInterfaceStats;
using atm::NetworkInterfaceType;
using atm::NetworkTrafficHistory;

NetworkInterfaceInfo makeInterface(const std::string &name, int ifindex,
                                   NetworkInterfaceType type,
                                   std::uint64_t rx_bytes,
                                   std::uint64_t tx_bytes,
                                   std::uint64_t rx_packets = 0,
                                   std::uint64_t tx_packets = 0,
                                   std::uint64_t rx_errors = 0,
                                   std::uint64_t tx_errors = 0,
                                   std::uint64_t rx_dropped = 0,
                                   std::uint64_t tx_dropped = 0) {
  NetworkInterfaceInfo info;
  info.name = name;
  info.type = type;
  info.link.ifindex = ifindex;
  NetworkInterfaceStats traffic;
  traffic.name = name;
  traffic.rx_bytes = rx_bytes;
  traffic.tx_bytes = tx_bytes;
  traffic.rx_packets = rx_packets;
  traffic.tx_packets = tx_packets;
  traffic.rx_errors = rx_errors;
  traffic.tx_errors = tx_errors;
  traffic.rx_dropped = rx_dropped;
  traffic.tx_dropped = tx_dropped;
  info.traffic = traffic;
  info.refreshed_at = std::chrono::system_clock::now();
  return info;
}

NetworkInterfaceSnapshot snapshotWith(
    const std::vector<NetworkInterfaceInfo> &interfaces) {
  NetworkInterfaceSnapshot snapshot;
  snapshot.interfaces = interfaces;
  snapshot.refreshed_at = std::chrono::system_clock::now();
  return snapshot;
}

/// Records after guaranteeing a positive, non-zero elapsed window.
void recordWithGap(NetworkTrafficHistory &monitor,
                   const NetworkInterfaceSnapshot &snapshot) {
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  monitor.record(snapshot);
}

/// Leaves `history` with a last tick whose eth0 RX/TX rate is at least
/// kViolatingMin bytes/s (window is ~2-100 ms, so the delta/elapsed rate is
/// large and positive).
constexpr double kViolatingMin = 1000000.0;  // 1 MB/s guaranteed floor

void primeViolatingTick(NetworkTrafficHistory &history,
                        std::uint64_t delta = 100000) {
  std::vector<NetworkInterfaceInfo> base;
  base.push_back(makeInterface("eth0", 2, NetworkInterfaceType::Ethernet, 0, 0));
  history.record(snapshotWith(base));
  std::vector<NetworkInterfaceInfo> advancing;
  advancing.push_back(makeInterface("eth0", 2, NetworkInterfaceType::Ethernet,
                                    delta, delta));
  recordWithGap(history, snapshotWith(advancing));
}

/// Leaves `history` with a last tick whose eth0 rate is exactly 0 (delta 0).
void primeZeroTick(NetworkTrafficHistory &history) {
  std::vector<NetworkInterfaceInfo> one;
  one.push_back(makeInterface("eth0", 2, NetworkInterfaceType::Ethernet, 50, 50));
  history.record(snapshotWith(one));
  recordWithGap(history, snapshotWith(one));
}

/// A fully configured, valid rule for eth0 (identity "idx:2").
atm::NetworkTrafficAlertRule baseRule() {
  atm::NetworkTrafficAlertRule rule;
  rule.enabled = true;
  rule.target_identity = "idx:2";
  rule.type = atm::NetworkTrafficAlertType::Receive;
  rule.threshold = kViolatingMin;
  rule.unit = atm::NetworkTrafficAlertUnit::BytesPerSecond;
  rule.confirmation = atm::NetworkTrafficAlertConfirmation::Samples;
  rule.confirmation_samples = 3;
  rule.notify_enabled = false;
  rule.cooldown_seconds = 0;
  rule.repeat = false;
  rule.severity = atm::AlertSeverity::Critical;
  return rule;
}

const atm::NetworkTrafficRuleStatus &statusAt(
    const atm::NetworkTrafficAlertMonitor &monitor, std::size_t index) {
  return monitor.statuses()[index];
}

}  // namespace

// -------------------------------------------------------------------------
// Monitor behaviour
// -------------------------------------------------------------------------

int g_sink_calls = 0;
void countingSink(const atm::AlertEvent & /*event*/) { ++g_sink_calls; }

void test_samples_confirmation_and_history() {
  run("samples confirmation fires, records central history, recovers");
  atm::NetworkTrafficHistory history;
  primeViolatingTick(history);
  atm::AlertManager alerts;
  atm::NetworkTrafficAlertMonitor monitor(alerts);

  atm::NetworkTrafficAlertRule rule = baseRule();
  monitor.setRules({rule});
  g_sink_calls = 0;
  monitor.setEventSink(&countingSink);

  monitor.evaluate(history);  // 1st violating tick
  CHECK(statusAt(monitor, 0).status ==
        atm::NetworkTrafficAlertRuleStatus::Pending);
  CHECK(statusAt(monitor, 0).pending_samples == 1);
  CHECK(alerts.history().empty());

  monitor.evaluate(history);  // 2nd
  CHECK(statusAt(monitor, 0).status ==
        atm::NetworkTrafficAlertRuleStatus::Pending);

  monitor.evaluate(history);  // 3rd -> confirmed
  CHECK(statusAt(monitor, 0).status ==
        atm::NetworkTrafficAlertRuleStatus::Exceeded);
  CHECK(statusAt(monitor, 0).available);
  CHECK(statusAt(monitor, 0).value >= kViolatingMin);
  CHECK(g_sink_calls == 0);  // notify_enabled is false

  const auto &events = alerts.history();
  CHECK(events.size() == 1);
  const atm::AlertEvent &ev = events.back();
  CHECK(ev.type == atm::AlertType::NetworkReceive);
  CHECK(ev.source == "eth0");
  CHECK(ev.severity == atm::AlertSeverity::Critical);
  CHECK(ev.is_recovery == false);
  CHECK(ev.value >= kViolatingMin);
  CHECK(ev.threshold == rule.threshold);
  CHECK(ev.message.find("threshold exceeded") != std::string::npos);

  const auto active = alerts.activeAlerts();
  CHECK(active.size() == 1);
  CHECK(active[0].type == atm::AlertType::NetworkReceive);
  CHECK(active[0].source == "eth0");

  // Recovery: a zero-rate tick drops below the hysteresis floor.
  const std::uint64_t at_delta = 100000;
  std::vector<NetworkInterfaceInfo> steady;
  steady.push_back(makeInterface("eth0", 2, NetworkInterfaceType::Ethernet,
                                 at_delta, at_delta));
  recordWithGap(history, snapshotWith(steady));
  monitor.evaluate(history);
  CHECK(statusAt(monitor, 0).status ==
        atm::NetworkTrafficAlertRuleStatus::Recovered);
  CHECK(alerts.history().size() == 2);
  CHECK(alerts.history().back().is_recovery);
  CHECK(alerts.history().back().source == "eth0");
  CHECK(alerts.history().back().message.find("back to normal") !=
        std::string::npos);
  CHECK(alerts.activeAlerts().empty());
}

void test_duration_confirmation_injectable_now() {
  run("duration confirmation with an injected clock");
  atm::NetworkTrafficHistory history;
  primeViolatingTick(history);
  atm::AlertManager alerts;
  atm::NetworkTrafficAlertMonitor monitor(alerts);

  atm::NetworkTrafficAlertRule rule = baseRule();
  rule.confirmation = atm::NetworkTrafficAlertConfirmation::Duration;
  rule.confirmation_duration_seconds = 2.0;
  monitor.setRules({rule});

  const auto t0 = std::chrono::steady_clock::now();
  monitor.evaluate(history, t0);
  CHECK(statusAt(monitor, 0).status ==
        atm::NetworkTrafficAlertRuleStatus::Pending);
  monitor.evaluate(history, t0 + std::chrono::seconds(1));
  CHECK(statusAt(monitor, 0).status ==
        atm::NetworkTrafficAlertRuleStatus::Pending);
  monitor.evaluate(history, t0 + std::chrono::seconds(2));
  CHECK(statusAt(monitor, 0).status ==
        atm::NetworkTrafficAlertRuleStatus::Exceeded);
  CHECK(alerts.history().size() == 1);
}

void test_below_status_and_value() {
  run("a zero-rate tick evaluates as Below with an available value");
  atm::NetworkTrafficHistory history;
  primeZeroTick(history);
  atm::AlertManager alerts;
  atm::NetworkTrafficAlertMonitor monitor(alerts);
  monitor.setRules({baseRule()});
  monitor.evaluate(history);
  CHECK(statusAt(monitor, 0).status == atm::NetworkTrafficAlertRuleStatus::Below);
  CHECK(statusAt(monitor, 0).available);
  CHECK(statusAt(monitor, 0).value == 0.0);
  CHECK(alerts.history().empty());
}

void test_interface_unavailable_resets() {
  run("a vanished interface reports Unavailable and clears active state");
  atm::NetworkTrafficHistory history;
  primeViolatingTick(history);
  atm::AlertManager alerts;
  atm::NetworkTrafficAlertMonitor monitor(alerts);
  atm::NetworkTrafficAlertRule rule = baseRule();
  rule.confirmation_samples = 1;
  monitor.setRules({rule});
  // Single confirm instantly.
  monitor.evaluate(history);
  CHECK(statusAt(monitor, 0).status ==
        atm::NetworkTrafficAlertRuleStatus::Exceeded);
  CHECK(alerts.activeAlerts().size() == 1);

  // A history that has never seen eth0 (record() keeps stale identities in
  // last_tick_, so a genuinely empty source is a fresh history).
  atm::NetworkTrafficHistory other;
  monitor.evaluate(other);
  CHECK(statusAt(monitor, 0).status ==
        atm::NetworkTrafficAlertRuleStatus::InterfaceUnavailable);
  CHECK(alerts.activeAlerts().empty());
}

void test_disabled_and_rule_replacement_clear_subjects() {
  run("setRules() and disabled rules clear stale central subjects");
  atm::NetworkTrafficHistory history;
  primeViolatingTick(history);
  atm::AlertManager alerts;
  atm::NetworkTrafficAlertMonitor monitor(alerts);
  atm::NetworkTrafficAlertRule rule = baseRule();
  rule.confirmation_samples = 1;
  monitor.setRules({rule});
  monitor.evaluate(history);
  CHECK(alerts.activeAlerts().size() == 1);

  // Disabling via setRules clears the central subject and resets the rule to
  // Disabled on the next evaluation.
  atm::NetworkTrafficAlertRule disabled = rule;
  disabled.enabled = false;
  monitor.setRules({disabled});
  CHECK(alerts.activeAlerts().empty());
  monitor.evaluate(history);
  CHECK(statusAt(monitor, 0).status ==
        atm::NetworkTrafficAlertRuleStatus::Disabled);

  // Removing the rule entirely also clears any lingering active subject.
  atm::NetworkTrafficAlertRule rule2 = baseRule();
  rule2.confirmation_samples = 1;
  monitor.setRules({rule2});
  monitor.evaluate(history);
  CHECK(alerts.activeAlerts().size() == 1);
  monitor.setRules({});
  CHECK(alerts.activeAlerts().empty());
}

void test_cooldown_and_repeat() {
  run("repeat respects the rule cooldown with an injected clock");
  atm::NetworkTrafficHistory history;
  primeViolatingTick(history);
  atm::AlertManager alerts;
  atm::NetworkTrafficAlertMonitor monitor(alerts);
  atm::NetworkTrafficAlertRule rule = baseRule();
  rule.confirmation_samples = 1;
  rule.notify_enabled = true;
  rule.repeat = true;
  rule.cooldown_seconds = 5;
  monitor.setRules({rule});
  g_sink_calls = 0;
  monitor.setEventSink(&countingSink);

  const auto t0 = std::chrono::steady_clock::now();
  monitor.evaluate(history, t0);  // confirms, sinks once
  CHECK(g_sink_calls == 1);
  CHECK(statusAt(monitor, 0).status ==
        atm::NetworkTrafficAlertRuleStatus::Exceeded);

  monitor.evaluate(history, t0 + std::chrono::seconds(4));
  CHECK(statusAt(monitor, 0).status ==
        atm::NetworkTrafficAlertRuleStatus::Cooldown);
  CHECK(g_sink_calls == 1);  // throttled

  monitor.evaluate(history, t0 + std::chrono::seconds(5));
  CHECK(statusAt(monitor, 0).status ==
        atm::NetworkTrafficAlertRuleStatus::Exceeded);
  CHECK(g_sink_calls == 2);  // cooldown elapsed -> repeat fires

  monitor.evaluate(history, t0 + std::chrono::seconds(6));
  CHECK(statusAt(monitor, 0).status ==
        atm::NetworkTrafficAlertRuleStatus::Cooldown);
  CHECK(g_sink_calls == 2);

  // No waiting rule: single confirm, no repeats, only one central event.
  atm::NetworkTrafficAlertRule quiet = rule;
  quiet.repeat = false;
  monitor.setRules({quiet});
  const auto t1 = std::chrono::steady_clock::now();
  monitor.evaluate(history, t1);
  CHECK(statusAt(monitor, 0).status ==
        atm::NetworkTrafficAlertRuleStatus::Exceeded);
  monitor.evaluate(history, t1 + std::chrono::seconds(60));
  CHECK(statusAt(monitor, 0).status ==
        atm::NetworkTrafficAlertRuleStatus::Exceeded);
  CHECK(g_sink_calls == 3);  // only the new confirmation
}

void test_combined_aggregate_metric() {
  run("combined aggregate target sums RX and TX");
  atm::NetworkTrafficHistory history;
  primeViolatingTick(history);
  atm::AlertManager alerts;
  atm::NetworkTrafficAlertMonitor monitor(alerts);
  atm::NetworkTrafficAlertRule rule = baseRule();
  rule.target_identity = std::string(atm::kNetworkTrafficAllIdentity);
  rule.type = atm::NetworkTrafficAlertType::Combined;
  rule.threshold = 1500000.0;
  rule.confirmation_samples = 1;
  monitor.setRules({rule});
  monitor.evaluate(history);
  CHECK(statusAt(monitor, 0).status ==
        atm::NetworkTrafficAlertRuleStatus::Exceeded);
  CHECK(statusAt(monitor, 0).value >= 2000000.0);  // rx+tx, each >= 1 MB/s
  CHECK(alerts.history().back().type == atm::AlertType::NetworkCombined);
  CHECK(alerts.history().back().source == "All interfaces");
}

void test_worst_severity() {
  run("worstSeverity reflects confirmed rules and clears on recovery");
  atm::NetworkTrafficHistory history;
  primeViolatingTick(history);
  atm::AlertManager alerts;
  atm::NetworkTrafficAlertMonitor monitor(alerts);
  atm::NetworkTrafficAlertRule critical = baseRule();
  atm::NetworkTrafficAlertRule warning = baseRule();
  warning.type = atm::NetworkTrafficAlertType::Transmit;
  warning.severity = atm::AlertSeverity::Warning;
  critical.confirmation_samples = 1;
  warning.confirmation_samples = 1;
  monitor.setRules({critical, warning});
  monitor.evaluate(history);
  CHECK(monitor.worstSeverity() == atm::AlertSeverity::Critical);

  // Disable the critical rule -> warning remains.
  critical.enabled = false;
  warning.enabled = true;
  monitor.setRules({critical, warning});
  monitor.evaluate(history);
  CHECK(monitor.worstSeverity() == atm::AlertSeverity::Warning);
}

// -------------------------------------------------------------------------
// Unit conversion and formatting
// -------------------------------------------------------------------------

void test_units_and_formatting() {
  run("bit/byte canonical conversions round trip");
  CHECK_NEAR(atm::networkTrafficAlertToCanonical(8.0,
                                                 atm::NetworkTrafficAlertUnit::BitsPerSecond),
             1.0, 1e-9);
  CHECK_NEAR(atm::networkTrafficAlertFromCanonical(
                 1.0, atm::NetworkTrafficAlertUnit::BitsPerSecond),
             8.0, 1e-9);
  CHECK_NEAR(atm::networkTrafficAlertToCanonical(
                 12.5, atm::NetworkTrafficAlertUnit::BytesPerSecond),
             12.5, 1e-9);

  const auto bandwidth_units = {
      atm::NetworkTrafficAlertUnit::BytesPerSecond,
      atm::NetworkTrafficAlertUnit::BitsPerSecond};
  for (auto u : bandwidth_units) {
    CHECK(atm::networkTrafficAlertUnitSupported(
        atm::NetworkTrafficAlertType::Receive, u));
    CHECK(atm::networkTrafficAlertUnitSupported(
        atm::NetworkTrafficAlertType::Transmit, u));
    CHECK(atm::networkTrafficAlertUnitSupported(
        atm::NetworkTrafficAlertType::Combined, u));
    CHECK(!atm::networkTrafficAlertUnitSupported(
        atm::NetworkTrafficAlertType::RxPackets, u));
  }
  CHECK(atm::networkTrafficAlertUnitSupported(
      atm::NetworkTrafficAlertType::RxPackets,
      atm::NetworkTrafficAlertUnit::PacketsPerSecond));
  CHECK(atm::networkTrafficAlertUnitSupported(
      atm::NetworkTrafficAlertType::TxDropped,
      atm::NetworkTrafficAlertUnit::EventsPerSecond));
  CHECK(!atm::networkTrafficAlertUnitSupported(
      atm::NetworkTrafficAlertType::Transmit,
      atm::NetworkTrafficAlertUnit::PacketsPerSecond));

  // Sanity of formatValue: suffix and scaling are present, no crash on 0/N/A.
  CHECK(atm::networkTrafficAlertFormatValue(
            atm::NetworkTrafficAlertType::Receive, 0.0)
            .find("B/s") != std::string::npos);
  CHECK(atm::networkTrafficAlertFormatValue(
            atm::NetworkTrafficAlertType::Combined, 2048.0)
            .find("kB/s") != std::string::npos);
  CHECK(atm::networkTrafficAlertFormatValue(
            atm::NetworkTrafficAlertType::RxPackets, 120000.0)
            .find("kpkt/s") != std::string::npos);
  CHECK(atm::networkTrafficAlertFormatValue(
            atm::NetworkTrafficAlertType::RxErrors, 5.0)
            .find("err/s") != std::string::npos);
  CHECK(atm::networkTrafficAlertFormatValue(
            atm::NetworkTrafficAlertType::TxDropped, 1.0)
            .find("drop/s") != std::string::npos);

  CHECK(std::string(atm::networkTrafficAlertTypeName(
            atm::NetworkTrafficAlertType::TxPackets)) == "tx_packets");
  CHECK(std::string(atm::networkTrafficAlertRuleStatusName(
            atm::NetworkTrafficAlertRuleStatus::Cooldown)) == "Cooldown");
  CHECK(atm::networkTrafficAlertToAlertType(
            atm::NetworkTrafficAlertType::RxErrors) ==
        atm::AlertType::NetworkRxErrors);
}

// -------------------------------------------------------------------------
// Field application / persistence helpers
// -------------------------------------------------------------------------

void test_field_application() {
  run("applyNetworkTrafficAlertRuleField parses and rejects strictly");
  atm::NetworkTrafficAlertRule rule;
  CHECK(atm::applyNetworkTrafficAlertRuleField(
      rule, atm::NetworkTrafficAlertRuleField::Enabled, "false"));
  CHECK(!rule.enabled);
  CHECK(atm::applyNetworkTrafficAlertRuleField(
      rule, atm::NetworkTrafficAlertRuleField::TargetIdentity, "  eth0  "));
  CHECK(rule.target_identity == "eth0");
  CHECK(!atm::applyNetworkTrafficAlertRuleField(
      rule, atm::NetworkTrafficAlertRuleField::TargetIdentity, "a|b"));
  CHECK(rule.target_identity == "eth0");  // unchanged on failure
  CHECK(atm::applyNetworkTrafficAlertRuleField(
      rule, atm::NetworkTrafficAlertRuleField::Threshold, "250.5"));
  CHECK_NEAR(rule.threshold, 250.5, 1e-9);
  CHECK(!atm::applyNetworkTrafficAlertRuleField(
      rule, atm::NetworkTrafficAlertRuleField::Threshold, "abc"));
  CHECK_NEAR(rule.threshold, 250.5, 1e-9);
  CHECK(atm::applyNetworkTrafficAlertRuleField(
      rule, atm::NetworkTrafficAlertRuleField::Type, "tx_packets"));
  CHECK(rule.type == atm::NetworkTrafficAlertType::TxPackets);
  CHECK(!atm::applyNetworkTrafficAlertRuleField(
      rule, atm::NetworkTrafficAlertRuleField::Type, "bogus"));
  CHECK(atm::applyNetworkTrafficAlertRuleField(
      rule, atm::NetworkTrafficAlertRuleField::Unit, "bits_per_second"));
  CHECK(rule.unit == atm::NetworkTrafficAlertUnit::BitsPerSecond);
  CHECK(!atm::applyNetworkTrafficAlertRuleField(
      rule, atm::NetworkTrafficAlertRuleField::Unit, "bytes"));
  CHECK(atm::applyNetworkTrafficAlertRuleField(
      rule, atm::NetworkTrafficAlertRuleField::Confirmation, "duration"));
  CHECK(rule.confirmation == atm::NetworkTrafficAlertConfirmation::Duration);
  CHECK(atm::applyNetworkTrafficAlertRuleField(
      rule, atm::NetworkTrafficAlertRuleField::ConfirmationSamples, "5"));
  CHECK(rule.confirmation_samples == 5);
  CHECK(!atm::applyNetworkTrafficAlertRuleField(
      rule, atm::NetworkTrafficAlertRuleField::ConfirmationSamples, "0"));
  CHECK(atm::applyNetworkTrafficAlertRuleField(
      rule, atm::NetworkTrafficAlertRuleField::ConfirmationDurationSeconds,
      "2.5"));
  CHECK_NEAR(rule.confirmation_duration_seconds, 2.5, 1e-9);
  CHECK(!atm::applyNetworkTrafficAlertRuleField(
      rule, atm::NetworkTrafficAlertRuleField::ConfirmationDurationSeconds,
      "-1"));
  CHECK(atm::applyNetworkTrafficAlertRuleField(
      rule, atm::NetworkTrafficAlertRuleField::NotifyEnabled, "yes"));
  CHECK(rule.notify_enabled);
  CHECK(atm::applyNetworkTrafficAlertRuleField(
      rule, atm::NetworkTrafficAlertRuleField::CooldownSeconds, "30"));
  CHECK(rule.cooldown_seconds == 30);
  CHECK(!atm::applyNetworkTrafficAlertRuleField(
      rule, atm::NetworkTrafficAlertRuleField::CooldownSeconds, "-1"));
  CHECK(atm::applyNetworkTrafficAlertRuleField(
      rule, atm::NetworkTrafficAlertRuleField::Repeat, "1"));
  CHECK(rule.repeat);
  CHECK(atm::applyNetworkTrafficAlertRuleField(
      rule, atm::NetworkTrafficAlertRuleField::Severity, "warning"));
  CHECK(rule.severity == atm::AlertSeverity::Warning);
  CHECK(!atm::applyNetworkTrafficAlertRuleField(
      rule, atm::NetworkTrafficAlertRuleField::Severity, "warn"));
  CHECK(atm::applyNetworkTrafficAlertRuleField(
      rule, atm::NetworkTrafficAlertRuleField::Name, "\"my rule\""));
  CHECK(rule.name == "my rule");
  CHECK(!atm::applyNetworkTrafficAlertRuleField(
      rule, atm::NetworkTrafficAlertRuleField::Name, "a\nb"));
  CHECK(rule.name == "my rule");
}

void test_validation() {
  run("validate rejects invalid rules and accepts good ones");
  atm::NetworkTrafficAlertRule good = baseRule();
  CHECK(atm::validateNetworkTrafficAlertRule(good).valid);

  atm::NetworkTrafficAlertRule bad = good;
  bad.target_identity.clear();
  CHECK(!atm::validateNetworkTrafficAlertRule(bad).valid);
  bad = good;
  bad.target_identity = "idx:2|x";
  CHECK(!atm::validateNetworkTrafficAlertRule(bad).valid);
  bad = good;
  bad.threshold = 0.0;
  CHECK(!atm::validateNetworkTrafficAlertRule(bad).valid);
  bad = good;
  bad.threshold = -1.0;
  CHECK(!atm::validateNetworkTrafficAlertRule(bad).valid);
  bad = good;
  bad.threshold = std::numeric_limits<double>::infinity();
  CHECK(!atm::validateNetworkTrafficAlertRule(bad).valid);
  bad = good;
  bad.unit = atm::NetworkTrafficAlertUnit::PacketsPerSecond;
  CHECK(!atm::validateNetworkTrafficAlertRule(bad).valid);
  bad = good;
  bad.confirmation_samples = 0;
  CHECK(!atm::validateNetworkTrafficAlertRule(bad).valid);
  bad = good;
  bad.confirmation = atm::NetworkTrafficAlertConfirmation::Duration;
  bad.confirmation_duration_seconds = 0.0;
  CHECK(!atm::validateNetworkTrafficAlertRule(bad).valid);
  bad = good;
  bad.cooldown_seconds = atm::kMaxCooldownSeconds + 1;
  CHECK(!atm::validateNetworkTrafficAlertRule(bad).valid);
  bad = good;
  bad.severity = atm::AlertSeverity::Normal;
  CHECK(!atm::validateNetworkTrafficAlertRule(bad).valid);
  bad = good;
  bad.name = "a|b";
  CHECK(!atm::validateNetworkTrafficAlertRule(bad).valid);
}

void test_clamping() {
  run("clamp repairs repairable fields and flags unrepairable rules");
  atm::NetworkTrafficAlertRule rule = baseRule();

  rule.threshold = atm::kMaxNetworkTrafficThreshold * 2.0;
  atm::NetworkTrafficAlertValidation res = atm::clampNetworkTrafficAlertRule(rule);
  CHECK(res.valid);
  CHECK(rule.threshold == atm::kMaxNetworkTrafficThreshold);
  CHECK(!res.problems.empty());

  rule = baseRule();
  rule.threshold = -5.0;
  res = atm::clampNetworkTrafficAlertRule(rule);
  CHECK(!res.valid);  // zero/negative threshold cannot be repaired
  CHECK(rule.threshold == 0.0);

  rule = baseRule();
  rule.threshold = std::numeric_limits<double>::quiet_NaN();
  res = atm::clampNetworkTrafficAlertRule(rule);
  CHECK(!res.valid);

  rule = baseRule();
  rule.unit = atm::NetworkTrafficAlertUnit::PacketsPerSecond;  // invalid combo
  res = atm::clampNetworkTrafficAlertRule(rule);
  CHECK(res.valid);
  CHECK(rule.unit == atm::NetworkTrafficAlertUnit::BytesPerSecond);

  rule = baseRule();
  rule.confirmation_samples = 0;
  rule.severity = atm::AlertSeverity::Normal;
  res = atm::clampNetworkTrafficAlertRule(rule);
  CHECK(res.valid);
  CHECK(rule.confirmation_samples == 1);
  CHECK(rule.severity == atm::AlertSeverity::Critical);

  rule = baseRule();
  rule.target_identity = "  idx:2  ";
  rule.name = "a|b\nc";
  res = atm::clampNetworkTrafficAlertRule(rule);
  CHECK(res.valid);
  CHECK(rule.target_identity == "idx:2");
  CHECK(rule.name == "abc");  // illegal characters dropped

  rule = baseRule();
  rule.target_identity = "   ";
  res = atm::clampNetworkTrafficAlertRule(rule);
  CHECK(!res.valid);
}

void test_persistence_round_trip() {
  run("writeNetworkTrafficAlertRule output reparses to the same rule");
  atm::NetworkTrafficAlertRule rule = baseRule();
  rule.target_identity = "name:enp3s0";
  rule.type = atm::NetworkTrafficAlertType::Combined;
  rule.threshold = 12500000.0;
  rule.unit = atm::NetworkTrafficAlertUnit::BitsPerSecond;
  rule.confirmation = atm::NetworkTrafficAlertConfirmation::Duration;
  rule.confirmation_duration_seconds = 7.5;
  rule.notify_enabled = true;
  rule.cooldown_seconds = 120;
  rule.repeat = true;
  rule.severity = atm::AlertSeverity::Warning;
  rule.name = "wifi backup";

  std::ostringstream out;
  atm::writeNetworkTrafficAlertRule(out, 3, rule);
  const std::string text = out.str();
  CHECK(text.find("[alerts.network_rule.3]") == 0);
  CHECK(text.find("threshold = 1.25e+07") != std::string::npos);

  // Minimal section parser (mirrors parseSettings/applyEntry), mapping the key
  // onto the matching rule field.
  atm::NetworkTrafficAlertRule parsed;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    const std::size_t begin = line.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) continue;
    const std::string trimmed =
        line.substr(begin, line.find_last_not_of(" \t\r\n") - begin + 1);
    if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == '[') continue;
    const std::size_t eq = trimmed.find('=');
    if (eq == std::string::npos) continue;
    std::string key = trimmed.substr(0, eq);
    {
      const auto ktrim = key.find_first_not_of(" \t\r\n");
      key = (ktrim == std::string::npos) ? std::string()
                                         : key.substr(ktrim, key.find_last_not_of(" \t\r\n") - ktrim + 1);
    }
    const std::string value_raw = trimmed.substr(eq + 1);
    const auto vtrim = value_raw.find_first_not_of(" \t\r\n");
    const std::string value = (vtrim == std::string::npos)
                                 ? std::string()
                                 : value_raw.substr(
                                       vtrim,
                                       value_raw.find_last_not_of(" \t\r\n") - vtrim + 1);
    for (int f = 0; f <= static_cast<int>(atm::NetworkTrafficAlertRuleField::Name); ++f) {
      const auto field = static_cast<atm::NetworkTrafficAlertRuleField>(f);
      if (key == atm::networkTrafficAlertRuleFieldName(field)) {
        CHECK(atm::applyNetworkTrafficAlertRuleField(parsed, field, value));
        break;
      }
    }
  }
  CHECK(parsed.enabled == rule.enabled);
  CHECK(parsed.target_identity == rule.target_identity);
  CHECK(parsed.type == rule.type);
  CHECK_NEAR(parsed.threshold, rule.threshold, 1e-6);
  CHECK(parsed.unit == rule.unit);
  CHECK(parsed.confirmation == rule.confirmation);
  CHECK_NEAR(parsed.confirmation_duration_seconds,
             rule.confirmation_duration_seconds, 1e-6);
  CHECK(parsed.notify_enabled == rule.notify_enabled);
  CHECK(parsed.cooldown_seconds == rule.cooldown_seconds);
  CHECK(parsed.repeat == rule.repeat);
  CHECK(parsed.severity == rule.severity);
  CHECK(parsed.name == rule.name);
}

// -------------------------------------------------------------------------

int main() {
  test_samples_confirmation_and_history();
  test_duration_confirmation_injectable_now();
  test_below_status_and_value();
  test_interface_unavailable_resets();
  test_disabled_and_rule_replacement_clear_subjects();
  test_cooldown_and_repeat();
  test_combined_aggregate_metric();
  test_worst_severity();
  test_units_and_formatting();
  test_field_application();
  test_validation();
  test_clamping();
  test_persistence_round_trip();

  std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
  if (g_failures != 0) {
    return EXIT_FAILURE;
  }
  std::fprintf(stderr, "ALL TESTS PASSED\n");
  return EXIT_SUCCESS;
}