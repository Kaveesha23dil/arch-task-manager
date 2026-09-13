#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "alert_manager.hpp"
#include "network_traffic_history.hpp"

namespace atm {

// --- Rule model ----------------------------------------------------------

/// The metric a network-traffic rule monitors. Traffic rules track transfer
/// throughput; the others track packet/error/drop counts. Every metric is
/// evaluated as a *rate* (per-second count) derived by NetworkTrafficHistory
/// from the same validated counters the graphs and export use — never from a
/// second poll of /proc/net/dev.
enum class NetworkTrafficAlertType {
  Receive,   // RX throughput (bytes/s)
  Transmit,  // TX throughput (bytes/s)
  Combined,  // RX + TX throughput (bytes/s)
  RxPackets,
  TxPackets,
  RxErrors,
  TxErrors,
  RxDropped,
  TxDropped,
};

[[nodiscard]] const char *networkTrafficAlertTypeName(
    NetworkTrafficAlertType type);

/// The per-second unit a rule's threshold is expressed/displayed in. The rule
/// always stores the canonical value (bytes/s for traffic, count/s for the
/// packet/error/drop metrics); bits/s is converted to bytes/s at the boundary
/// and never stored.
enum class NetworkTrafficAlertUnit {
  BytesPerSecond,
  BitsPerSecond,
  PacketsPerSecond,
  EventsPerSecond,
};

[[nodiscard]] const char *networkTrafficAlertUnitName(
    NetworkTrafficAlertUnit unit);

/// Whether a unit is valid for an alert type (traffic types accept bytes or
/// bits per second; packet metrics use packets/s and error/drop metrics use
/// events/s).
[[nodiscard]] bool networkTrafficAlertUnitSupported(
    NetworkTrafficAlertType type, NetworkTrafficAlertUnit unit);

/// Converts a user-facing threshold expressed in `unit` to the canonical
/// per-second value stored in structs / persisted in the configuration.
[[nodiscard]] double networkTrafficAlertToCanonical(
    double value, NetworkTrafficAlertUnit unit);

/// Inverse of ToCanonical: a canonical per-second value back into `unit`.
[[nodiscard]] double networkTrafficAlertFromCanonical(
    double value, NetworkTrafficAlertUnit unit);

/// How a rule confirms that a threshold breach is real before firing.
enum class NetworkTrafficAlertConfirmation {
  Samples,   // N consecutive violating refreshes
  Duration,  // continuously violating for a span of wall time
};

enum class NetworkTrafficAlertRuleStatus {
  Disabled,             // rule.enabled == false
  WaitingForBaseline,   // metric not yet measurable (no safe rate window)
  Below,                // metric available and under the threshold
  Pending,              // violating but not yet confirmed
  Exceeded,             // confirmed breach, actively in the alert episode
  Cooldown,             // confirmed breach, repeat throttled by rule cooldown
  InterfaceUnavailable, // target identity not present in the last tick
  Recovered,            // recovered this tick (below the recovery floor)
};

[[nodiscard]] const char *networkTrafficAlertRuleStatusName(
    NetworkTrafficAlertRuleStatus status);

// --- Limits --------------------------------------------------------------

inline constexpr std::size_t kMaxNetworkTrafficAlertRules = 32;
inline constexpr double kMaxNetworkTrafficThreshold = 1e15;  // canonical /s
inline constexpr std::size_t kMaxConfirmationSamples = 1000;
inline constexpr double kMaxConfirmationDurationSeconds = 3600.0;
inline constexpr std::size_t kMaxCooldownSeconds = 3600;
inline constexpr std::size_t kMaxRuleNameLength = 128;
inline constexpr std::size_t kMaxTargetIdentityLength = 128;
/// Hysteresis floor: after a confirmed breach the rule recovers only when the
/// metric drops below threshold * this fraction, so values hovering near the
/// threshold do not oscillate.
inline constexpr double kNetworkTrafficRecoveryFloorFraction = 0.9;
/// Display name of the "all interfaces" aggregate (mirrors the history
/// component's label so the UI and notifications stay consistent).
inline constexpr std::string_view kNetworkTrafficAllDisplayName =
    "All interfaces";

/// One configurable network-traffic alert rule.
struct NetworkTrafficAlertRule {
  bool enabled = true;
  /// Stable target identity: kNetworkTrafficAllIdentity for the aggregate, or
  /// the "idx:<ifindex>" / "name:<name>" identity of a specific interface.
  std::string target_identity;
  NetworkTrafficAlertType type = NetworkTrafficAlertType::Receive;
  /// Threshold in canonical per-second units (> 0).
  double threshold = 0.0;
  NetworkTrafficAlertUnit unit = NetworkTrafficAlertUnit::BytesPerSecond;
  NetworkTrafficAlertConfirmation confirmation =
      NetworkTrafficAlertConfirmation::Samples;
  std::size_t confirmation_samples = 3;    // used in Samples mode
  double confirmation_duration_seconds = 5.0;  // used in Duration mode
  bool notify_enabled = true;
  std::size_t cooldown_seconds = 0;  // min gap between repeated notifications
  bool repeat = false;               // re-notify after cooldown while exceeding
  AlertSeverity severity = AlertSeverity::Critical;  // Warning or Critical
  std::string name;  // optional human label, must not contain '|' or '\n'
};

/// Single flat field of NetworkTrafficAlertRule that indexing/persisting
/// helpers map a configuration key onto.
enum class NetworkTrafficAlertRuleField {
  Enabled,
  TargetIdentity,
  Type,
  Threshold,
  Unit,
  Confirmation,
  ConfirmationSamples,
  ConfirmationDurationSeconds,
  NotifyEnabled,
  CooldownSeconds,
  Repeat,
  Severity,
  Name,
};

/// Parses one `[alerts.network_rule.<i>]` key/value into the field of `rule`.
/// Returns false when the value cannot be trusted (the caller records a
/// problem and keeps the previous value). Never throws.
[[nodiscard]] bool applyNetworkTrafficAlertRuleField(
    NetworkTrafficAlertRule &rule, NetworkTrafficAlertRuleField field,
    std::string_view raw);

[[nodiscard]] const char *networkTrafficAlertRuleFieldName(
    NetworkTrafficAlertRuleField field);

/// Serializes one rule into its `[alerts.network_rule.<i>]` section text
/// (multiline; appended to `out`). Deterministic, matches parseSettings().
void writeNetworkTrafficAlertRule(std::ostringstream &out,
                                  std::size_t index,
                                  const NetworkTrafficAlertRule &rule);

// --- Validation ----------------------------------------------------------

struct NetworkTrafficAlertValidation {
  bool valid = true;
  std::vector<std::string> problems;
};

/// Cross-field validation: identity present/well-formed, threshold strictly
/// positive and finite, unit supported for the type, confirmation counts and
/// durations in range, cooldown bounded, severity Warning/Critical, name free
/// of '|' and newlines. A rule with an empty identity or a non-positive
/// threshold cannot be repaired and is reported invalid.
[[nodiscard]] NetworkTrafficAlertValidation validateNetworkTrafficAlertRule(
    const NetworkTrafficAlertRule &rule);

/// Clamps every repairable field into range (threshold, confirmation counts,
/// durations, cooldown, name length, unit selection, severity) and returns any
/// problems; `valid` is false only when the rule must be dropped (empty
/// identity or non-positive threshold after clamping).
[[nodiscard]] NetworkTrafficAlertValidation clampNetworkTrafficAlertRule(
    NetworkTrafficAlertRule &rule);

/// Formats a canonical metric value for the given alert type ("12.5 MB/s",
/// "3.2 kpkt/s", "128 evt/s", ...). Used for rule status lines, central alert
/// descriptions and notification messages.
[[nodiscard]] std::string networkTrafficAlertFormatValue(
    NetworkTrafficAlertType type, double value);

/// Maps a rule metric type to the central AlertType used for subjects/history.
[[nodiscard]] AlertType networkTrafficAlertToAlertType(
    NetworkTrafficAlertType type);

// --- Monitor -------------------------------------------------------------

/// One rule's live status, produced fresh every evaluate().
struct NetworkTrafficRuleStatus {
  std::size_t index = 0;
  NetworkTrafficAlertRule rule;  // copy of the rule as evaluated
  NetworkTrafficAlertRuleStatus status = NetworkTrafficAlertRuleStatus::Below;
  double value = 0.0;       // canonical per-second value when available
  bool available = false;   // whether a value was measurable this tick
  std::size_t pending_samples = 0;  // Samples-mode confirmation progress
  std::string detail;       // transient human detail line
};

/// Evaluates configured network-traffic alert rules against the exact derived
/// rates NetworkTrafficHistory::record() produces — the same validated values
/// the graphs and export use, so alerts can never disagree with the UI. No
/// second polling loop and no /proc parse happen here.
///
/// Each rule is an independent state machine: unconfirmed violations accumulate
/// (consecutive samples or wall-clock duration) until confirmed; the resulting
/// severity transition is recorded into the central AlertManager (history +
/// active state) without firing its notification sink; desktop notifications
/// are delivered through the optional event sink under the rule's own
/// notify_enabled/cooldown/repeat policy (NotificationManager still applies
/// the global settings and its own cooldown). Recovery uses a hysteresis floor
/// (threshold * kNetworkTrafficRecoveryFloorFraction). A missing interface or
/// an unmeasurable metric never trips an alert: metrics freeze, interfaces
/// reset the rule to InterfaceUnavailable and clear any stale active state.
class NetworkTrafficAlertMonitor {
 public:
  explicit NetworkTrafficAlertMonitor(AlertManager &alerts);

  NetworkTrafficAlertMonitor(const NetworkTrafficAlertMonitor &) = delete;
  NetworkTrafficAlertMonitor &operator=(const NetworkTrafficAlertMonitor &) =
      delete;

  using EventSink = void (*)(const AlertEvent &);

  /// Replaces the configured rules. Stale alert subjects whose rules were
  /// removed/disabled are silently cleared and all runtime state is reset.
  void setRules(const std::vector<NetworkTrafficAlertRule> &rules);

  [[nodiscard]] const std::vector<NetworkTrafficAlertRule> &rules() const {
    return rules_;
  }

  /// Evaluates every rule against the latest history tick. `now` is injectable
  /// for deterministic tests; production callers omit it (steady clock).
  void evaluate(const NetworkTrafficHistory &history,
                std::optional<std::chrono::steady_clock::time_point> now =
                    std::nullopt);

  /// The most recent per-rule statuses (one per configured rule, in rules
  /// order). Read-only; regenerated on every evaluate().
  [[nodiscard]] const std::vector<NetworkTrafficRuleStatus> &statuses() const {
    return statuses_;
  }

  /// Worst severity across the currently-confirmed rules (Normal when none).
  [[nodiscard]] AlertSeverity worstSeverity() const;

  /// Registers the desktop-notification delivery callback; the monitor invokes
  /// it for new violations/recoveries/repeats when the rule enables them. May
  /// be set to nullptr to disable.
  void setEventSink(EventSink sink);

 private:
  struct RuleRuntime {
    bool confirmed = false;  // confirmed alert episode in progress
    std::size_t consecutive_violations = 0;
    std::optional<std::chrono::steady_clock::time_point> streak_start;
    std::optional<std::chrono::steady_clock::time_point> last_emission;
    std::string last_source;  // display name used as the central subject
  };

  AlertManager &alerts_;
  std::vector<NetworkTrafficAlertRule> rules_;
  std::unordered_map<std::string, RuleRuntime> states_;
  std::vector<NetworkTrafficRuleStatus> statuses_;
  EventSink event_sink_ = nullptr;

  static std::string stateKey(const NetworkTrafficAlertRule &rule);

  void fireSink(const AlertEvent &event);
  [[nodiscard]] double recoveryFloor(double threshold) const;
};

}  // namespace atm