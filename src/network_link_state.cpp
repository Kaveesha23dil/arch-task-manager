#include "network_link_state.hpp"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

namespace atm {

const char *networkOperStateName(NetworkOperState state) {
  switch (state) {
    case NetworkOperState::Unknown:        return "unknown";
    case NetworkOperState::Up:             return "up";
    case NetworkOperState::Down:           return "down";
    case NetworkOperState::Dormant:        return "dormant";
    case NetworkOperState::LowerLayerDown: return "lowerlayerdown";
    case NetworkOperState::Testing:        return "testing";
    case NetworkOperState::NotPresent:     return "notpresent";
    case NetworkOperState::Unavailable:    return "unavailable";
  }
  return "unknown";
}

const char *networkCarrierStateName(NetworkCarrierState state) {
  switch (state) {
    case NetworkCarrierState::Unknown:   return "unknown";
    case NetworkCarrierState::NoCarrier: return "no carrier";
    case NetworkCarrierState::Carrier:   return "yes";
  }
  return "unknown";
}

const char *networkAdminStateName(NetworkAdminState state) {
  switch (state) {
    case NetworkAdminState::Unknown:   return "unknown";
    case NetworkAdminState::AdminDown: return "disabled";
    case NetworkAdminState::AdminUp:   return "enabled";
  }
  return "unknown";
}

const char *networkLinkAvailabilityName(NetworkLinkAvailability availability) {
  switch (availability) {
    case NetworkLinkAvailability::Unknown:     return "Unknown";
    case NetworkLinkAvailability::Connected:   return "Connected";
    case NetworkLinkAvailability::Down:        return "Down";
    case NetworkLinkAvailability::Unavailable: return "Unavailable";
  }
  return "Unknown";
}

const char *networkLinkEventTypeName(NetworkLinkEventType type) {
  switch (type) {
    case NetworkLinkEventType::StateChanged: return "StateChanged";
    case NetworkLinkEventType::Appeared:     return "Appeared";
    case NetworkLinkEventType::Disappeared:  return "Disappeared";
    case NetworkLinkEventType::Reappeared:   return "Reappeared";
  }
  return "StateChanged";
}

bool isPhysicalNetworkLink(NetworkInterfaceType type) {
  switch (type) {
    case NetworkInterfaceType::Ethernet:
    case NetworkInterfaceType::Wifi:
    case NetworkInterfaceType::InfiniBand:
      return true;
    case NetworkInterfaceType::Loopback:
    case NetworkInterfaceType::Tunnel:
    case NetworkInterfaceType::P2P:
    case NetworkInterfaceType::Bridge:
    case NetworkInterfaceType::Bond:
    case NetworkInterfaceType::Virtual:
    case NetworkInterfaceType::Unknown:
      return false;
  }
  return false;
}

NetworkOperState parseOperState(const std::optional<std::string> &operstate) {
  if (!operstate.has_value()) {
    return NetworkOperState::Unknown;
  }
  const std::string &value = *operstate;
  if (value == "up")            return NetworkOperState::Up;
  if (value == "down")          return NetworkOperState::Down;
  if (value == "dormant")       return NetworkOperState::Dormant;
  if (value == "lowerlayerdown") return NetworkOperState::LowerLayerDown;
  if (value == "testing")       return NetworkOperState::Testing;
  if (value == "notpresent")    return NetworkOperState::NotPresent;
  return NetworkOperState::Unknown;  // "unknown" and anything unrecognised
}

NetworkCarrierState parseCarrierState(const std::optional<int> &carrier) {
  if (!carrier.has_value()) {
    return NetworkCarrierState::Unknown;
  }
  return *carrier > 0 ? NetworkCarrierState::Carrier
                      : NetworkCarrierState::NoCarrier;
}

NetworkAdminState parseAdminState(const std::optional<unsigned> &flags,
                                  bool admin_up) {
  if (!flags.has_value()) {
    return NetworkAdminState::Unknown;
  }
  return admin_up ? NetworkAdminState::AdminUp : NetworkAdminState::AdminDown;
}

NetworkLinkAvailability linkAvailability(bool link_known, NetworkOperState oper,
                                         NetworkCarrierState carrier,
                                         NetworkAdminState admin) {
  if (!link_known) {
    return NetworkLinkAvailability::Unavailable;
  }
  if (oper == NetworkOperState::Up) {
    return NetworkLinkAvailability::Connected;
  }
  if (oper == NetworkOperState::NotPresent) {
    return NetworkLinkAvailability::Unavailable;
  }
  // A driver that reports carrier (but no usable operstate) signals a live
  // link provided the interface is not administratively disabled.
  if (oper == NetworkOperState::Unknown &&
      carrier == NetworkCarrierState::Carrier &&
      admin != NetworkAdminState::AdminDown) {
    return NetworkLinkAvailability::Connected;
  }
  // Present with metadata but no reported state yet — not yet sampled.
  if (oper == NetworkOperState::Unknown &&
      carrier == NetworkCarrierState::Unknown &&
      admin == NetworkAdminState::Unknown) {
    return NetworkLinkAvailability::Unknown;
  }
  return NetworkLinkAvailability::Down;
}

NetworkLinkAvailability TrackedInterface::availability() const {
  return linkAvailability(link_known, oper, carrier, admin);
}

namespace {

/// One-line summary of a state transition, e.g.
/// "operstate up -> down, carrier lost". Empty when nothing changed.
std::string describeTransition(NetworkOperState previous_oper,
                               NetworkOperState new_oper,
                               NetworkCarrierState previous_carrier,
                               NetworkCarrierState new_carrier,
                               NetworkAdminState previous_admin,
                               NetworkAdminState new_admin) {
  std::vector<std::string> parts;
  if (previous_oper != new_oper) {
    parts.push_back(std::string(networkOperStateName(previous_oper)) + " -> " +
                    networkOperStateName(new_oper));
  }
  if (previous_carrier == NetworkCarrierState::Carrier &&
      new_carrier != NetworkCarrierState::Carrier) {
    parts.push_back("carrier lost");
  } else if (previous_carrier != NetworkCarrierState::Carrier &&
             new_carrier == NetworkCarrierState::Carrier) {
    parts.push_back("carrier present");
  }
  if (previous_admin == NetworkAdminState::AdminUp &&
      new_admin == NetworkAdminState::AdminDown) {
    parts.push_back("administratively disabled");
  } else if (previous_admin == NetworkAdminState::AdminDown &&
             new_admin == NetworkAdminState::AdminUp) {
    parts.push_back("administratively enabled");
  }
  std::ostringstream out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) {
      out << ", ";
    }
    out << parts[i];
  }
  return out.str();
}

}  // namespace

NetworkLinkStateMonitor::NetworkLinkStateMonitor(AlertManager &alerts,
                                                 std::size_t max_history)
    : alerts_(alerts),
      max_history_(std::max<std::size_t>(max_history, 1)),
      history_(ResourceHistory<NetworkLinkStateEvent>(max_history_)) {}

const TrackedInterface *NetworkLinkStateMonitor::tracked(
    const std::string &identity) const {
  const auto it = tracked_.find(identity);
  return it == tracked_.end() ? nullptr : &it->second;
}

void NetworkLinkStateMonitor::pushEvent(const NetworkLinkStateEvent &event) {
  history_.addSample(event);
}

void NetworkLinkStateMonitor::recordAlert(const std::string &identity,
                                          AlertSeverity severity,
                                          bool is_recovery,
                                          const std::string &message) {
  AlertEvent event;
  event.type = AlertType::LinkStateChanged;
  event.severity = severity;
  event.source = identity;
  event.value = 0.0;
  event.threshold = 0.0;
  event.is_recovery = is_recovery;
  event.message = message;
  event.timestamp = std::chrono::system_clock::now();
  alerts_.recordRuleEvent(event.type, event.source, event.severity, event.value,
                          event.threshold, event.message);
  if (event_sink_ != nullptr) {
    event_sink_(event);
  }
}

void NetworkLinkStateMonitor::update(
    const NetworkInterfaceSnapshot &snapshot) {
  const auto now = std::chrono::steady_clock::now();
  const auto now_system = std::chrono::system_clock::now();

  std::vector<std::string> present;
  present.reserve(snapshot.interfaces.size());

  for (const NetworkInterfaceInfo &info : snapshot.interfaces) {
    const std::string identity = info.identity();
    present.push_back(identity);

    const NetworkOperState oper = parseOperState(info.link.operstate);
    const NetworkCarrierState carrier = parseCarrierState(info.link.carrier);
    const NetworkAdminState admin =
        parseAdminState(info.link.flags, info.link.admin_up);
    const bool link_known = info.link.operstate.has_value() ||
                            info.link.carrier.has_value() ||
                            info.link.flags.has_value();

    // --- New identity: record discovery, store the baseline silently.
    auto it = tracked_.find(identity);
    if (it == tracked_.end()) {
      TrackedInterface tracked;
      tracked.identity = identity;
      tracked.name = info.name;
      tracked.oper = oper;
      tracked.carrier = carrier;
      tracked.admin = admin;
      tracked.has_valid_state = link_known;
      tracked.link_known = link_known;
      tracked.present = true;
      tracked.first_seen = now;
      tracked.last_change_steady = now;
      tracked.last_change_system = now_system;

      NetworkLinkStateEvent event;
      event.type = NetworkLinkEventType::Appeared;
      event.identity = identity;
      event.interface_name = info.name;
      event.new_oper = oper;
      event.new_carrier = carrier;
      event.new_admin = admin;
      event.reason = link_known ? "interface discovered"
                                : "interface discovered (link state not yet readable)";
      event.timestamp = now_system;
      pushEvent(event);

      tracked_.emplace(identity, std::move(tracked));
      continue;
    }

    TrackedInterface &t = it->second;
    const bool was_gone = !t.present;
    t.present = true;
    t.link_known = link_known;
    t.name = info.name;

    // --- Reappeared after removal: reset baseline and treat as recovery.
    if (was_gone) {
      const NetworkLinkStateEvent event = [&] {
        NetworkLinkStateEvent e;
        e.type = NetworkLinkEventType::Reappeared;
        e.identity = identity;
        e.interface_name = info.name;
        e.previous_oper = t.oper;
        e.new_oper = oper;
        e.previous_carrier = t.carrier;
        e.new_carrier = carrier;
        e.previous_admin = t.admin;
        e.new_admin = admin;
        e.is_recovery = true;
        e.reason = "interface available again";
        e.timestamp = now_system;
        return e;
      }();

      t.oper = oper;
      t.carrier = carrier;
      t.admin = admin;
      t.has_valid_state = link_known;
      t.last_change_steady = now;
      t.last_change_system = now_system;
      ++t.transition_count;

      pushEvent(event);
      recordAlert(identity, AlertSeverity::Normal, true,
                  "Interface " + info.name + " is available again");
      continue;
    }

    // --- Temporary read failure: keep the last valid state, mark the
    // --- interface as unavailable for display only. No event, no alert.
    if (!link_known) {
      continue;
    }

    // --- First readable sample becomes the baseline (the Appeared event
    // --- already documented the discovery); never alert on initial state.
    if (!t.has_valid_state) {
      t.oper = oper;
      t.carrier = carrier;
      t.admin = admin;
      t.has_valid_state = true;
      t.last_change_steady = now;
      t.last_change_system = now_system;
      continue;
    }

    const bool oper_changed = t.oper != oper;
    const bool carrier_changed = t.carrier != carrier;
    const bool admin_changed = t.admin != admin;
    if (!oper_changed && !carrier_changed && !admin_changed) {
      continue;
    }

    const NetworkOperState previous_oper = t.oper;
    const NetworkCarrierState previous_carrier = t.carrier;
    const NetworkAdminState previous_admin = t.admin;

    const bool was_connected =
        linkAvailability(true, previous_oper, previous_carrier,
                         previous_admin) == NetworkLinkAvailability::Connected;
    const bool is_connected =
        linkAvailability(true, oper, carrier, admin) ==
        NetworkLinkAvailability::Connected;

    const bool lost =
        (was_connected && !is_connected) ||
        (previous_carrier == NetworkCarrierState::Carrier &&
         carrier != NetworkCarrierState::Carrier) ||
        (previous_admin == NetworkAdminState::AdminUp &&
         admin != NetworkAdminState::AdminUp);
    const bool gained =
        (!was_connected && is_connected) ||
        (previous_carrier != NetworkCarrierState::Carrier &&
         carrier == NetworkCarrierState::Carrier);

    t.oper = oper;
    t.carrier = carrier;
    t.admin = admin;
    t.has_valid_state = true;
    t.last_change_steady = now;
    t.last_change_system = now_system;
    ++t.transition_count;

    NetworkLinkStateEvent event;
    event.type = NetworkLinkEventType::StateChanged;
    event.identity = identity;
    event.interface_name = info.name;
    event.previous_oper = previous_oper;
    event.new_oper = oper;
    event.previous_carrier = previous_carrier;
    event.new_carrier = carrier;
    event.previous_admin = previous_admin;
    event.new_admin = admin;
    event.is_recovery = gained && !lost;
    event.transition_count = t.transition_count;
    event.reason = describeTransition(previous_oper, oper, previous_carrier,
                                      carrier, previous_admin, admin);
    event.timestamp = now_system;
    pushEvent(event);

    if (lost && !gained) {
      recordAlert(identity, AlertSeverity::Warning, false,
                  "Interface " + info.name + " link lost: " + event.reason);
    } else if (gained && !lost) {
      recordAlert(identity, AlertSeverity::Normal, true,
                  "Interface " + info.name + " is connected again: " +
                      event.reason);
    }
  }

  // --- Disappearance detection: an identity no longer present this tick is
  // --- marked gone. The entry is kept (bounded) so a re-plug of the same
  // --- slot is detected as a recovery rather than a fresh discovery.
  if (tracked_.size() > present.size()) {
    for (auto &kv : tracked_) {
      TrackedInterface &t = kv.second;
      if (std::find(present.begin(), present.end(), kv.first) !=
          present.end() || !t.present) {
        continue;
      }
      t.present = false;
      t.last_change_steady = now;
      t.last_change_system = now_system;
      ++t.transition_count;
      alerts_.clearSubject(AlertType::LinkStateChanged, kv.first);

      NetworkLinkStateEvent event;
      event.type = NetworkLinkEventType::Disappeared;
      event.identity = kv.first;
      event.interface_name = t.name;
      event.previous_oper = t.oper;
      event.new_oper = t.oper;
      event.previous_carrier = t.carrier;
      event.new_carrier = t.carrier;
      event.previous_admin = t.admin;
      event.new_admin = t.admin;
      event.reason = "interface removed";
      event.transition_count = t.transition_count;
      event.timestamp = now_system;
      pushEvent(event);
    }
  }

  // --- Bound the number of retained identities: evict the oldest gone
  // --- entries; live entries are never evicted.
  if (tracked_.size() > kMaxTrackedLinkStateInterfaces) {
    std::vector<std::string> gone;
    gone.reserve(tracked_.size());
    for (const auto &kv : tracked_) {
      if (!kv.second.present) {
        gone.push_back(kv.first);
      }
    }
    std::sort(gone.begin(), gone.end(),
              [&](const std::string &a, const std::string &b) {
                return tracked_.at(a).last_change_steady <
                       tracked_.at(b).last_change_steady;
              });
    std::size_t excess = tracked_.size() - kMaxTrackedLinkStateInterfaces;
    for (const std::string &identity : gone) {
      if (excess == 0) {
        break;
      }
      tracked_.erase(identity);
      --excess;
    }
  }

  recount(snapshot);
}

void NetworkLinkStateMonitor::recount(const NetworkInterfaceSnapshot &snapshot) {
  NetworkLinkStateCounts counts;
  for (const NetworkInterfaceInfo &info : snapshot.interfaces) {
    if (!isPhysicalNetworkLink(info.type)) {
      continue;
    }
    ++counts.physical_total;
    const TrackedInterface *t = tracked(info.identity());
    if (t == nullptr || !t->present) {
      continue;
    }
    const NetworkLinkAvailability availability = t->availability();
    if (availability == NetworkLinkAvailability::Connected) {
      ++counts.connected;
    } else if (availability == NetworkLinkAvailability::Down) {
      ++counts.down;
    } else if (availability == NetworkLinkAvailability::Unavailable) {
      ++counts.unavailable;
    }
    if (t->carrier == NetworkCarrierState::Carrier) {
      ++counts.with_carrier;
    }
  }
  counts_ = counts;
}

void NetworkLinkStateMonitor::reset() {
  tracked_.clear();
  history_.clear();
  counts_ = NetworkLinkStateCounts{};
}

}  // namespace atm