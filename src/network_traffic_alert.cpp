#include "network_traffic_alert.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>

namespace atm {

// ---------------------------------------------------------------------------
// Name maps
// ---------------------------------------------------------------------------

const char *networkTrafficAlertTypeName(NetworkTrafficAlertType type) {
  switch (type) {
    case NetworkTrafficAlertType::Receive:   return "receive";
    case NetworkTrafficAlertType::Transmit:  return "transmit";
    case NetworkTrafficAlertType::Combined:  return "combined";
    case NetworkTrafficAlertType::RxPackets:  return "rx_packets";
    case NetworkTrafficAlertType::TxPackets:  return "tx_packets";
    case NetworkTrafficAlertType::RxErrors:   return "rx_errors";
    case NetworkTrafficAlertType::TxErrors:   return "tx_errors";
    case NetworkTrafficAlertType::RxDropped:  return "rx_dropped";
    case NetworkTrafficAlertType::TxDropped:  return "tx_dropped";
  }
  return "unknown";
}

const char *networkTrafficAlertUnitName(NetworkTrafficAlertUnit unit) {
  switch (unit) {
    case NetworkTrafficAlertUnit::BytesPerSecond:    return "bytes_per_second";
    case NetworkTrafficAlertUnit::BitsPerSecond:     return "bits_per_second";
    case NetworkTrafficAlertUnit::PacketsPerSecond:  return "packets_per_second";
    case NetworkTrafficAlertUnit::EventsPerSecond:   return "events_per_second";
  }
  return "unknown";
}

const char *networkTrafficAlertRuleStatusName(
    NetworkTrafficAlertRuleStatus status) {
  switch (status) {
    case NetworkTrafficAlertRuleStatus::Disabled:            return "Disabled";
    case NetworkTrafficAlertRuleStatus::WaitingForBaseline:  return "Waiting";
    case NetworkTrafficAlertRuleStatus::Below:               return "Below";
    case NetworkTrafficAlertRuleStatus::Pending:             return "Pending";
    case NetworkTrafficAlertRuleStatus::Exceeded:            return "Exceeded";
    case NetworkTrafficAlertRuleStatus::Cooldown:            return "Cooldown";
    case NetworkTrafficAlertRuleStatus::InterfaceUnavailable:return "Unavailable";
    case NetworkTrafficAlertRuleStatus::Recovered:           return "Recovered";
  }
  return "Unknown";
}

const char *networkTrafficAlertRuleFieldName(
    NetworkTrafficAlertRuleField field) {
  switch (field) {
    case NetworkTrafficAlertRuleField::Enabled:                  return "enabled";
    case NetworkTrafficAlertRuleField::TargetIdentity:           return "target_identity";
    case NetworkTrafficAlertRuleField::Type:                     return "type";
    case NetworkTrafficAlertRuleField::Threshold:                return "threshold";
    case NetworkTrafficAlertRuleField::Unit:                     return "unit";
    case NetworkTrafficAlertRuleField::Confirmation:             return "confirmation";
    case NetworkTrafficAlertRuleField::ConfirmationSamples:      return "confirmation_samples";
    case NetworkTrafficAlertRuleField::ConfirmationDurationSeconds: return "confirmation_duration_seconds";
    case NetworkTrafficAlertRuleField::NotifyEnabled:            return "notify_enabled";
    case NetworkTrafficAlertRuleField::CooldownSeconds:          return "cooldown_seconds";
    case NetworkTrafficAlertRuleField::Repeat:                   return "repeat";
    case NetworkTrafficAlertRuleField::Severity:                 return "severity";
    case NetworkTrafficAlertRuleField::Name:                     return "name";
  }
  return "unknown";
}

// ---------------------------------------------------------------------------
// Unit helpers
// ---------------------------------------------------------------------------

bool networkTrafficAlertUnitSupported(NetworkTrafficAlertType type,
                                      NetworkTrafficAlertUnit unit) {
  switch (type) {
    case NetworkTrafficAlertType::Receive:
    case NetworkTrafficAlertType::Transmit:
    case NetworkTrafficAlertType::Combined:
      return unit == NetworkTrafficAlertUnit::BytesPerSecond ||
             unit == NetworkTrafficAlertUnit::BitsPerSecond;
    case NetworkTrafficAlertType::RxPackets:
    case NetworkTrafficAlertType::TxPackets:
      return unit == NetworkTrafficAlertUnit::PacketsPerSecond;
    case NetworkTrafficAlertType::RxErrors:
    case NetworkTrafficAlertType::TxErrors:
    case NetworkTrafficAlertType::RxDropped:
    case NetworkTrafficAlertType::TxDropped:
      return unit == NetworkTrafficAlertUnit::EventsPerSecond;
  }
  return false;
}

double networkTrafficAlertToCanonical(double value,
                                      NetworkTrafficAlertUnit unit) {
  if (unit == NetworkTrafficAlertUnit::BitsPerSecond) {
    return value / 8.0;
  }
  return value;
}

double networkTrafficAlertFromCanonical(double value,
                                        NetworkTrafficAlertUnit unit) {
  if (unit == NetworkTrafficAlertUnit::BitsPerSecond) {
    return value * 8.0;
  }
  return value;
}

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------

namespace {

std::string formatBytesPerSecond(double value) {
  static constexpr std::array<const char *, 5> kSuffixes = {"B/s", "kB/s",
                                                            "MB/s", "GB/s",
                                                            "TB/s"};
  const double v = std::abs(value);
  std::size_t idx = 0;
  double scaled = v;
  while (scaled >= 1024.0 && idx + 1 < kSuffixes.size()) {
    scaled /= 1024.0;
    ++idx;
  }
  std::ostringstream out;
  out << std::fixed << std::setprecision(scaled >= 100.0 ? 0 : 1) << scaled
      << ' ' << kSuffixes[idx];
  return out.str();
}

std::string formatCountPerSecond(double value, const char *unit) {
  static constexpr std::array<const char *, 5> kPrefix = {"", "k", "M", "G",
                                                          "T"};
  const double v = std::abs(value);
  std::size_t idx = 0;
  double scaled = v;
  while (scaled >= 1000.0 && idx + 1 < kPrefix.size()) {
    scaled /= 1000.0;
    ++idx;
  }
  std::ostringstream out;
  out << std::fixed << std::setprecision(scaled >= 100.0 ? 0 : 1) << scaled
      << ' ' << kPrefix[idx] << unit;
  return out.str();
}

}  // namespace

std::string networkTrafficAlertFormatValue(NetworkTrafficAlertType type,
                                           double value) {
  switch (type) {
    case NetworkTrafficAlertType::Receive:
    case NetworkTrafficAlertType::Transmit:
    case NetworkTrafficAlertType::Combined:
      return formatBytesPerSecond(value);
    case NetworkTrafficAlertType::RxPackets:
    case NetworkTrafficAlertType::TxPackets:
      return formatCountPerSecond(value, "pkt/s");
    case NetworkTrafficAlertType::RxErrors:
    case NetworkTrafficAlertType::TxErrors:
      return formatCountPerSecond(value, "err/s");
    case NetworkTrafficAlertType::RxDropped:
    case NetworkTrafficAlertType::TxDropped:
      return formatCountPerSecond(value, "drop/s");
  }
  return std::to_string(value) + "/s";
}

AlertType networkTrafficAlertToAlertType(NetworkTrafficAlertType type) {
  switch (type) {
    case NetworkTrafficAlertType::Receive:   return AlertType::NetworkReceive;
    case NetworkTrafficAlertType::Transmit:  return AlertType::NetworkTransmit;
    case NetworkTrafficAlertType::Combined:  return AlertType::NetworkCombined;
    case NetworkTrafficAlertType::RxPackets:  return AlertType::NetworkRxPackets;
    case NetworkTrafficAlertType::TxPackets:  return AlertType::NetworkTxPackets;
    case NetworkTrafficAlertType::RxErrors:   return AlertType::NetworkRxErrors;
    case NetworkTrafficAlertType::TxErrors:   return AlertType::NetworkTxErrors;
    case NetworkTrafficAlertType::RxDropped:  return AlertType::NetworkRxDropped;
    case NetworkTrafficAlertType::TxDropped:  return AlertType::NetworkTxDropped;
  }
  return AlertType::NetworkCombined;
}

// ---------------------------------------------------------------------------
// Field parsing and writing
// ---------------------------------------------------------------------------

namespace {

bool parseBoolField(bool &out, std::string_view raw) {
  // Match settings.cpp true/1/yes/false/0/no acceptance.
  const auto trim = [](std::string_view sv) {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' ||
                           sv.front() == '\r' || sv.front() == '\n'))
      sv.remove_prefix(1);
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' ||
                           sv.back() == '\r' || sv.back() == '\n'))
      sv.remove_suffix(1);
    return sv;
  };
  const std::string_view v = trim(raw);
  if (v == "true" || v == "1" || v == "yes") { out = true; return true; }
  if (v == "false" || v == "0" || v == "no") { out = false; return true; }
  return false;
}

bool parseTypeField(NetworkTrafficAlertType &out, std::string_view raw) {
  const auto trim = [](std::string_view sv) {
    while (!sv.empty() && (sv.front() == '"' || sv.front() == '\''))
      sv.remove_prefix(1);
    while (!sv.empty() && (sv.back() == '"' || sv.back() == '\''))
      sv.remove_suffix(1);
    return sv;
  };
  const std::string_view v = trim(raw);
  // Exact-match against type names (lowercase).
  if (v == "receive")  { out = NetworkTrafficAlertType::Receive;  return true; }
  if (v == "transmit") { out = NetworkTrafficAlertType::Transmit; return true; }
  if (v == "combined") { out = NetworkTrafficAlertType::Combined; return true; }
  if (v == "rx_packets") { out = NetworkTrafficAlertType::RxPackets; return true; }
  if (v == "tx_packets") { out = NetworkTrafficAlertType::TxPackets; return true; }
  if (v == "rx_errors")  { out = NetworkTrafficAlertType::RxErrors;  return true; }
  if (v == "tx_errors")  { out = NetworkTrafficAlertType::TxErrors;  return true; }
  if (v == "rx_dropped") { out = NetworkTrafficAlertType::RxDropped; return true; }
  if (v == "tx_dropped") { out = NetworkTrafficAlertType::TxDropped; return true; }
  return false;
}

bool parseUnitField(NetworkTrafficAlertUnit &out, std::string_view raw) {
  const auto trim = [](std::string_view sv) {
    while (!sv.empty() && (sv.front() == '"' || sv.front() == '\''))
      sv.remove_prefix(1);
    while (!sv.empty() && (sv.back() == '"' || sv.back() == '\''))
      sv.remove_suffix(1);
    return sv;
  };
  const std::string_view v = trim(raw);
  if (v == "bytes_per_second") { out = NetworkTrafficAlertUnit::BytesPerSecond;    return true; }
  if (v == "bits_per_second")  { out = NetworkTrafficAlertUnit::BitsPerSecond;     return true; }
  if (v == "packets_per_second") { out = NetworkTrafficAlertUnit::PacketsPerSecond; return true; }
  if (v == "events_per_second") { out = NetworkTrafficAlertUnit::EventsPerSecond;   return true; }
  return false;
}

bool parseConfirmationField(NetworkTrafficAlertConfirmation &out,
                            std::string_view raw) {
  const auto trim = [](std::string_view sv) {
    while (!sv.empty() && (sv.front() == '"' || sv.front() == '\''))
      sv.remove_prefix(1);
    while (!sv.empty() && (sv.back() == '"' || sv.back() == '\''))
      sv.remove_suffix(1);
    return sv;
  };
  const std::string_view v = trim(raw);
  if (v == "samples")  { out = NetworkTrafficAlertConfirmation::Samples;  return true; }
  if (v == "duration") { out = NetworkTrafficAlertConfirmation::Duration; return true; }
  return false;
}

bool parseSeverityField(AlertSeverity &out, std::string_view raw) {
  const auto trim = [](std::string_view sv) {
    while (!sv.empty() && (sv.front() == '"' || sv.front() == '\''))
      sv.remove_prefix(1);
    while (!sv.empty() && (sv.back() == '"' || sv.back() == '\''))
      sv.remove_suffix(1);
    return sv;
  };
  const std::string_view v = trim(raw);
  if (v == "warning")  { out = AlertSeverity::Warning;  return true; }
  if (v == "critical") { out = AlertSeverity::Critical; return true; }
  return false;
}

double strtodStrict(std::string_view raw) {
  std::string s(raw);
  char *end = nullptr;
  errno = 0;
  const double v = std::strtod(s.c_str(), &end);
  if (errno != 0 || end == nullptr || *end != '\0') {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return v;
}

long strtolStrict(std::string_view raw) {
  std::string s(raw);
  char *end = nullptr;
  errno = 0;
  const long v = std::strtol(s.c_str(), &end, 10);
  if (errno != 0 || end == nullptr || *end != '\0') {
    return 0;
  }
  return v;
}

std::string trimAndUnquote(std::string_view raw) {
  std::string s(raw);
  // Trim surrounding whitespace
  const auto first = s.find_first_not_of(" \t\r\n");
  const auto last  = s.find_last_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }
  s = s.substr(first, last - first + 1);
  // Strip double quotes
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
    s = s.substr(1, s.size() - 2);
  }
  return s;
}

bool hasBadChar(const std::string &s, bool reject_pipe, bool reject_newline) {
  for (char ch : s) {
    if (reject_pipe && ch == '|') return true;
    if (reject_newline && (ch == '\n' || ch == '\r')) return true;
  }
  return false;
}

}  // namespace

bool applyNetworkTrafficAlertRuleField(NetworkTrafficAlertRule &rule,
                                       NetworkTrafficAlertRuleField field,
                                       std::string_view raw) {
  switch (field) {
    case NetworkTrafficAlertRuleField::Enabled: {
      bool v = rule.enabled;
      return parseBoolField(v, raw) ? (rule.enabled = v, true) : false;
    }
    case NetworkTrafficAlertRuleField::TargetIdentity: {
      const std::string v = trimAndUnquote(raw);
      if (v.empty() || hasBadChar(v, true, true) ||
          v.size() > kMaxTargetIdentityLength) {
        return false;
      }
      rule.target_identity = v;
      return true;
    }
    case NetworkTrafficAlertRuleField::Type: {
      return parseTypeField(rule.type, raw);
    }
    case NetworkTrafficAlertRuleField::Threshold: {
      const double v = strtodStrict(raw);
      if (std::isfinite(v)) {
        rule.threshold = v;
        return true;
      }
      return false;
    }
    case NetworkTrafficAlertRuleField::Unit: {
      return parseUnitField(rule.unit, raw);
    }
    case NetworkTrafficAlertRuleField::Confirmation: {
      return parseConfirmationField(rule.confirmation, raw);
    }
    case NetworkTrafficAlertRuleField::ConfirmationSamples: {
      const long v = strtolStrict(raw);
      if (v > 0) {
        rule.confirmation_samples = static_cast<std::size_t>(v);
        return true;
      }
      return false;
    }
    case NetworkTrafficAlertRuleField::ConfirmationDurationSeconds: {
      const double v = strtodStrict(raw);
      if (std::isfinite(v) && v > 0.0) {
        rule.confirmation_duration_seconds = v;
        return true;
      }
      return false;
    }
    case NetworkTrafficAlertRuleField::NotifyEnabled: {
      bool v = rule.notify_enabled;
      return parseBoolField(v, raw) ? (rule.notify_enabled = v, true) : false;
    }
    case NetworkTrafficAlertRuleField::CooldownSeconds: {
      const long v = strtolStrict(raw);
      if (v >= 0) {
        rule.cooldown_seconds = static_cast<std::size_t>(v);
        return true;
      }
      return false;
    }
    case NetworkTrafficAlertRuleField::Repeat: {
      bool v = rule.repeat;
      return parseBoolField(v, raw) ? (rule.repeat = v, true) : false;
    }
    case NetworkTrafficAlertRuleField::Severity: {
      return parseSeverityField(rule.severity, raw);
    }
    case NetworkTrafficAlertRuleField::Name: {
      const std::string v = trimAndUnquote(raw);
      if (hasBadChar(v, true, true) || v.size() > kMaxRuleNameLength) {
        return false;
      }
      rule.name = v;
      return true;
    }
  }
  return false;
}

void writeNetworkTrafficAlertRule(std::ostringstream &out, std::size_t index,
                                  const NetworkTrafficAlertRule &rule) {
  out << "[alerts.network_rule." << index << "]\n"
      << "enabled = " << (rule.enabled ? "true" : "false") << '\n'
      << "target_identity = \"" << rule.target_identity << "\"\n"
      << "type = \"" << networkTrafficAlertTypeName(rule.type) << "\"\n"
      << "threshold = " << rule.threshold << '\n'
      << "unit = \"" << networkTrafficAlertUnitName(rule.unit) << "\"\n"
      << "confirmation = \""
      << (rule.confirmation == NetworkTrafficAlertConfirmation::Samples
              ? "samples"
              : "duration")
      << "\"\n"
      << "confirmation_samples = " << rule.confirmation_samples << '\n'
      << "confirmation_duration_seconds = "
      << rule.confirmation_duration_seconds << '\n'
      << "notify_enabled = " << (rule.notify_enabled ? "true" : "false") << '\n'
      << "cooldown_seconds = " << rule.cooldown_seconds << '\n'
      << "repeat = " << (rule.repeat ? "true" : "false") << '\n'
      << "severity = \""
      << (rule.severity == AlertSeverity::Warning ? "warning" : "critical")
      << "\"\n"
      << "name = \"" << rule.name << "\"\n";
}

// ---------------------------------------------------------------------------
// Validation / clamping
// ---------------------------------------------------------------------------

NetworkTrafficAlertValidation validateNetworkTrafficAlertRule(
    const NetworkTrafficAlertRule &rule) {
  NetworkTrafficAlertValidation result;
  auto fail = [&](const std::string &problem) {
    result.valid = false;
    result.problems.push_back(problem);
  };

  if (rule.target_identity.empty()) {
    fail("network rule: target_identity is empty");
  }
  if (rule.target_identity.size() > kMaxTargetIdentityLength) {
    fail("network rule: target_identity too long");
  }
  if (hasBadChar(rule.target_identity, true, true)) {
    fail("network rule: target_identity contains illegal characters");
  }
  if (!std::isfinite(rule.threshold) || rule.threshold <= 0.0) {
    fail("network rule: threshold must be a positive number");
  }
  if (rule.threshold > kMaxNetworkTrafficThreshold) {
    fail("network rule: threshold exceeds maximum");
  }
  if (!networkTrafficAlertUnitSupported(rule.type, rule.unit)) {
    fail("network rule: unit not supported for this alert type");
  }
  if (rule.confirmation == NetworkTrafficAlertConfirmation::Samples) {
    if (rule.confirmation_samples < 1 ||
        rule.confirmation_samples > kMaxConfirmationSamples) {
      fail("network rule: confirmation_samples out of range");
    }
  } else if (rule.confirmation == NetworkTrafficAlertConfirmation::Duration) {
    if (!std::isfinite(rule.confirmation_duration_seconds) ||
        rule.confirmation_duration_seconds <= 0.0 ||
        rule.confirmation_duration_seconds > kMaxConfirmationDurationSeconds) {
      fail("network rule: confirmation_duration_seconds out of range");
    }
  } else {
    fail("network rule: unknown confirmation mode");
  }
  if (rule.cooldown_seconds > kMaxCooldownSeconds) {
    fail("network rule: cooldown_seconds exceeds maximum");
  }
  if (rule.severity != AlertSeverity::Warning &&
      rule.severity != AlertSeverity::Critical) {
    fail("network rule: severity must be 'warning' or 'critical'");
  }
  if (rule.name.size() > kMaxRuleNameLength) {
    fail("network rule: name too long");
  }
  if (hasBadChar(rule.name, true, true)) {
    fail("network rule: name contains illegal characters");
  }
  return result;
}

NetworkTrafficAlertValidation clampNetworkTrafficAlertRule(
    NetworkTrafficAlertRule &rule) {
  NetworkTrafficAlertValidation result;
  auto note = [&](const std::string &message) {
    result.problems.push_back(message);
  };

  // Threshold: keep finite, clamp positive range.  A zero/negative threshold
  // after clamping means the rule is irreparably invalid and must be dropped.
  if (!std::isfinite(rule.threshold)) {
    rule.threshold = 0.0;
  }
  if (rule.threshold < 0.0) {
    rule.threshold = 0.0;
    note("adjusted out-of-range network rule threshold to zero");
  }
  if (rule.threshold > kMaxNetworkTrafficThreshold) {
    rule.threshold = kMaxNetworkTrafficThreshold;
    note("clamped network rule threshold to maximum");
  }
  if (rule.threshold <= 0.0) {
    result.valid = false;
    result.problems.push_back(
        "network rule threshold must be greater than zero");
  }

  // Confirmation counts/durations.
  if (rule.confirmation_samples < 1) {
    rule.confirmation_samples = 1;
    note("adjusted network rule confirmation_samples to minimum");
  }
  if (rule.confirmation_samples > kMaxConfirmationSamples) {
    rule.confirmation_samples = kMaxConfirmationSamples;
    note("clamped network rule confirmation_samples to maximum");
  }
  if (!std::isfinite(rule.confirmation_duration_seconds) ||
      rule.confirmation_duration_seconds <= 0.0) {
    rule.confirmation_duration_seconds = 1.0;
    note("adjusted network rule confirmation_duration_seconds to 1 s");
  }
  if (rule.confirmation_duration_seconds > kMaxConfirmationDurationSeconds) {
    rule.confirmation_duration_seconds = kMaxConfirmationDurationSeconds;
    note("clamped network rule confirmation_duration_seconds to maximum");
  }

  // Cooldown.
  if (rule.cooldown_seconds > kMaxCooldownSeconds) {
    rule.cooldown_seconds = kMaxCooldownSeconds;
    note("clamped network rule cooldown_seconds to maximum");
  }

  // Unit — fix unsupported combos (traffic→B/s, packet→pkt/s, err/drop→evt/s).
  if (!networkTrafficAlertUnitSupported(rule.type, rule.unit)) {
    rule.unit = (rule.type == NetworkTrafficAlertType::Receive ||
                 rule.type == NetworkTrafficAlertType::Transmit ||
                 rule.type == NetworkTrafficAlertType::Combined)
                    ? NetworkTrafficAlertUnit::BytesPerSecond
                    : (rule.type == NetworkTrafficAlertType::RxPackets ||
                       rule.type == NetworkTrafficAlertType::TxPackets)
                          ? NetworkTrafficAlertUnit::PacketsPerSecond
                          : NetworkTrafficAlertUnit::EventsPerSecond;
    note("repaired unsupported unit for network rule alert type");
  }

  // Severity.
  if (rule.severity != AlertSeverity::Warning &&
      rule.severity != AlertSeverity::Critical) {
    rule.severity = AlertSeverity::Critical;
    note("repaired unsupported severity to Critical");
  }

  // Target identity — trim whitespace; drop on empty.
  {
    const auto first = rule.target_identity.find_first_not_of(" \t\r\n");
    const auto last  = rule.target_identity.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) {
      result.valid = false;
      result.problems.push_back(
          "network rule target_identity is empty after trimming");
    } else {
      rule.target_identity =
          rule.target_identity.substr(first, last - first + 1);
    }
  }
  if (rule.target_identity.size() > kMaxTargetIdentityLength) {
    rule.target_identity.resize(kMaxTargetIdentityLength);
    note("truncated network rule target_identity");
  }

  // Name — drop illegal characters and truncate.
  rule.name.erase(
      std::remove_if(rule.name.begin(), rule.name.end(),
                     [](char ch) { return ch == '|' || ch == '\n' || ch == '\r'; }),
      rule.name.end());
  if (rule.name.size() > kMaxRuleNameLength) {
    rule.name.resize(kMaxRuleNameLength);
    note("truncated network rule name");
  }

  return result;
}

// ---------------------------------------------------------------------------
// NetworkTrafficAlertMonitor
// ---------------------------------------------------------------------------

namespace {

/// Extract the canonical metric value from a rates struct for the rule type.
std::optional<double> metricValue(const NetworkTrafficRates &rates,
                                  NetworkTrafficAlertType type) {
  switch (type) {
    case NetworkTrafficAlertType::Receive:
      return rates.rx_bytes_per_second;
    case NetworkTrafficAlertType::Transmit:
      return rates.tx_bytes_per_second;
    case NetworkTrafficAlertType::Combined: {
      if (rates.rx_bytes_per_second.has_value() &&
          rates.tx_bytes_per_second.has_value()) {
        return *rates.rx_bytes_per_second + *rates.tx_bytes_per_second;
      }
      return std::nullopt;
    }
    case NetworkTrafficAlertType::RxPackets:
      return rates.rx_packets_per_second;
    case NetworkTrafficAlertType::TxPackets:
      return rates.tx_packets_per_second;
    case NetworkTrafficAlertType::RxErrors:
      return rates.rx_errors_per_second;
    case NetworkTrafficAlertType::TxErrors:
      return rates.tx_errors_per_second;
    case NetworkTrafficAlertType::RxDropped:
      return rates.rx_dropped_per_second;
    case NetworkTrafficAlertType::TxDropped:
      return rates.tx_dropped_per_second;
  }
  return std::nullopt;
}

}  // namespace

NetworkTrafficAlertMonitor::NetworkTrafficAlertMonitor(AlertManager &alerts)
    : alerts_(alerts) {}

std::string NetworkTrafficAlertMonitor::stateKey(
    const NetworkTrafficAlertRule &rule) {
  return rule.target_identity + "|" +
         std::to_string(static_cast<int>(rule.type));
}

double NetworkTrafficAlertMonitor::recoveryFloor(double threshold) const {
  return threshold * kNetworkTrafficRecoveryFloorFraction;
}

void NetworkTrafficAlertMonitor::setEventSink(EventSink sink) {
  event_sink_ = sink;
}

void NetworkTrafficAlertMonitor::fireSink(const AlertEvent &event) {
  if (event_sink_ != nullptr) {
    event_sink_(event);
  }
}

void NetworkTrafficAlertMonitor::setRules(
    const std::vector<NetworkTrafficAlertRule> &rules) {
  // Clear any stale central-state subjects whose rules are disappearing or
  // being replaced so the dashboard never shows an active alert for a rule
  // that no longer exists.
  for (const auto &kv : states_) {
    if (!kv.second.confirmed) continue;
    // Find the old rule by matching the state key.
    for (const auto &old_rule : rules_) {
      if (stateKey(old_rule) == kv.first) {
        alerts_.clearSubject(networkTrafficAlertToAlertType(old_rule.type),
                             kv.second.last_source);
        break;
      }
    }
  }
  rules_ = rules;
  states_.clear();
  statuses_.clear();
}

AlertSeverity NetworkTrafficAlertMonitor::worstSeverity() const {
  AlertSeverity worst = AlertSeverity::Normal;
  for (const auto &s : statuses_) {
    if ((s.status == NetworkTrafficAlertRuleStatus::Exceeded ||
         s.status == NetworkTrafficAlertRuleStatus::Cooldown) &&
        s.rule.severity > worst) {
      worst = s.rule.severity;
    }
  }
  return worst;
}

void NetworkTrafficAlertMonitor::evaluate(
    const NetworkTrafficHistory &history,
    std::optional<std::chrono::steady_clock::time_point> now_opt) {
  const std::chrono::steady_clock::time_point now =
      now_opt.value_or(std::chrono::steady_clock::now());
  const NetworkTrafficTick *tick = history.lastTick();
  statuses_.clear();

  for (std::size_t i = 0; i < rules_.size(); ++i) {
    const NetworkTrafficAlertRule &rule = rules_[i];
    NetworkTrafficRuleStatus st;
    st.index = i;
    st.rule = rule;

    // --- Disabled rules: clear any stale episode and report.
    if (!rule.enabled) {
      const std::string key = stateKey(rule);
      auto it = states_.find(key);
      if (it != states_.end() && it->second.confirmed) {
        alerts_.clearSubject(networkTrafficAlertToAlertType(rule.type),
                             it->second.last_source);
      }
      if (it != states_.end()) {
        states_.erase(it);
      }
      st.status = NetworkTrafficAlertRuleStatus::Disabled;
      statuses_.push_back(std::move(st));
      continue;
    }

    // --- Check identity presence.
    bool identity_known = false;
    const NetworkTrafficRates *rates = nullptr;
    std::string display_name;
    if (tick != nullptr) {
      auto rate_it = tick->rates.find(rule.target_identity);
      auto name_it = tick->names.find(rule.target_identity);
      if (rate_it != tick->rates.end() && name_it != tick->names.end()) {
        identity_known = true;
        rates = &rate_it->second;
        display_name = name_it->second;
      }
    }

    if (!identity_known) {
      const std::string key = stateKey(rule);
      auto it = states_.find(key);
      if (it != states_.end()) {
        if (it->second.confirmed) {
          alerts_.clearSubject(networkTrafficAlertToAlertType(rule.type),
                               it->second.last_source);
        }
        states_.erase(it);
      }
      st.status = NetworkTrafficAlertRuleStatus::InterfaceUnavailable;
      statuses_.push_back(std::move(st));
      continue;
    }

    // --- Metric availability.
    const std::optional<double> value = metricValue(*rates, rule.type);
    const std::string key = stateKey(rule);
    RuleRuntime &rt = states_[key];

    if (!value.has_value()) {
      // Metric unavailable this window: freeze.  Never count, never zero.
      if (rt.confirmed) {
        st.status = NetworkTrafficAlertRuleStatus::Exceeded;
        st.detail = "metric unavailable; holding state";
      } else {
        st.status = NetworkTrafficAlertRuleStatus::WaitingForBaseline;
        st.detail = "no measurable rate yet";
      }
      statuses_.push_back(std::move(st));
      continue;
    }

    st.available = true;
    st.value = *value;

    const bool violating = *value >= rule.threshold;
    const double floor = recoveryFloor(rule.threshold);

    // --- Lambda helpers for emit/notification.
    const std::string source = display_name;
    const AlertType central_type = networkTrafficAlertToAlertType(rule.type);
    const std::string threshold_text =
        networkTrafficAlertFormatValue(rule.type, rule.threshold);
    const std::string value_text =
        networkTrafficAlertFormatValue(rule.type, *value);

    auto makeEvent = [&](AlertSeverity severity, double metric_value,
                         bool is_recovery, const std::string &msg) -> AlertEvent {
      AlertEvent e;
      e.type = central_type;
      e.severity = severity;
      e.source = source;
      e.value = metric_value;
      e.threshold = rule.threshold;
      e.is_recovery = is_recovery;
      e.message = msg;
      e.timestamp = std::chrono::system_clock::now();
      return e;
    };

    auto emitToCentral = [&](AlertSeverity severity, double metric_value,
                             const std::string &msg) {
      alerts_.recordRuleEvent(central_type, source, severity, metric_value,
                              rule.threshold, msg);
    };

    auto emitNotification = [&](const AlertEvent &event) {
      if (rule.notify_enabled) {
        fireSink(event);
      }
    };

    // --- State machine.

    if (!rt.confirmed) {
      // Not yet in an active episode.
      if (!violating) {
        rt.consecutive_violations = 0;
        rt.streak_start.reset();
        st.status = NetworkTrafficAlertRuleStatus::Below;
      } else if (rule.confirmation ==
                 NetworkTrafficAlertConfirmation::Samples) {
        rt.streak_start.reset();
        ++rt.consecutive_violations;
        st.pending_samples = rt.consecutive_violations;
        if (rt.consecutive_violations >= rule.confirmation_samples) {
          // Confirmed.
          const std::string msg =
              std::string(networkTrafficAlertTypeName(rule.type)) +
              " threshold exceeded: " + value_text + " (limit " +
              threshold_text + ")";
          emitToCentral(rule.severity, *value, msg);
          rt.confirmed = true;
          rt.last_source = source;
          emitNotification(makeEvent(rule.severity, *value, false, msg));
          if (rule.notify_enabled) {
            rt.last_emission = now;
          }
          st.status = NetworkTrafficAlertRuleStatus::Exceeded;
          st.detail = "confirmed after " +
                      std::to_string(rt.consecutive_violations) +
                      " consecutive samples";
        } else {
          st.status = NetworkTrafficAlertRuleStatus::Pending;
          st.detail = "violating for " +
                      std::to_string(rt.consecutive_violations) + "/" +
                      std::to_string(rule.confirmation_samples) + " samples";
        }
      } else {
        // Duration mode.
        rt.consecutive_violations = 0;
        if (!rt.streak_start.has_value()) {
          rt.streak_start = now;
        }
        const double elapsed =
            std::chrono::duration<double>(now - *rt.streak_start).count();
        if (elapsed >= rule.confirmation_duration_seconds) {
          const std::string msg =
              std::string(networkTrafficAlertTypeName(rule.type)) +
              " threshold exceeded: " + value_text + " (limit " +
              threshold_text + ")";
          emitToCentral(rule.severity, *value, msg);
          rt.confirmed = true;
          rt.last_source = source;
          emitNotification(makeEvent(rule.severity, *value, false, msg));
          if (rule.notify_enabled) {
            rt.last_emission = now;
          }
          st.status = NetworkTrafficAlertRuleStatus::Exceeded;
          st.detail = "confirmed after continuous violation";
        } else {
          st.status = NetworkTrafficAlertRuleStatus::Pending;
          std::ostringstream oss;
          oss << std::fixed << std::setprecision(1) << elapsed << "/"
              << std::fixed << std::setprecision(1)
              << rule.confirmation_duration_seconds << " s";
          st.detail = oss.str();
        }
      }
    } else {
      // Active confirmed episode.
      if (*value < floor) {
        // Recovery.
        const std::string msg =
            std::string(networkTrafficAlertTypeName(rule.type)) +
            " back to normal: " + value_text;
        emitToCentral(AlertSeverity::Normal, *value, msg);
        emitNotification(makeEvent(AlertSeverity::Normal, *value, true, msg));
        // Reset episode.
        rt.confirmed = false;
        rt.consecutive_violations = 0;
        rt.streak_start.reset();
        rt.last_emission.reset();
        st.status = NetworkTrafficAlertRuleStatus::Recovered;
        st.detail = "below recovery floor";
      } else if (violating) {
        // Still violating.
        const bool in_cooldown =
            rule.notify_enabled && rule.cooldown_seconds > 0 &&
            rt.last_emission.has_value() &&
            std::chrono::duration<double>(now - *rt.last_emission).count() <
                static_cast<double>(rule.cooldown_seconds);

        if (rule.repeat && rule.notify_enabled && !in_cooldown) {
          const std::string msg =
              std::string(networkTrafficAlertTypeName(rule.type)) +
              " threshold exceeded: " + value_text + " (limit " +
              threshold_text + ")";
          emitNotification(makeEvent(rule.severity, *value, false, msg));
          rt.last_emission = now;
          st.status = NetworkTrafficAlertRuleStatus::Exceeded;
          st.detail = "repeat notification sent";
        } else if (rule.repeat && rule.notify_enabled && in_cooldown) {
          st.status = NetworkTrafficAlertRuleStatus::Cooldown;
          const double remaining =
              static_cast<double>(rule.cooldown_seconds) -
              std::chrono::duration<double>(now - *rt.last_emission).count();
          std::ostringstream oss;
          oss << "next repeat in " << std::fixed << std::setprecision(1)
              << remaining << " s";
          st.detail = oss.str();
        } else {
          st.status = NetworkTrafficAlertRuleStatus::Exceeded;
        }
      } else {
        // In hysteresis band: below threshold but not below recovery floor.
        st.status = NetworkTrafficAlertRuleStatus::Exceeded;
        st.detail = "below threshold; awaiting recovery";
      }
    }

    statuses_.push_back(std::move(st));
  }
}

}  // namespace atm