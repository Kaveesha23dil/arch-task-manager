#include "network_link_metrics.hpp"

#include <algorithm>
#include <utility>

namespace atm {

namespace {

/// True when the sysfs "speed" file carried a usable positive number this read
/// (the kernel reports -1 for "unknown speed"). A value can never be zero for
/// a negotiated link, so 0 is treated as "no fresh speed" rather than trusted.
bool isFreshSpeed(const std::optional<int> &mbps) {
  return mbps.has_value() && *mbps > 0;
}

}  // namespace

const char *networkDuplexModeName(NetworkDuplexMode mode) {
  switch (mode) {
    case NetworkDuplexMode::Unknown:
      return "unknown";
    case NetworkDuplexMode::Half:
      return "half";
    case NetworkDuplexMode::Full:
      return "full";
  }
  return "unknown";
}

const char *networkSpeedStateName(NetworkSpeedState state) {
  switch (state) {
    case NetworkSpeedState::Unknown:
      return "unknown";
    case NetworkSpeedState::Valid:
      return "valid";
    case NetworkSpeedState::Stale:
      return "stale";
    case NetworkSpeedState::Unavailable:
      return "unavailable";
  }
  return "unknown";
}

const char *networkDuplexStateName(NetworkDuplexState state) {
  switch (state) {
    case NetworkDuplexState::Unknown:
      return "unknown";
    case NetworkDuplexState::Valid:
      return "valid";
    case NetworkDuplexState::Stale:
      return "stale";
    case NetworkDuplexState::Unavailable:
      return "unavailable";
  }
  return "unknown";
}

NetworkDuplexMode parseNetworkDuplex(const std::optional<std::string> &duplex) {
  if (!duplex.has_value()) {
    return NetworkDuplexMode::Unknown;
  }
  if (*duplex == "full") {
    return NetworkDuplexMode::Full;
  }
  if (*duplex == "half") {
    return NetworkDuplexMode::Half;
  }
  return NetworkDuplexMode::Unknown;
}

NetworkLinkMetrics updateNetworkLinkMetrics(
    const NetworkLinkMetrics &previous, const NetworkInterfaceInfo &info,
    bool link_active, std::chrono::system_clock::time_point refreshed_at) {
  NetworkLinkMetrics m = previous;  // keep identity, first_seen, retained values
  m.name = info.name;
  m.present = true;
  m.physical = isPhysicalNetworkLink(info.type);
  m.link_active = link_active;

  // Virtual/tunnel/bond/loopback devices negotiate no link speed or duplex the
  // kernel exposes through these sysfs files. Report them unavailable instead
  // of echoing whatever the driver happens to leave in the file, and never
  // claim a stale value from an earlier physical incarnation.
  if (!m.physical) {
    m.speed_state = NetworkSpeedState::Unavailable;
    m.speed_mbps.reset();
    m.duplex_state = NetworkDuplexState::Unavailable;
    m.duplex = NetworkDuplexMode::Unknown;
    return m;
  }

  const NetworkDuplexMode mode = parseNetworkDuplex(info.link.duplex);

  // Speed: a fresh positive reading is trusted; otherwise the last known value
  // is preserved and marked stale for display. Nothing retained means the
  // interface has never reported a usable speed.
  if (isFreshSpeed(info.link.speed_mbps)) {
    m.speed_mbps = info.link.speed_mbps;
    m.speed_state =
        link_active ? NetworkSpeedState::Valid : NetworkSpeedState::Stale;
    m.last_update = refreshed_at;
  } else if (m.speed_mbps.has_value()) {
    m.speed_state = NetworkSpeedState::Stale;
  } else {
    m.speed_mbps.reset();
    m.speed_state = NetworkSpeedState::Unavailable;
  }

  // Duplex mirrors the speed handling.
  if (mode != NetworkDuplexMode::Unknown) {
    m.duplex = mode;
    m.duplex_state =
        link_active ? NetworkDuplexState::Valid : NetworkDuplexState::Stale;
    m.last_update = refreshed_at;
  } else if (m.duplex != NetworkDuplexMode::Unknown) {
    m.duplex_state = NetworkDuplexState::Stale;
  } else {
    m.duplex = NetworkDuplexMode::Unknown;
    m.duplex_state = NetworkDuplexState::Unavailable;
  }

  return m;
}

const NetworkLinkMetrics *NetworkLinkMetricsMonitor::tracked(
    const std::string &identity) const {
  const auto it = tracked_.find(identity);
  return it == tracked_.end() ? nullptr : &it->second;
}

void NetworkLinkMetricsMonitor::update(
    const NetworkInterfaceSnapshot &snapshot) {
  const auto now = std::chrono::steady_clock::now();

  std::vector<std::string> present;
  present.reserve(snapshot.interfaces.size());

  for (const NetworkInterfaceInfo &info : snapshot.interfaces) {
    const std::string identity = info.identity();
    present.push_back(identity);

    const bool link_known = info.link.operstate.has_value() ||
                            info.link.carrier.has_value() ||
                            info.link.flags.has_value();
    const bool active =
        linkAvailability(link_known, parseOperState(info.link.operstate),
                         parseCarrierState(info.link.carrier),
                         parseAdminState(info.link.flags, info.link.admin_up)) ==
        NetworkLinkAvailability::Connected;

    auto it = tracked_.find(identity);
    NetworkLinkMetrics base;
    if (it == tracked_.end()) {
      base.identity = identity;
      base.first_seen = now;
    } else {
      base = it->second;  // preserves retained speed/duplex across renames
    }
    NetworkLinkMetrics next =
        updateNetworkLinkMetrics(base, info, active, snapshot.refreshed_at);
    tracked_[identity] = std::move(next);
  }

  // Identities that are no longer discovered are kept but marked absent so a
  // temporary removal is not misreported and a recreated interface (new
  // ifindex) naturally starts a fresh record instead of inheriting an old one.
  for (auto &kv : tracked_) {
    if (std::find(present.begin(), present.end(), kv.first) == present.end()) {
      kv.second.present = false;
    }
  }

  evictOverflow();
}

void NetworkLinkMetricsMonitor::evictOverflow() {
  // Bound the number of retained identities: evict the oldest gone entries;
  // live entries are never evicted.
  if (tracked_.size() <= kMaxTrackedLinkMetricsInterfaces) {
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
  std::size_t excess = tracked_.size() - kMaxTrackedLinkMetricsInterfaces;
  for (const std::string &identity : gone) {
    if (excess == 0) {
      break;
    }
    tracked_.erase(identity);
    --excess;
  }
}

void NetworkLinkMetricsMonitor::reset() { tracked_.clear(); }

}  // namespace atm