#include "network_wireless.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fstream>
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

NetworkWirelessMonitor::NetworkWirelessMonitor(
    std::filesystem::path root, std::chrono::milliseconds reread_interval,
    std::size_t history_max_samples)
    : root_(std::move(root)),
      reread_interval_(reread_interval),
      history_max_samples_(history_max_samples) {}

const NetworkWirelessInfo *NetworkWirelessMonitor::tracked(
    const std::string &identity) const {
  const auto it = tracked_.find(identity);
  return it == tracked_.end() ? nullptr : &it->second;
}

const ResourceHistory<TimedSample> *NetworkWirelessMonitor::signalHistory(
    const std::string &identity) const {
  const auto it = tracked_.find(identity);
  return it == tracked_.end() ? nullptr : &it->second.signal_history;
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
      base.signal_history = ResourceHistory<TimedSample>(history_max_samples_);
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

    // One signal sample per tick (never more, never on the render path). A
    // stale/gap tick is simply not sampled, keeping the ring honest.
    if (next.presence == WirelessPresence::Wireless &&
        next.field_state == WirelessFieldState::Available &&
        next.signal_dbm.has_value()) {
      next.signal_history.addSample(
          TimedSample{now, static_cast<double>(*next.signal_dbm)});
    } else if (next.presence != WirelessPresence::Wireless) {
      next.signal_history.clear();
    }

    next.last_read = snapshot.refreshed_at;
    next.present = true;
    tracked_[identity] = std::move(next);
  }

  // Identities that are no longer discovered are kept but marked gone so a
  // temporary removal is not misreported and a recreated interface (new
  // ifindex) naturally starts a fresh record instead of inheriting an old one.
  for (auto &kv : tracked_) {
    if (std::find(present.begin(), present.end(), kv.first) == present.end()) {
      kv.second.present = false;
    }
  }

  evictOverflow();
}

void NetworkWirelessMonitor::setHistoryMaxSamples(std::size_t max_samples) {
  history_max_samples_ = max_samples;
  for (auto &kv : tracked_) {
    kv.second.signal_history = ResourceHistory<TimedSample>(max_samples);
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
    tracked_.erase(identity);
    --excess;
  }
}

void NetworkWirelessMonitor::reset() { tracked_.clear(); }

}  // namespace atm