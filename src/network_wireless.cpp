#include "network_wireless.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>

namespace atm {

namespace {

/// Strips leading/trailing ASCII whitespace (including a CR).
std::string trimWhitespace(const std::string &text) {
  std::size_t begin = 0;
  while (begin < text.size() &&
         (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r')) {
    ++begin;
  }
  std::size_t end = text.size();
  while (end > begin &&
         (text[end - 1] == ' ' || text[end - 1] == '\t' ||
          text[end - 1] == '\r')) {
    --end;
  }
  return text.substr(begin, end - begin);
}

/// Formats a system_clock time_point as a local "YYYY-MM-DD HH:MM:SS" string.
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

/// Reads and trims one sysfs attribute file. Returns std::nullopt for an
/// absent/empty file; `denied` is set when the file exists but is not readable
/// (a permission problem — unlike a clean absence, this is temporary).
std::optional<std::string> readField(const std::filesystem::path &file,
                                     bool &denied) {
  denied = false;
  std::error_code ec;
  const std::filesystem::file_status status =
      std::filesystem::status(file, ec);
  if (ec) {
    if (ec == std::errc::permission_denied) {
      denied = true;
    }
    return std::nullopt;
  }
  if (!std::filesystem::exists(status)) {
    return std::nullopt;
  }
  std::ifstream in(file);
  if (!in) {
    denied = true;  // exists but unreadable — temporary, never a clean "none"
    return std::nullopt;
  }
  std::string text;
  if (!std::getline(in, text)) {
    return std::nullopt;
  }
  text = trimWhitespace(text);
  return text.empty() ? std::nullopt
                      : std::optional<std::string>(std::move(text));
}

/// Strict signed decimal parse used by the /proc/net/wireless columns. The WE
/// numbers are printed by the kernel as e.g. "-40." or "54."; the optional
/// trailing '.' (and any surrounding whitespace) is accepted and the value must
/// fit in `int`. Never clamps and never guesses.
bool parseSignedDecimal(const std::string &text, int &value) {
  if (text.empty()) {
    return false;
  }
  errno = 0;
  char *end = nullptr;
  const double parsed = std::strtod(text.c_str(), &end);
  if (errno == ERANGE || end == text.c_str()) {
    return false;
  }
  // Accept only fully consumed input, possibly with a trailing '.' (the
  // kernel's "%d." formatting of WIRELESS_EXT values).
  if (*end == '.') {
    ++end;
  }
  while (*end == ' ' || *end == '\t') {
    ++end;
  }
  if (*end != '\0') {
    return false;
  }
  if (parsed < static_cast<double>(std::numeric_limits<int>::min()) ||
      parsed > static_cast<double>(std::numeric_limits<int>::max())) {
    return false;
  }
  value = static_cast<int>(parsed);
  return true;
}

/// True when the ARPHRD link type belongs to the IEEE80211 family. The values
/// are literal to stay portable across libc headers: 801 (ARPHRD_IEEE80211),
/// 802 (ARPHRD_IEEE80211_PRISM), 803 (ARPHRD_IEEE80211_RADIOTAP),
/// 804 (ARPHRD_IEEE80211_RADIOTAP_NG), 805 (ARPHRD_IEEE80211_MESH).
bool arphrdIsIeee80211(unsigned arphrd) {
  return arphrd == 801u || arphrd == 802u || arphrd == 803u ||
         arphrd == 804u || arphrd == 805u;
}

/// Builds this tick's connection-event observation from a wireless record.
/// Frequency / channel / AP identity are not exposed by the approved native
/// sources (sysfs, /proc/net/wireless), so they always stay nullopt/unreliable
/// in production — roaming is thereby never falsely reported.
WirelessObservation makeWirelessObservation(const NetworkWirelessInfo &wireless,
                                            bool present) {
  WirelessObservation observation;
  observation.present = present;
  observation.wireless =
      present && wireless.presence == WirelessPresence::Wireless;
  observation.association = wireless.association;
  if (wireless.signal_dbm.has_value()) {
    observation.signal_dbm = static_cast<double>(*wireless.signal_dbm);
  }
  return observation;
}

/// The observation describing an interface that has vanished from the
/// discovery snapshot this tick.
WirelessObservation makeGoneObservation() {
  WirelessObservation observation;
  observation.present = false;
  observation.wireless = false;
  observation.association = WirelessAssociation::Unknown;
  return observation;
}

}  // namespace

const char *wirelessPresenceName(WirelessPresence presence) {
  switch (presence) {
    case WirelessPresence::Unknown:
      return "unknown";
    case WirelessPresence::NotWireless:
      return "not wireless";
    case WirelessPresence::Wireless:
      return "wireless";
  }
  return "unknown";
}

const char *wirelessAssociationName(WirelessAssociation association) {
  switch (association) {
    case WirelessAssociation::Unknown:
      return "unknown";
    case WirelessAssociation::Associated:
      return "associated";
    case WirelessAssociation::Disconnected:
      return "disconnected";
    case WirelessAssociation::Unavailable:
      return "unavailable";
  }
  return "unknown";
}

const char *wirelessFieldStateName(WirelessFieldState state) {
  switch (state) {
    case WirelessFieldState::Unknown:
      return "unknown";
    case WirelessFieldState::Available:
      return "available";
    case WirelessFieldState::Stale:
      return "stale";
    case WirelessFieldState::Unavailable:
      return "unavailable";
  }
  return "unknown";
}

WirelessProbe readWirelessProbe(const std::filesystem::path &root,
                                const std::string &iface) {
  WirelessProbe probe;
  const std::filesystem::path iface_dir =
      root / "sys" / "class" / "net" / iface;

  // The phy80211 directory is the conclusive mac80211 signal.
  std::error_code ec;
  bool is_phy_dir =
      std::filesystem::is_directory(iface_dir / "phy80211", ec);
  if (ec) {
    if (ec == std::errc::permission_denied) {
      probe.access_denied = true;
      probe.probe_ok = false;
    }
    is_phy_dir = false;
  }
  if (is_phy_dir) {
    probe.phy80211_present = true;
    probe.metadata_available = true;
    bool denied = false;
    if (const std::optional<std::string> name =
            readField(iface_dir / "phy80211" / "name", denied)) {
      probe.phy_name = *name;
    } else if (denied) {
      probe.access_denied = true;
      probe.probe_ok = false;
    }
    if (const std::optional<std::string> index =
            readField(iface_dir / "phy80211" / "index", denied)) {
      probe.phy_index = parseSysfsInt(*index);
    } else if (denied) {
      probe.access_denied = true;
      probe.probe_ok = false;
    }
  }

  // The legacy WIRELESS_EXT directory (usually absent on modern mac80211).
  std::error_code wec;
  const bool is_we_dir =
      std::filesystem::is_directory(iface_dir / "wireless", wec);
  if (wec) {
    if (wec == std::errc::permission_denied) {
      probe.access_denied = true;
      probe.probe_ok = false;
    }
  } else if (is_we_dir) {
    probe.wireless_dir_present = true;
    probe.metadata_available = true;
  }

  return probe;
}

ProcWirelessStats parseProcNetWirelessStats(const std::string &text,
                                            const std::string &iface) {
  ProcWirelessStats stats;
  std::istringstream input(text);
  std::string line;
  while (std::getline(input, line)) {
    if (trimWhitespace(line).empty()) {
      continue;
    }
    std::istringstream line_stream(line);
    std::string field;
    std::vector<std::string> fields;
    while (line_stream >> field) {
      fields.push_back(std::move(field));
    }
    if (fields.size() < 5) {
      continue;
    }
    std::string name = fields[0];
    if (!name.empty() && name.back() == ':') {
      name.pop_back();
    }
    if (name != iface) {
      continue;
    }
    stats.present = true;
    // Kernel columns (net/wireless/core.c, "%6s: %04x  %3d.  %3d.  %3d ..."):
    // [0] face, [1] updated/status, [2] link (qual), [3] level, [4] noise.
    // The status column is real and always present, so the signal level and
    // noise floor are fields 3 and 4 — never 2/3 (field 2 is the driver-scale
    // link quality).
    int value = 0;
    if (parseSignedDecimal(fields[3], value) &&
        value != kIwQualInvalidSentinel) {
      stats.level_dbm = value;
    }
    if (parseSignedDecimal(fields[4], value) &&
        value != kIwQualInvalidSentinel) {
      stats.noise_dbm = value;
    }
    // The "link" quality column (field 2) is intentionally NOT captured: its
    // scale is driver-defined and would be misleading as a cross-driver
    // percentage.
    break;  // one line per interface
  }
  return stats;
}

ProcWirelessStats readProcNetWirelessStats(const std::filesystem::path &path,
                                           const std::string &iface) {
  std::ifstream in(path);
  if (!in) {
    return {};  // no /proc/net/wireless (or unreadable) — never an error
  }
  std::string text((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  return parseProcNetWirelessStats(text, iface);
}

NetworkWirelessInfo judgeWireless(const NetworkInterfaceInfo &info,
                                  const WirelessProbe &probe,
                                  const ProcWirelessStats &proc,
                                  const NetworkWirelessInfo &previous) {
  NetworkWirelessInfo w = previous;  // keeps identity, first_seen, history
  w.name = info.name;

  const bool arphrd_ieee =
      info.link.link_type.has_value() && arphrdIsIeee80211(*info.link.link_type);
  const bool now_wireless = info.wireless.present || arphrd_ieee ||
                            probe.phy80211_present;
  w.presence =
      now_wireless ? WirelessPresence::Wireless : WirelessPresence::NotWireless;

  if (w.presence != WirelessPresence::Wireless) {
    // Cleanly not wireless (or against a strict classification): no invented
    // values, no stale carry-over for a link that is not wireless anymore.
    w.link_quality.reset();
    w.signal_dbm.reset();
    w.noise_dbm.reset();
    w.phy_name.clear();
    w.phy_index.reset();
    w.is_mac80211 = false;
    w.field_state = WirelessFieldState::Unavailable;
    w.phy_state = WirelessFieldState::Unavailable;
    w.association = WirelessAssociation::Unknown;
    w.carrier_exposed = info.link.carrier.has_value();
    w.has_carrier = info.link.carrier == 1;
    w.enabled = info.link.admin_up;
    return w;
  }

  // Static PHY metadata (from the cached probe). A permission failure is
  // temporary: preserve the last known PHY and mark it stale.
  w.is_mac80211 = probe.phy80211_present;
  if (probe.phy80211_present) {
    w.phy_name = probe.phy_name;
    w.phy_index = probe.phy_index;
    w.phy_state = WirelessFieldState::Available;
  } else if (probe.access_denied) {
    w.phy_state = (previous.phy_state == WirelessFieldState::Available ||
                   previous.phy_state == WirelessFieldState::Stale)
                      ? WirelessFieldState::Stale
                      : WirelessFieldState::Unavailable;
  } else {
    w.phy_state = WirelessFieldState::Unavailable;
    w.phy_name.clear();
    w.phy_index.reset();
  }

  // Dynamic WIRELESS_EXT values come straight from the per-tick snapshot;
  // /proc/net/wireless is only a native fallback for interfaces that expose no
  // /wireless directory. Updating always wins; a temporary loss of values
  // preserves the last valid reading as stale; a clean absence is unavailable.
  bool updated = false;
  if (info.wireless.link.has_value()) {
    w.link_quality = *info.wireless.link;
    updated = true;
  }
  if (info.wireless.level.has_value()) {
    w.signal_dbm = *info.wireless.level;
    updated = true;
  }
  if (info.wireless.noise.has_value()) {
    w.noise_dbm = *info.wireless.noise;
    updated = true;
  }
  if (!updated && proc.present) {
    if (proc.level_dbm.has_value()) {
      w.signal_dbm = *proc.level_dbm;
      updated = true;
    }
    if (proc.noise_dbm.has_value()) {
      w.noise_dbm = *proc.noise_dbm;
      updated = true;
    }
  }
  const bool has_values = w.signal_dbm.has_value() || w.noise_dbm.has_value() ||
                          w.link_quality.has_value();
  if (updated) {
    w.field_state = WirelessFieldState::Available;
  } else if (has_values) {
    w.field_state = WirelessFieldState::Stale;  // preserved from previous
  } else {
    w.field_state = WirelessFieldState::Unavailable;
  }

  // Association is derived from the kernel carrier bit (mac80211 raises the
  // carrier on association). Without a carrier file, association cannot be
  // confirmed — an "up" operstate alone never implies association.
  w.carrier_exposed = info.link.carrier.has_value();
  w.has_carrier = info.link.carrier == 1;
  w.enabled = info.link.admin_up;
  if (w.carrier_exposed) {
    w.association = w.has_carrier ? WirelessAssociation::Associated
                                  : WirelessAssociation::Disconnected;
  } else {
    w.association = WirelessAssociation::Unknown;
  }
  return w;
}

std::string formatWirelessSignal(const std::optional<int> &signal_dbm) {
  if (!signal_dbm.has_value()) {
    return "N/A";
  }
  return std::to_string(*signal_dbm) + " dBm";
}

std::string formatWirelessLinkQuality(const std::optional<int> &link,
                                      bool is_mac80211) {
  if (!link.has_value()) {
    return "N/A";
  }
  std::string value = std::to_string(*link);
  if (is_mac80211) {
    value += " /" + std::to_string(kWirelessMac80211WextQualityMax) +
             " (mac80211 driver scale)";
  }
  return value;
}

std::string formatWirelessNoise(const std::optional<int> &noise_dbm) {
  if (!noise_dbm.has_value()) {
    return "N/A";
  }
  return std::to_string(*noise_dbm) + " dBm";
}

std::string formatWirelessBitrate(const std::optional<double> &bps) {
  if (!bps.has_value() || !std::isfinite(*bps) || *bps < 0.0) {
    return "N/A";
  }
  const double value = *bps;
  const double kbps = value / 1000.0;
  const double mbps = kbps / 1000.0;
  const double gbps = mbps / 1000.0;
  std::ostringstream out;
  if (gbps >= 1.0) {
    out << std::fixed << std::setprecision(1) << gbps << " Gb/s";
  } else if (mbps >= 1.0) {
    const double rounded = std::round(mbps);
    if (std::abs(mbps - rounded) < 0.05) {
      out << static_cast<long long>(rounded) << " Mb/s";
    } else {
      out << std::fixed << std::setprecision(1) << mbps << " Mb/s";
    }
  } else if (kbps >= 1.0) {
    out << std::fixed << std::setprecision(0) << kbps << " Kb/s";
  } else {
    out << std::fixed << std::setprecision(0) << value << " b/s";
  }
  return out.str();
}

std::string formatWirelessFrequency(const std::optional<double> &mhz) {
  if (!mhz.has_value() || !std::isfinite(*mhz) || *mhz < 0.0) {
    return "N/A";
  }
  const double rounded = std::round(*mhz);
  if (std::abs(*mhz - rounded) < 0.05) {
    return std::to_string(static_cast<long long>(rounded)) + " MHz";
  }
  std::ostringstream out;
  out << std::fixed << std::setprecision(1) << *mhz << " MHz";
  return out.str();
}

std::string formatWirelessChannel(const std::optional<int> &channel) {
  if (!channel.has_value()) {
    return "N/A";
  }
  return std::to_string(*channel);
}

WirelessHistorySummary summarizeWirelessHistory(const NetworkWirelessInfo &wireless,
                                                std::size_t max_samples) {
  WirelessHistorySummary summary;
  const auto &samples = wireless.history.samples();
  summary.sample_count = samples.size();
  if (summary.sample_count == 0) {
    return summary;
  }
  summary.has_data = true;

  for (const WirelessHistorySample &sample : samples) {
    if (sample.valid) {
      ++summary.valid_sample_count;
    }
    if (sample.signal_dbm.has_value()) {
      ++summary.signal_sample_count;
      const double value = *sample.signal_dbm;
      summary.current_signal_dbm = value;
      if (!summary.min_signal_dbm.has_value() ||
          value < *summary.min_signal_dbm) {
        summary.min_signal_dbm = value;
      }
      if (!summary.max_signal_dbm.has_value() ||
          value > *summary.max_signal_dbm) {
        summary.max_signal_dbm = value;
      }
      summary.avg_signal_dbm = summary.avg_signal_dbm.value_or(0.0) + value;
    }
    if (sample.link_quality.has_value()) {
      ++summary.link_quality_sample_count;
      const int value = *sample.link_quality;
      summary.current_link_quality = value;
      if (!summary.min_link_quality.has_value() ||
          value < *summary.min_link_quality) {
        summary.min_link_quality = value;
      }
      if (!summary.max_link_quality.has_value() ||
          value > *summary.max_link_quality) {
        summary.max_link_quality = value;
      }
      summary.avg_link_quality =
          summary.avg_link_quality.value_or(0.0) +
          static_cast<double>(value);
    }
    if (sample.bitrate_bps.has_value()) {
      ++summary.bitrate_sample_count;
      const double value = *sample.bitrate_bps;
      summary.current_bitrate_bps = value;
      if (!summary.min_bitrate_bps.has_value() ||
          value < *summary.min_bitrate_bps) {
        summary.min_bitrate_bps = value;
      }
      if (!summary.max_bitrate_bps.has_value() ||
          value > *summary.max_bitrate_bps) {
        summary.max_bitrate_bps = value;
      }
      summary.avg_bitrate_bps = summary.avg_bitrate_bps.value_or(0.0) + value;
    }
    if (sample.frequency_mhz.has_value()) {
      ++summary.frequency_sample_count;
      const double value = *sample.frequency_mhz;
      summary.current_frequency_mhz = value;
      if (!summary.min_frequency_mhz.has_value() ||
          value < *summary.min_frequency_mhz) {
        summary.min_frequency_mhz = value;
      }
      if (!summary.max_frequency_mhz.has_value() ||
          value > *summary.max_frequency_mhz) {
        summary.max_frequency_mhz = value;
      }
      summary.avg_frequency_mhz =
          summary.avg_frequency_mhz.value_or(0.0) + value;
    }
    if (sample.channel.has_value()) {
      ++summary.channel_sample_count;
      summary.current_channel = *sample.channel;
    }
  }

  if (summary.signal_sample_count > 0) {
    summary.avg_signal_dbm =
        *summary.avg_signal_dbm / static_cast<double>(summary.signal_sample_count);
  } else {
    summary.avg_signal_dbm.reset();
  }
  if (summary.link_quality_sample_count > 0) {
    summary.avg_link_quality =
        *summary.avg_link_quality /
        static_cast<double>(summary.link_quality_sample_count);
  } else {
    summary.avg_link_quality.reset();
  }
  if (summary.bitrate_sample_count > 0) {
    summary.avg_bitrate_bps =
        *summary.avg_bitrate_bps /
        static_cast<double>(summary.bitrate_sample_count);
  } else {
    summary.avg_bitrate_bps.reset();
  }
  if (summary.frequency_sample_count > 0) {
    summary.avg_frequency_mhz =
        *summary.avg_frequency_mhz /
        static_cast<double>(summary.frequency_sample_count);
  } else {
    summary.avg_frequency_mhz.reset();
  }

  if (max_samples > 0) {
    summary.coverage = std::min(
        1.0, static_cast<double>(summary.sample_count) /
                 static_cast<double>(max_samples));
    summary.valid_coverage = std::min(
        1.0, static_cast<double>(summary.valid_sample_count) /
                 static_cast<double>(max_samples));
    summary.history_complete = summary.sample_count >= max_samples;
  }
  summary.span_seconds =
      std::chrono::duration<double>(samples.back().timestamp -
                                    samples.front().timestamp)
          .count();
  summary.event_count = wireless.connection_events.size();
  summary.last_update = wireless.last_sample_wall;
  return summary;
}

// -------------------------------------------------------------------------
// Step 52 — Wireless connection quality summary helpers (internal).
// -------------------------------------------------------------------------

/// Population standard deviation of the provided values. Returns nullopt for
/// fewer than two values (stddev is undefined for zero or one sample).
std::optional<double> populationStddev(const std::vector<double> &values) {
  if (values.size() < 2) {
    return std::nullopt;
  }
  double sum = 0.0;
  for (double v : values) {
    sum += v;
  }
  const double mean = sum / static_cast<double>(values.size());
  double sq = 0.0;
  for (double v : values) {
    const double d = v - mean;
    sq += d * d;
  }
  return std::sqrt(sq / static_cast<double>(values.size()));
}

/// Clamp a value into [lo, hi].
double clampUnit(double value, double lo, double hi) {
  if (value < lo) {
    return lo;
  }
  if (value > hi) {
    return hi;
  }
  return value;
}

/// Factor weights (must sum to 1.0 before exclusion/renormalisation). Each
/// factor is fully documented in the header doc-comment for
/// WirelessQualitySummary; these constants centralise the numeric values.
/// Connectivity factors dominate (0.30 + 0.25) so an interface that mostly
/// stays associated but weak/flappy cannot earn a flattering grade from the
/// smaller signal/availability factors alone.
constexpr double kContinuityWeight = 0.30;
constexpr double kDisconnectionWeight = 0.25;
constexpr double kRecoveryWeight = 0.10;
constexpr double kSignalStrengthWeight = 0.15;
constexpr double kSignalVariabilityWeight = 0.05;
constexpr double kAvailabilityWeight = 0.10;
constexpr double kRoamingWeight = 0.05;

/// Reference durations/scales for the individual factors (seconds).
constexpr double kRecoveryReferenceSeconds = 60.0;
constexpr double kSignalVariabilityReferenceDbm = 6.0;
constexpr double kAvailabilityReferenceCount = 3.0;
constexpr double kRoamingReferenceCount = 2.0;
constexpr double kDisconnectionScalePerHour = 10.0;

/// Mapping from average signal dBm to [0,1]: -30 dBm → 1.0, -90 dBm → 0.0,
/// linearly clamped to the band.
double signalQualityFromAvgDbm(double avg_dbm) {
  const double span = kWirelessSignalBestDbm - kWirelessSignalWorstDbm;
  if (span <= 0.0) {
    return 0.5;
  }
  return clampUnit((avg_dbm - kWirelessSignalWorstDbm) / span, 0.0, 1.0);
}

/// Weighted, clamped, renormalised stability score. Returns nullopt when the
/// input factor list is empty (all factors excluded). Factors with nullopt
/// value never participate.
std::optional<double> computeWeightedScore(
    const std::vector<WirelessStabilityFactor> &factors) {
  double numerator = 0.0;
  double denominator = 0.0;
  for (const auto &f : factors) {
    if (!f.value.has_value()) {
      continue;
    }
    numerator += f.weight * *f.value;
    denominator += f.weight;
  }
  if (denominator <= 0.0) {
    return std::nullopt;
  }
  return 100.0 * (numerator / denominator);
}

/// Classify a score into a stability band.
WirelessStability classifyScore(double score) {
  if (score >= kWirelessExcellentThreshold) {
    return WirelessStability::Excellent;
  }
  if (score >= kWirelessGoodThreshold) {
    return WirelessStability::Good;
  }
  if (score >= kWirelessFairThreshold) {
    return WirelessStability::Fair;
  }
  return WirelessStability::Poor;
}

/// Core stability classification. Builds the individual factors, renormalises
/// their weights, computes the weighted score and assigns a stability band.
WirelessStabilityAssessment assessWirelessStability(
    const NetworkWirelessInfo &wireless,
    const WirelessQualitySummary &q) {
  WirelessStabilityAssessment result;
  std::vector<WirelessStabilityFactor> factors;

  const double determined =
      q.connected_seconds + q.disconnected_seconds;

  // Factor 1: connection continuity (connected / determined).
  if (determined >= kWirelessQualityMinDeterminedSeconds) {
    const double v = q.connected_seconds / determined;
    WirelessStabilityFactor f;
    f.name = "connection continuity";
    f.value = clampUnit(v, 0.0, 1.0);
    f.weight = kContinuityWeight;
    std::ostringstream det;
    det << std::fixed << std::setprecision(1) << q.connected_seconds
        << " s connected / " << std::fixed << std::setprecision(1) << determined
        << " s determined";
    f.detail = det.str();
    factors.push_back(std::move(f));
  }

  // Factor 2: disconnection frequency. Penalises both the fraction of the
  // determined window actually spent disconnected and the rate of recorded
  // disconnection events (10/h is the reference scale for the rate term), so
  // an interface that oscillates between associated and disconnected is never
  // rewarded just because its disconnects were short.
  if (determined >= kWirelessQualityMinDeterminedSeconds) {
    const double hours = determined / 3600.0;
    const double rate =
        hours > 0.0 ? static_cast<double>(q.disconnection_count) / hours : 0.0;
    const double disconnected_fraction =
        determined > 0.0 ? q.disconnected_seconds / determined : 0.0;
    const double v = (1.0 - disconnected_fraction) *
                     (1.0 / (1.0 + rate / kDisconnectionScalePerHour));
    WirelessStabilityFactor f;
    f.name = "disconnection frequency";
    f.value = clampUnit(v, 0.0, 1.0);
    f.weight = kDisconnectionWeight;
    std::ostringstream det;
    det << std::fixed << std::setprecision(1)
        << (disconnected_fraction * 100.0) << "% time disconnected, "
        << std::fixed << std::setprecision(2) << rate << " disconnects/hour";
    f.detail = det.str();
    factors.push_back(std::move(f));
  }

  // Factor 3: recovery speed (penalise long outages; 60 s is the reference).
  {
    const double v =
        1.0 - clampUnit(q.longest_disconnected_seconds /
                            kRecoveryReferenceSeconds,
                        0.0, 1.0);
    WirelessStabilityFactor f;
    f.name = "recovery speed";
    f.value = clampUnit(v, 0.0, 1.0);
    f.weight = kRecoveryWeight;
    std::ostringstream det;
    det << "longest outage " << std::fixed << std::setprecision(1)
        << q.longest_disconnected_seconds << " s";
    f.detail = det.str();
    factors.push_back(std::move(f));
  }

  // Factor 4: signal strength (quality from average dBm).
  if (q.signal_sample_count >= kWirelessQualityMinFactorSamples &&
      q.avg_signal_dbm.has_value()) {
    const double v = signalQualityFromAvgDbm(*q.avg_signal_dbm);
    WirelessStabilityFactor f;
    f.name = "signal strength";
    f.value = clampUnit(v, 0.0, 1.0);
    f.weight = kSignalStrengthWeight;
    std::ostringstream det;
    det << "avg " << std::fixed << std::setprecision(1) << *q.avg_signal_dbm
        << " dBm";
    f.detail = det.str();
    factors.push_back(std::move(f));
  }

  // Factor 5: signal variability (penalise high stddev; 6 dBm is the
  // reference).
  if (q.signal_stddev_dbm.has_value()) {
    const double v =
        1.0 - clampUnit(*q.signal_stddev_dbm / kSignalVariabilityReferenceDbm,
                        0.0, 1.0);
    WirelessStabilityFactor f;
    f.name = "signal stability";
    f.value = clampUnit(v, 0.0, 1.0);
    f.weight = kSignalVariabilityWeight;
    std::ostringstream det;
    det << "stddev " << std::fixed << std::setprecision(1)
        << *q.signal_stddev_dbm << " dBm";
    f.detail = det.str();
    factors.push_back(std::move(f));
  }

  // Factor 6: interface availability (penalise unavailable episodes; 3 is the
  // reference).
  {
    const double v =
        1.0 - clampUnit(
                  static_cast<double>(q.interface_unavailable_count) /
                      kAvailabilityReferenceCount,
                  0.0, 1.0);
    WirelessStabilityFactor f;
    f.name = "interface availability";
    f.value = clampUnit(v, 0.0, 1.0);
    f.weight = kAvailabilityWeight;
    std::ostringstream det;
    det << q.interface_unavailable_count << " unavailable";
    f.detail = det.str();
    factors.push_back(std::move(f));
  }

  // Factor 7: roaming (penalise frequent roaming; 2 is the reference). Only
  // included when the retained connection-event history shows at least one
  // event with reliable AP identity — otherwise roaming cannot be reliably
  // detected and the factor is omitted rather than rewarded by default.
  {
    bool reliable_ap_identity = false;
    for (const auto &event : wireless.connection_events.samples()) {
      if (event.ap_identity_reliable) {
        reliable_ap_identity = true;
        break;
      }
    }
    if (reliable_ap_identity) {
      const double v =
          1.0 - clampUnit(static_cast<double>(q.roaming_count) /
                              kRoamingReferenceCount,
                          0.0, 1.0);
      WirelessStabilityFactor f;
      f.name = "roaming";
      f.value = clampUnit(v, 0.0, 1.0);
      f.weight = kRoamingWeight;
      std::ostringstream det;
      det << q.roaming_count << " roam events";
      f.detail = det.str();
      factors.push_back(std::move(f));
    }
  }

  // Need at least 2 factors remaining before classifying; otherwise the
  // renormalised weights become over-sensitive to a single measurement.
  std::size_t present_count = 0;
  for (const auto &f : factors) {
    if (f.value.has_value()) {
      ++present_count;
    }
  }
  if (present_count < 2) {
    return result;
  }

  result.factors = std::move(factors);
  result.score = computeWeightedScore(result.factors);
  if (result.score.has_value()) {
    result.stability = classifyScore(*result.score);
  }
  return result;
}

// -------------------------------------------------------------------------
// Step 52 — Public functions.
// -------------------------------------------------------------------------

const char *wirelessStabilityName(WirelessStability stability) {
  switch (stability) {
    case WirelessStability::Unknown:
      return "unknown";
    case WirelessStability::Poor:
      return "poor";
    case WirelessStability::Fair:
      return "fair";
    case WirelessStability::Good:
      return "good";
    case WirelessStability::Excellent:
      return "excellent";
  }
  return "unknown";
}

WirelessQualitySummary summarizeWirelessQuality(
    const NetworkWirelessInfo &wireless, std::size_t max_samples) {
  WirelessQualitySummary q;
  const auto &samples = wireless.history.samples();
  q.sample_count = samples.size();
  if (q.sample_count == 0 && wireless.connection_events.empty()) {
    return q;
  }
  q.has_data = true;

  // --- Valid / coverage counts ------------------------------------------------
  for (const WirelessHistorySample &sample : samples) {
    if (sample.valid) {
      ++q.valid_sample_count;
    }
    if (sample.signal_dbm.has_value()) {
      ++q.signal_sample_count;
    }
    if (sample.bitrate_bps.has_value()) {
      ++q.bitrate_sample_count;
    }
  }
  if (max_samples > 0) {
    q.coverage = std::min(
        1.0, static_cast<double>(q.sample_count) /
                 static_cast<double>(max_samples));
    q.valid_coverage = std::min(
        1.0, static_cast<double>(q.valid_sample_count) /
                 static_cast<double>(max_samples));
  }
  if (q.valid_sample_count > 0) {
    q.valid_with_signal = std::min(
        1.0, static_cast<double>(q.signal_sample_count) /
                 static_cast<double>(q.valid_sample_count));
    q.valid_with_bitrate = std::min(
        1.0, static_cast<double>(q.bitrate_sample_count) /
                 static_cast<double>(q.valid_sample_count));
  }

  // --- Temporary gaps (invalid tick count) ------------------------------------
  q.temporary_gap_count = 0;
  for (const WirelessHistorySample &sample : samples) {
    if (!sample.valid) {
      ++q.temporary_gap_count;
    }
  }

  // --- Signal stats -----------------------------------------------------------
  std::vector<double> signal_values;
  signal_values.reserve(q.signal_sample_count);
  for (const WirelessHistorySample &sample : samples) {
    if (sample.signal_dbm.has_value()) {
      q.current_signal_dbm = *sample.signal_dbm;
      signal_values.push_back(*sample.signal_dbm);
      if (!q.min_signal_dbm.has_value() || *sample.signal_dbm < *q.min_signal_dbm) {
        q.min_signal_dbm = *sample.signal_dbm;
      }
      if (!q.max_signal_dbm.has_value() || *sample.signal_dbm > *q.max_signal_dbm) {
        q.max_signal_dbm = *sample.signal_dbm;
      }
    }
  }
  if (!signal_values.empty()) {
    double sum = 0.0;
    for (double v : signal_values) {
      sum += v;
    }
    q.avg_signal_dbm = sum / static_cast<double>(signal_values.size());
    q.signal_stddev_dbm = populationStddev(signal_values);
  }

  // --- Bitrate stats ----------------------------------------------------------
  std::vector<double> bitrate_values;
  bitrate_values.reserve(q.bitrate_sample_count);
  for (const WirelessHistorySample &sample : samples) {
    if (sample.bitrate_bps.has_value()) {
      q.current_bitrate_bps = *sample.bitrate_bps;
      bitrate_values.push_back(*sample.bitrate_bps);
      if (!q.min_bitrate_bps.has_value() ||
          *sample.bitrate_bps < *q.min_bitrate_bps) {
        q.min_bitrate_bps = *sample.bitrate_bps;
      }
      if (!q.max_bitrate_bps.has_value() ||
          *sample.bitrate_bps > *q.max_bitrate_bps) {
        q.max_bitrate_bps = *sample.bitrate_bps;
      }
    }
  }
  if (!bitrate_values.empty()) {
    double sum = 0.0;
    for (double v : bitrate_values) {
      sum += v;
    }
    q.avg_bitrate_bps = sum / static_cast<double>(bitrate_values.size());
    q.bitrate_stddev_bps = populationStddev(bitrate_values);
  }

  // --- Window and span --------------------------------------------------------
  if (q.sample_count >= 2) {
    q.span_seconds = std::chrono::duration<double>(
                         samples.back().timestamp - samples.front().timestamp)
                         .count();
  }
  q.window_end = wireless.last_sample_wall;
  if (q.sample_count >= 2 && q.window_end != std::chrono::system_clock::time_point{}) {
    q.window_start = q.window_end - std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                        std::chrono::duration<double>(q.span_seconds));
  } else {
    q.window_start = q.window_end;
  }

  // --- Connectivity durations -------------------------------------------------
  const double max_gap_seconds =
      static_cast<double>(kWirelessQualityMaxGap.count());

  enum class RunState { None, Connected, Disconnected };
  RunState current_run = RunState::None;
  double current_run_seconds = 0.0;

  for (std::size_t i = 1; i < q.sample_count; ++i) {
    const double delta = std::chrono::duration<double>(
                             samples[i].timestamp - samples[i - 1].timestamp)
                             .count();
    if (delta <= 0.0) {
      continue;
    }

    // Long gap or unknown/unavailable state → unobserved, break any run.
    const WirelessAssociation prev_state = samples[i - 1].association;
    if (delta > max_gap_seconds ||
        prev_state == WirelessAssociation::Unknown ||
        prev_state == WirelessAssociation::Unavailable) {
      q.unobserved_seconds += delta;
      if (current_run == RunState::Connected &&
          current_run_seconds > q.longest_connected_seconds) {
        q.longest_connected_seconds = current_run_seconds;
      }
      if (current_run == RunState::Disconnected &&
          current_run_seconds > q.longest_disconnected_seconds) {
        q.longest_disconnected_seconds = current_run_seconds;
      }
      current_run = RunState::None;
      current_run_seconds = 0.0;
      continue;
    }

    if (prev_state == WirelessAssociation::Associated) {
      q.connected_seconds += delta;
      if (current_run == RunState::Connected) {
        current_run_seconds += delta;
      } else {
        if (current_run == RunState::Connected &&
            current_run_seconds > q.longest_connected_seconds) {
          q.longest_connected_seconds = current_run_seconds;
        }
        if (current_run == RunState::Disconnected &&
            current_run_seconds > q.longest_disconnected_seconds) {
          q.longest_disconnected_seconds = current_run_seconds;
        }
        current_run = RunState::Connected;
        current_run_seconds = delta;
      }
    } else {
      // Disconnected (carrier clear, not Unknown/Unavailable which are above).
      q.disconnected_seconds += delta;
      if (current_run == RunState::Disconnected) {
        current_run_seconds += delta;
      } else {
        if (current_run == RunState::Connected &&
            current_run_seconds > q.longest_connected_seconds) {
          q.longest_connected_seconds = current_run_seconds;
        }
        if (current_run == RunState::Disconnected &&
            current_run_seconds > q.longest_disconnected_seconds) {
          q.longest_disconnected_seconds = current_run_seconds;
        }
        current_run = RunState::Disconnected;
        current_run_seconds = delta;
      }
    }
  }
  // Flush the final open run.
  if (current_run == RunState::Connected &&
      current_run_seconds > q.longest_connected_seconds) {
    q.longest_connected_seconds = current_run_seconds;
  }
  if (current_run == RunState::Disconnected &&
      current_run_seconds > q.longest_disconnected_seconds) {
    q.longest_disconnected_seconds = current_run_seconds;
  }

  // --- Event counts -----------------------------------------------------------
  for (const auto &event : wireless.connection_events.samples()) {
    switch (event.type) {
      case WirelessConnectionEventType::Disassociated:
        ++q.disconnection_count;
        break;
      case WirelessConnectionEventType::Reconnected:
        ++q.reconnection_count;
        break;
      case WirelessConnectionEventType::Associated:
        ++q.association_count;
        break;
      case WirelessConnectionEventType::Roamed:
        ++q.roaming_count;
        break;
      case WirelessConnectionEventType::InterfaceUnavailable:
        ++q.interface_unavailable_count;
        break;
      default:
        break;
    }
  }

  // --- Last update ------------------------------------------------------------
  q.last_update = wireless.last_sample_wall;

  // --- Stability assessment ---------------------------------------------------
  // Sufficient-data gate: at least a minimum number of freshly-read samples,
  // plus some determined connected/disconnected time.
  const double determined = q.connected_seconds + q.disconnected_seconds;
  if (q.valid_sample_count >= kWirelessQualityMinValidSamples &&
      determined >= kWirelessQualityMinDeterminedSeconds) {
    q.assessment = assessWirelessStability(wireless, q);
  }
  return q;
}

std::string renderWirelessQualitySummary(const NetworkWirelessInfo &wireless,
                                         std::size_t max_samples) {
  if (wireless.presence != WirelessPresence::Wireless) {
    return "not a wireless interface\n";
  }

  const WirelessQualitySummary q =
      summarizeWirelessQuality(wireless, max_samples);

  std::ostringstream out;

  out << "Wireless connection quality summary:\n";
  if (!q.has_data) {
    out << "  no data (no retained wireless history or connection events)\n";
    return out.str();
  }

  // --- Stability and score ----------------------------------------------------
  out << "  Stability: " << wirelessStabilityName(q.assessment.stability);
  if (q.assessment.score.has_value()) {
    out << " (score " << std::fixed << std::setprecision(0)
        << *q.assessment.score << "/100)";
  }
  out << "\n";

  // --- Signal -----------------------------------------------------------------
  out << "  Signal: ";
  if (q.signal_sample_count == 0) {
    out << "N/A (no data)";
  } else {
    out << "current " << formatWirelessSignal(
               q.current_signal_dbm.has_value()
                   ? std::optional<int>(static_cast<int>(*q.current_signal_dbm))
                   : std::optional<int>{});
    if (q.avg_signal_dbm.has_value()) {
      out << ", avg " << std::fixed << std::setprecision(1) << *q.avg_signal_dbm
          << " dBm";
    }
    if (q.min_signal_dbm.has_value() && q.max_signal_dbm.has_value()) {
      out << ", range [" << std::fixed << std::setprecision(0)
          << static_cast<long long>(*q.min_signal_dbm) << ", "
          << static_cast<long long>(*q.max_signal_dbm) << "]";
    }
    if (q.signal_stddev_dbm.has_value()) {
      out << ", stddev " << std::fixed << std::setprecision(1)
          << *q.signal_stddev_dbm << " dBm";
    }
  }
  out << "\n";

  // --- Bitrate ----------------------------------------------------------------
  out << "  Bitrate: ";
  if (q.bitrate_sample_count == 0) {
    out << "N/A (no data)";
  } else {
    out << formatWirelessBitrate(q.current_bitrate_bps);
    if (q.avg_bitrate_bps.has_value()) {
      out << ", avg " << formatWirelessBitrate(q.avg_bitrate_bps);
    }
    if (q.min_bitrate_bps.has_value() && q.max_bitrate_bps.has_value()) {
      out << ", range [" << formatWirelessBitrate(q.min_bitrate_bps) << ", "
          << formatWirelessBitrate(q.max_bitrate_bps) << "]";
    }
  }
  out << "\n";

  // --- Connectivity durations -------------------------------------------------
  out << "  Connectivity: " << std::fixed << std::setprecision(1)
      << q.connected_seconds << " s connected, " << std::fixed
      << std::setprecision(1) << q.disconnected_seconds
      << " s disconnected, " << std::fixed << std::setprecision(1)
      << q.unobserved_seconds << " s unobserved\n";
  out << "  Longest runs: connected " << std::fixed << std::setprecision(1)
      << q.longest_connected_seconds << " s, disconnected " << std::fixed
      << std::setprecision(1) << q.longest_disconnected_seconds << " s\n";

  // --- Coverage ---------------------------------------------------------------
  out << "  Coverage: " << q.valid_sample_count << " / " << max_samples
      << " valid samples, " << std::fixed << std::setprecision(0)
      << std::llround(q.coverage * 100.0) << "% retained window\n";

  // --- Events -----------------------------------------------------------------
  out << "  Events: " << q.disconnection_count << " disconnections, "
      << q.reconnection_count << " reconnections, " << q.roaming_count
      << " roams, " << q.interface_unavailable_count << " unavailable\n";
  out << "  Gaps: " << q.temporary_gap_count << " invalid ticks\n";

  // --- Factors ----------------------------------------------------------------
  if (!q.assessment.factors.empty()) {
    out << "  Factors:\n";
    for (const auto &f : q.assessment.factors) {
      out << "    " << f.name << ": " << f.detail << " (weight " << std::fixed
          << std::setprecision(0) << std::llround(f.weight * 100.0) << "%)";
      if (!f.value.has_value()) {
        out << " (excluded: insufficient data)";
      }
      out << "\n";
    }
  }

  // --- Last update ------------------------------------------------------------
  out << "  Last update: " << formatTimestamp(q.last_update) << "\n";

  // --- Coverage note ----------------------------------------------------------
  if (q.valid_sample_count < kWirelessQualityMinValidSamples ||
      (q.connected_seconds + q.disconnected_seconds) <
          kWirelessQualityMinDeterminedSeconds) {
    out << "  Coverage is limited; classification reflects available data only.\n";
  }

  return out.str();
}

std::string describeWirelessLine(const NetworkWirelessInfo &wireless) {
  if (wireless.presence != WirelessPresence::Wireless) {
    return "not a wireless interface";
  }

  std::string line;
  switch (wireless.association) {
    case WirelessAssociation::Associated:
      line = "associated (kernel carrier bit set)";
      break;
    case WirelessAssociation::Disconnected:
      line = "disconnected (no carrier)";
      break;
    case WirelessAssociation::Unknown:
      line = "association unknown (no carrier bit exposed)";
      break;
    case WirelessAssociation::Unavailable:
      line = "association unavailable";
      break;
  }

  bool appended = false;
  const std::string signal = formatWirelessSignal(wireless.signal_dbm);
  if (signal != "N/A") {
    line += " \u2014 signal " + signal;
    appended = true;
  }
  const std::string quality =
      formatWirelessLinkQuality(wireless.link_quality, wireless.is_mac80211);
  if (quality != "N/A") {
    line += std::string(appended ? ", " : " \u2014 ") + "link quality " +
            quality;
    appended = true;
  }
  if (wireless.field_state == WirelessFieldState::Stale && appended) {
    line += " (values stale)";
  }
  return line;
}

/// True when the two observations represent the same lifecycle connection
/// state: same identity presence, same carrier-derived association, and — while
/// associated with a reliably reported AP identity — the same access point.
/// Signal/quality/frequency changes alone never constitute a state change.
bool sameWirelessConnectionState(const WirelessObservation &a,
                                 const WirelessObservation &b) {
  if (a.present != b.present) {
    return false;
  }
  if (!a.present) {
    return true;  // both gone: association values are irrelevant
  }
  if (a.association != b.association) {
    return false;
  }
  if (a.association == WirelessAssociation::Associated &&
      a.ap_identity_reliable && b.ap_identity_reliable) {
    return a.ap_fingerprint == b.ap_fingerprint;
  }
  return true;
}

std::optional<WirelessConnectionEventType> classifyWirelessConnectionChange(
    const WirelessObservation &before, const WirelessObservation &after,
    bool has_been_associated) {
  if (!before.present && !after.present) {
    return std::nullopt;
  }
  if (!before.present && after.present) {
    return WirelessConnectionEventType::InterfaceAvailable;
  }
  if (before.present && !after.present) {
    return WirelessConnectionEventType::InterfaceUnavailable;
  }

  if (before.association == after.association) {
    // Only a reliably reported AP handoff is a roam; identical associations
    // with signal/frequency changes (or an unreliable AP identity) never are.
    if (before.association == WirelessAssociation::Associated &&
        before.ap_identity_reliable && after.ap_identity_reliable &&
        before.ap_fingerprint.has_value() && after.ap_fingerprint.has_value() &&
        *before.ap_fingerprint != *after.ap_fingerprint) {
      return WirelessConnectionEventType::Roamed;
    }
    return std::nullopt;
  }

  switch (after.association) {
    case WirelessAssociation::Associated:
      return has_been_associated
                 ? WirelessConnectionEventType::Reconnected
                 : WirelessConnectionEventType::Associated;
    case WirelessAssociation::Disconnected:
      return WirelessConnectionEventType::Disassociated;
    case WirelessAssociation::Unknown:
      return WirelessConnectionEventType::StateUnknown;
    case WirelessAssociation::Unavailable:
      return WirelessConnectionEventType::InterfaceUnavailable;
  }
  return std::nullopt;
}

const char *wirelessConnectionEventTypeName(WirelessConnectionEventType type) {
  switch (type) {
    case WirelessConnectionEventType::Associated:
      return "associated";
    case WirelessConnectionEventType::Disassociated:
      return "disassociated";
    case WirelessConnectionEventType::Reconnected:
      return "reconnected";
    case WirelessConnectionEventType::Roamed:
      return "roamed";
    case WirelessConnectionEventType::InterfaceUnavailable:
      return "interface unavailable";
    case WirelessConnectionEventType::InterfaceAvailable:
      return "interface available";
    case WirelessConnectionEventType::StateUnknown:
      return "state unknown";
  }
  return "unknown";
}

std::uint64_t wirelessApFingerprint(std::string_view identity) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (unsigned char ch : identity) {
    hash ^= static_cast<std::uint64_t>(ch);
    hash *= 1099511628211ULL;
  }
  return hash;
}

/// Builds the committed event for a confirmed change. Pure: consumes only the
/// confirmed "before" observation, the new "after" observation and the type.
WirelessConnectionEvent makeWirelessConnectionEvent(
    WirelessConnectionEventType type, const WirelessObservation &before,
    const WirelessObservation &after, std::chrono::steady_clock::time_point now,
    std::chrono::system_clock::time_point wall_clock,
    const std::string &interface_name) {
  WirelessConnectionEvent event;
  event.timestamp = now;
  event.wall_clock = wall_clock;
  event.type = type;
  event.interface_name = interface_name;

  // A vanished interface presents no association; "unavailable" is the honest
  // reading for both its previous and its new association slot.
  event.previous_association =
      before.present ? before.association : WirelessAssociation::Unavailable;
  event.new_association =
      after.present ? after.association : WirelessAssociation::Unavailable;

  event.previous_signal_dbm = before.signal_dbm;
  event.new_signal_dbm = after.signal_dbm;
  event.previous_frequency_mhz = before.frequency_mhz;
  event.new_frequency_mhz = after.frequency_mhz;
  event.previous_channel = before.channel;
  event.new_channel = after.channel;

  event.ap_identity_reliable = after.ap_identity_reliable;
  event.ap_fingerprint_changed =
      type == WirelessConnectionEventType::Roamed &&
      before.ap_identity_reliable && after.ap_identity_reliable &&
      before.ap_fingerprint.has_value() && after.ap_fingerprint.has_value() &&
      *before.ap_fingerprint != *after.ap_fingerprint;

  switch (type) {
    case WirelessConnectionEventType::InterfaceAvailable:
    case WirelessConnectionEventType::InterfaceUnavailable:
    case WirelessConnectionEventType::Roamed:
      event.confident = true;
      event.source = type == WirelessConnectionEventType::Roamed ? "ap_identity"
                                                                 : "presence";
      break;
    case WirelessConnectionEventType::StateUnknown:
      event.confident = false;
      event.source = "carrier";
      break;
    case WirelessConnectionEventType::Associated:
    case WirelessConnectionEventType::Reconnected:
    case WirelessConnectionEventType::Disassociated:
      event.confident =
          before.association != WirelessAssociation::Unknown &&
          before.association != WirelessAssociation::Unavailable;
      event.source = "carrier";
      break;
  }
  return event;
}

std::optional<WirelessConnectionEvent> advanceWirelessConnectionTracker(
    WirelessConnectionTracker &tracker, const WirelessObservation &observation,
    std::chrono::steady_clock::time_point now,
    std::chrono::system_clock::time_point wall_clock,
    const std::string &interface_name, unsigned debounce_ticks,
    std::chrono::seconds reset_gap) {
  // Suspend / long-gap rebaseline: after a wall-clock jump (suspension) or a
  // long monitoring gap the connection continuity cannot be trusted, so the
  // tracker silently adopts the current observation instead of emitting a
  // burst of spurious events.
  if (tracker.last_observation_wall != std::chrono::system_clock::time_point{}) {
    const auto elapsed = wall_clock - tracker.last_observation_wall;
    if (elapsed > reset_gap) {
      tracker.confirmed = observation;
      tracker.confirmed_valid = true;
      tracker.pending_valid = false;
      tracker.has_been_associated =
          observation.present &&
          observation.association == WirelessAssociation::Associated;
      tracker.last_observation_wall = wall_clock;
      return std::nullopt;
    }
  }

  // First observation absorbed for this record: establish the baseline and the
  // "has ever been associated" fact silently.
  if (!tracker.confirmed_valid) {
    tracker.confirmed = observation;
    tracker.confirmed_valid = true;
    tracker.has_been_associated =
        observation.present &&
        observation.association == WirelessAssociation::Associated;
    tracker.last_observation_wall = wall_clock;
    return std::nullopt;
  }

  const auto commit = [&] {
    const WirelessObservation before = tracker.confirmed;
    const std::optional<WirelessConnectionEventType> type =
        classifyWirelessConnectionChange(before, observation,
                                         tracker.has_been_associated);
    if (observation.present &&
        observation.association == WirelessAssociation::Associated) {
      tracker.has_been_associated = true;
    }
    tracker.confirmed = observation;
    tracker.pending_valid = false;
    tracker.last_observation_wall = wall_clock;
    if (!type.has_value()) {
      return std::optional<WirelessConnectionEvent>{};
    }
    return std::optional<WirelessConnectionEvent>(makeWirelessConnectionEvent(
        *type, before, observation, now, wall_clock, interface_name));
  };

  // A change is announced only after the new state has been observed for
  // `debounce_ticks` consecutive ticks (0 = announce on the first differing
  // tick). `pending_remaining` counts the additional matching observations
  // still required after the current one: the candidate's first observation
  // already counts, so it starts at debounce_ticks - 1 and is decremented on
  // each consecutive matching tick, committing when it reaches 0.
  auto openWindow = [&]() -> std::optional<WirelessConnectionEvent> {
    tracker.pending = observation;
    tracker.pending_valid = true;
    tracker.pending_remaining = debounce_ticks > 0 ? debounce_ticks - 1 : 0;
    tracker.last_observation_wall = wall_clock;
    return tracker.pending_remaining == 0 ? commit()
                                          : std::optional<WirelessConnectionEvent>{};
  };
  auto extendWindow = [&]() -> std::optional<WirelessConnectionEvent> {
    --tracker.pending_remaining;
    tracker.last_observation_wall = wall_clock;
    return tracker.pending_remaining == 0 ? commit()
                                          : std::optional<WirelessConnectionEvent>{};
  };

  if (sameWirelessConnectionState(tracker.confirmed, observation)) {
    tracker.pending_valid = false;  // a transient blip reverted: discard it
    tracker.last_observation_wall = wall_clock;
    return std::nullopt;
  }

  if (tracker.pending_valid &&
      sameWirelessConnectionState(tracker.pending, observation)) {
    return extendWindow();
  }

  // A new, different candidate opens (or restarts) the confirmation window.
  return openWindow();
}

/// Local formatter for an optional signed number with up to one decimal place
/// (used for the dBm / MHz hints in the event description).
std::string formatOptionalDbmLike(const std::optional<double> &value) {
  if (!value.has_value()) {
    return "n/a";
  }
  const double v = *value;
  if (std::trunc(v) == v) {
    return std::to_string(static_cast<long long>(v));
  }
  std::ostringstream out;
  out << std::fixed << std::setprecision(1) << v;
  return out.str();
}

std::string describeWirelessConnectionEvent(const WirelessConnectionEvent &event) {
  std::string line = wirelessConnectionEventTypeName(event.type);
  if (!event.interface_name.empty()) {
    line += "  " + event.interface_name;
  }
  if (event.previous_association != event.new_association) {
    line += "  ";
    line += wirelessAssociationName(event.previous_association);
    line += " -> ";
    line += wirelessAssociationName(event.new_association);
  } else if (event.type == WirelessConnectionEventType::Roamed) {
    line += "  access point changed";
  }

  std::vector<std::string> hints;
  if (event.previous_signal_dbm.has_value() || event.new_signal_dbm.has_value()) {
    hints.push_back("signal " + formatOptionalDbmLike(event.previous_signal_dbm) +
                    " -> " + formatOptionalDbmLike(event.new_signal_dbm) + " dBm");
  }
  if (event.previous_frequency_mhz.has_value() ||
      event.new_frequency_mhz.has_value()) {
    hints.push_back("freq " + formatOptionalDbmLike(event.previous_frequency_mhz) +
                    " -> " + formatOptionalDbmLike(event.new_frequency_mhz) +
                    " MHz");
  }
  if (event.previous_channel.has_value() || event.new_channel.has_value()) {
    hints.push_back(
        "channel " + formatWirelessChannel(event.previous_channel) + " -> " +
        formatWirelessChannel(event.new_channel));
  }
  for (const std::string &hint : hints) {
    line += "  " + hint;
  }
  if (!event.confident) {
    line += "  (low confidence)";
  }
  return line;
}

NetworkWirelessMonitor::NetworkWirelessMonitor(
    AlertManager &alerts, std::filesystem::path root,
    std::chrono::milliseconds reread_interval, std::size_t history_max_samples)
    : alerts_(alerts),
      root_(std::move(root)),
      reread_interval_(reread_interval),
      history_max_samples_(history_max_samples) {}

const NetworkWirelessInfo *NetworkWirelessMonitor::tracked(
    const std::string &identity) const {
  const auto it = tracked_.find(identity);
  return it == tracked_.end() ? nullptr : &it->second;
}

const ResourceHistory<WirelessHistorySample> *
NetworkWirelessMonitor::wirelessHistory(const std::string &identity) const {
  const auto it = tracked_.find(identity);
  return it == tracked_.end() ? nullptr : &it->second.history;
}

void NetworkWirelessMonitor::recordConnectionEvent(
    const NetworkWirelessInfo &wireless,
    const WirelessConnectionEvent &event) {
  // Roaming is by design never notified (and, being Normal non-recovery, never
  // transitions the per-source severity), so only carrier/presence changes
  // reach the alert/notification path. The NotificationManager still applies
  // the device-wide settings and its own per-source cooldown.
  if (event.type == WirelessConnectionEventType::Roamed) {
    return;
  }
  AlertSeverity severity = AlertSeverity::Normal;
  bool is_recovery = false;
  switch (event.type) {
    case WirelessConnectionEventType::Associated:
    case WirelessConnectionEventType::Reconnected:
    case WirelessConnectionEventType::InterfaceAvailable:
      is_recovery = true;
      break;
    case WirelessConnectionEventType::Disassociated:
    case WirelessConnectionEventType::InterfaceUnavailable:
    case WirelessConnectionEventType::StateUnknown:
      severity = AlertSeverity::Warning;
      break;
    case WirelessConnectionEventType::Roamed:
      return;
  }
  AlertEvent alert;
  alert.type = AlertType::WirelessConnectionChanged;
  alert.severity = severity;
  alert.source = wireless.identity;
  alert.value = 0.0;
  alert.threshold = 0.0;
  alert.is_recovery = is_recovery;
  alert.message = describeWirelessConnectionEvent(event);
  alert.timestamp = event.wall_clock;
  alerts_.recordRuleEvent(alert.type, alert.source, alert.severity, alert.value,
                          alert.threshold, alert.message);
  if (event_sink_ != nullptr) {
    event_sink_(alert);
  }
}

void NetworkWirelessMonitor::update(const NetworkInterfaceSnapshot &snapshot) {
  const auto now = std::chrono::steady_clock::now();

  // A temporarily unreadable /sys/class/net is the one place the whole snapshot
  // is untrusted at once: preserve every tracked identity's last valid values,
  // mark them stale, and never probe (probing reads the same unreadable tree).
  if (!snapshot.sysfs_readable) {
    for (auto &kv : tracked_) {
      NetworkWirelessInfo &w = kv.second;
      if (w.field_state == WirelessFieldState::Available ||
          w.field_state == WirelessFieldState::Stale) {
        w.field_state = WirelessFieldState::Stale;
      }
      if (w.phy_state == WirelessFieldState::Available) {
        w.phy_state = WirelessFieldState::Stale;
      }
    }
    return;
  }

  std::vector<std::string> present;
  present.reserve(snapshot.interfaces.size());

  for (const NetworkInterfaceInfo &info : snapshot.interfaces) {
    const std::string identity = info.identity();
    present.push_back(identity);

    auto it = tracked_.find(identity);
    NetworkWirelessInfo base;
    if (it == tracked_.end()) {
      base.identity = identity;
      base.first_seen = now;
      base.history = ResourceHistory<WirelessHistorySample>(history_max_samples_);
      base.connection_events =
          ResourceHistory<WirelessConnectionEvent>(kMaxWirelessConnectionEvents);
    } else {
      base = it->second;  // preserves PHY fields and history across renames
    }

    // Static PHY metadata is cached: re-probe only on a new identity, a rename,
    // a previously failed probe, or after the reread interval elapsed.
    bool need_probe = it == tracked_.end();
    if (!need_probe && it->second.name != info.name) {
      need_probe = true;
    }
    if (!need_probe && !it->second.probe_ok) {
      need_probe = true;
    }
    if (!need_probe && now - it->second.last_probe >= reread_interval_) {
      need_probe = true;
    }

    WirelessProbe probe;
    if (need_probe) {
      probe = readWirelessProbe(root_, info.name);
    } else {
      // Reconstruct the cached probe view from the retained record so judge()
      // keeps using the same decisions between re-probes without re-reading
      // sysfs on the render path.
      probe.phy80211_present = base.is_mac80211;
      probe.wireless_dir_present = info.wireless.present;
      probe.metadata_available =
          base.phy_state == WirelessFieldState::Available;
      probe.probe_ok = base.probe_ok;
      probe.access_denied = !base.probe_ok;
      probe.phy_name = base.phy_name;
      probe.phy_index = base.phy_index;
    }

    // A native fallback for the dynamic signal/noise values: /proc/net/wireless
    // is consulted for wireless-capable interfaces whose snapshot carried no
    // WIRELESS_EXT values this tick. The gate is "no snapshot values" rather
    // than "no /wireless directory": many mac80211 drivers keep the directory
    // but never fill its link/level/noise files, publishing the values only
    // through the /proc table. The table is never read on the render path and
    // only ever re-read per tick for interfaces that actually need it.
    const bool wireless_capable =
        probe.phy80211_present || probe.wireless_dir_present ||
        info.wireless.present ||
        (info.link.link_type.has_value() &&
         arphrdIsIeee80211(*info.link.link_type));
    const bool snapshot_has_we_values =
        info.wireless.link.has_value() || info.wireless.level.has_value() ||
        info.wireless.noise.has_value();
    ProcWirelessStats proc;
    if (wireless_capable && !snapshot_has_we_values) {
      proc = readProcNetWirelessStats(root_ / "proc" / "net" / "wireless",
                                      info.name);
    }

    NetworkWirelessInfo next = judgeWireless(info, probe, proc, base);
    next.probe_ok = probe.probe_ok;
    if (need_probe) {
      next.last_probe = now;
    } else {
      next.last_probe = base.last_probe;
    }

    // One self-contained wireless sample per tick (never more, never on the
    // render path). A sample is recorded for EVERY present wireless interface
    // regardless of value availability so association, coverage and honest
    // gaps are tracked; unavailable metrics stay nullopt (never a fabricated
    // zero), and an unavailable/stale tick is marked invalid — the preserved
    // display values from judgeWireless() are NOT copied into the history.
      if (next.presence == WirelessPresence::Wireless) {
        WirelessHistorySample sample;
        sample.timestamp = now;
        sample.association = next.association;
        sample.is_mac80211 = next.is_mac80211;
        if (next.field_state == WirelessFieldState::Available) {
          sample.valid = true;
          if (next.signal_dbm.has_value()) {
            sample.signal_dbm = static_cast<double>(*next.signal_dbm);
          }
          if (next.link_quality.has_value()) {
            sample.link_quality = *next.link_quality;
          }
          // Bitrate / frequency / channel are not exposed by the approved
          // native sources (sysfs, /proc/net/wireless) and therefore stay
          // unavailable here; the sample carries the fields so the
          // export/summary/test machinery is complete and deterministic.
        }

        next.history.addSample(std::move(sample));
        next.last_sample_wall = snapshot.refreshed_at;

        // Connection-event state machine (Step 51): feeds one observation per
        // tick. A confirmed (debounced, deduplicated) change becomes a bounded
        // connection event and — for carrier/presence changes — an alert
        // record plus an optional desktop notification.
        WirelessObservation observation = makeWirelessObservation(next, true);
        if (std::optional<WirelessConnectionEvent> event =
                advanceWirelessConnectionTracker(
                    next.connection_tracker, observation, now,
                    snapshot.refreshed_at, next.name);
            event.has_value()) {
          next.connection_events.addSample(*event);
          recordConnectionEvent(next, *event);
        }
      } else {
        next.history.clear();
        next.connection_events.clear();
        next.connection_tracker = WirelessConnectionTracker{};
        next.last_sample_wall = {};
        // The identity is no longer managed by this monitor; a lingering
        // wireless alert (e.g. from a recent disconnect) must not stay active.
        alerts_.clearSubject(AlertType::WirelessConnectionChanged, identity);
      }

      next.last_read = snapshot.refreshed_at;
      next.present = true;
      tracked_[identity] = std::move(next);
    }

    // Identities that are no longer discovered are kept, marked gone, and fed
    // to the connection-event state machine (debounced: the interface must stay
    // gone for the confirmation window before an "interface unavailable" event
    // is announced, and a reappearance rebuilds an "interface available" event).
    // A recreated interface (new ifindex) naturally starts a fresh record
    // instead of inheriting an old one.
    for (auto &kv : tracked_) {
      if (std::find(present.begin(), present.end(), kv.first) != present.end()) {
        continue;
      }
      if (!kv.second.present) {
        continue;
      }
      kv.second.present = false;
      const WirelessObservation gone = makeGoneObservation();
      if (std::optional<WirelessConnectionEvent> event =
              advanceWirelessConnectionTracker(
                  kv.second.connection_tracker, gone, now,
                  snapshot.refreshed_at, kv.second.name);
          event.has_value()) {
        kv.second.connection_events.addSample(*event);
        recordConnectionEvent(kv.second, *event);
      }
    }

    evictOverflow();
  }

void NetworkWirelessMonitor::setHistoryMaxSamples(std::size_t max_samples) {
  history_max_samples_ = max_samples;
  for (auto &kv : tracked_) {
    kv.second.history = ResourceHistory<WirelessHistorySample>(max_samples);
    kv.second.connection_events =
        ResourceHistory<WirelessConnectionEvent>(kMaxWirelessConnectionEvents);
  }
}

void NetworkWirelessMonitor::evictOverflow() {
  // Bound the number of retained identities: evict the oldest gone entries;
  // live entries are never evicted.
  if (tracked_.size() <= kMaxTrackedWirelessInterfaces) {
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
  std::size_t excess = tracked_.size() - kMaxTrackedWirelessInterfaces;
  for (const std::string &identity : gone) {
    if (excess == 0) {
      break;
    }
    // A gone entry with an active warning (e.g. interface unplugged) must not
    // leave a stale active alert behind once its record is evicted.
    alerts_.clearSubject(AlertType::WirelessConnectionChanged, identity);
    tracked_.erase(identity);
    --excess;
  }
}

void NetworkWirelessMonitor::reset() { tracked_.clear(); }

}  // namespace atm