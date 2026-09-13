#include "network_interface_hardware.hpp"

#include <algorithm>
#include <fstream>
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
std::optional<std::string> readSysfsField(const std::filesystem::path &dir,
                                          const std::string &field,
                                          bool &denied) {
  denied = false;
  const std::filesystem::path file = dir / field;
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

}  // namespace

const char *networkHardwareStateName(NetworkHardwareState state) {
  switch (state) {
    case NetworkHardwareState::Unknown:
      return "unknown";
    case NetworkHardwareState::Available:
      return "available";
    case NetworkHardwareState::Stale:
      return "stale";
    case NetworkHardwareState::Unavailable:
      return "unavailable";
  }
  return "unknown";
}

const char *networkDeviceKindName(NetworkDeviceKind kind) {
  switch (kind) {
    case NetworkDeviceKind::Unknown:
      return "unknown";
    case NetworkDeviceKind::Physical:
      return "physical";
    case NetworkDeviceKind::Virtual:
      return "virtual";
  }
  return "unknown";
}

NetworkDeviceKind classifyNetworkDeviceKind(NetworkInterfaceType type,
                                            bool device_related,
                                            const std::string &device_path) {
  // Synthetic interface types never carry a real bus device.
  switch (type) {
    case NetworkInterfaceType::Loopback:
    case NetworkInterfaceType::Virtual:
    case NetworkInterfaceType::Bridge:
    case NetworkInterfaceType::Bond:
    case NetworkInterfaceType::Tunnel:
      return NetworkDeviceKind::Virtual;
    default:
      break;
  }

  // A device living under /sys/devices/virtual/ is unambiguous software even
  // when the ARPHRD type (or the name heuristic) would suggest otherwise.
  if (device_related &&
      device_path.find("/devices/virtual/") != std::string::npos) {
    return NetworkDeviceKind::Virtual;
  }

  // No device directory: never guess. Loopback and the synthetic types above
  // already returned Virtual; a physical-looking type without a device stays
  // honest as Unknown rather than "broken" or "physical".
  if (!device_related) {
    return NetworkDeviceKind::Unknown;
  }

  return isPhysicalNetworkLink(type) ? NetworkDeviceKind::Physical
                                     : NetworkDeviceKind::Unknown;
}

NetworkHardwareRead readNetworkInterfaceHardware(
    const std::filesystem::path &root, const std::string &iface) {
  NetworkHardwareRead read;
  const std::filesystem::path iface_dir =
      root / "sys" / "class" / "net" / iface;

  // The `device` symlink is the interface's relationship to its hardware.
  std::error_code ec;
  const std::filesystem::path device_path =
      std::filesystem::canonical(iface_dir / "device", ec);
  read.read_ok = true;
  if (ec) {
    // Absent device (loopback, virtual devices without one) is a clean result;
    // a permission error is temporary and lets the caller preserve the last
    // valid metadata as stale.
    if (ec == std::errc::permission_denied) {
      read.read_ok = false;
      read.access_denied = true;
    }
    read.device_related = false;
    return read;
  }
  if (!std::filesystem::is_directory(device_path, ec) || ec) {
    read.device_related = false;
    return read;
  }
  read.device_related = true;
  read.device_path = device_path.string();

  // The device's subsystem symlink names the bus ("pci", "usb", "platform").
  std::error_code subsystem_ec;
  const std::filesystem::path subsystem =
      std::filesystem::canonical(iface_dir / "device" / "subsystem",
                                 subsystem_ec);
  if (!subsystem_ec) {
    read.device_bus = subsystem.filename().string();
  }

  bool denied = false;
  if (const std::optional<std::string> dev_id =
          readSysfsField(iface_dir, "dev_id", denied)) {
    read.device_id = *std::move(dev_id);
  }
  if (const std::optional<std::string> assign =
          readSysfsField(iface_dir, "name_assign_type", denied)) {
    read.name_assign_type = parseSysfsInt(*assign);
  }

  // The driver symlink resolves to the bound driver's directory; its basename
  // is the driver name. A missing symlink is "no bound driver" (clean), while
  // an unreadable one is temporary.
  std::error_code driver_ec;
  const std::filesystem::path driver_path =
      std::filesystem::canonical(iface_dir / "device" / "driver", driver_ec);
  if (!driver_ec) {
    read.has_driver = true;
    read.driver = driver_path.filename().string();
  } else if (driver_ec == std::errc::permission_denied) {
    read.access_denied = true;
  }

  return read;
}

NetworkInterfaceHardware updateNetworkInterfaceHardware(
    const NetworkInterfaceHardware &previous, const NetworkHardwareRead &read,
    const std::string &name, NetworkInterfaceType type,
    std::chrono::system_clock::time_point refreshed_at) {
  NetworkInterfaceHardware hw = previous;  // keep identity, first_seen
  hw.name = name;

  // A temporary probe failure preserves the last good values (marked stale):
  // a transient permission error or removed path is never turned into a fake
  // "no hardware" answer.
  if (!read.read_ok) {
    hw.device_state = hw.device_related ? NetworkHardwareState::Stale
                                        : NetworkHardwareState::Unavailable;
    hw.driver_state =
        (hw.driver.empty() ||
         hw.driver_state == NetworkHardwareState::Unavailable)
            ? NetworkHardwareState::Unavailable
            : NetworkHardwareState::Stale;
    hw.device_kind =
        classifyNetworkDeviceKind(type, hw.device_related, hw.device_path);
    return hw;
  }

  if (read.device_related) {
    hw.device_related = true;
    hw.device_path = read.device_path;
    hw.device_bus = read.device_bus;
    hw.device_id = read.device_id;
    hw.name_assign_type = read.name_assign_type;
    hw.device_state = NetworkHardwareState::Available;
  } else {
    hw.device_related = false;
    hw.device_path.clear();
    hw.device_bus.clear();
    hw.device_id.clear();
    hw.name_assign_type.reset();
    hw.device_state = NetworkHardwareState::Unavailable;
  }

  if (read.has_driver) {
    hw.driver = read.driver;
    hw.driver_state = NetworkHardwareState::Available;
  } else if (read.access_denied && !hw.driver.empty() &&
             hw.driver_state != NetworkHardwareState::Unavailable) {
    // The driver could not be read right now: keep the last known driver and
    // mark it stale instead of claiming the device lost its driver.
    hw.driver_state = NetworkHardwareState::Stale;
  } else {
    hw.driver.clear();
    hw.driver_state = NetworkHardwareState::Unavailable;
  }

  hw.device_kind =
      classifyNetworkDeviceKind(type, hw.device_related, hw.device_path);
  hw.last_read = refreshed_at;
  return hw;
}

NetworkInterfaceHardwareMonitor::NetworkInterfaceHardwareMonitor(
    std::filesystem::path root, std::chrono::milliseconds reread_interval)
    : root_(std::move(root)), reread_interval_(reread_interval) {}

const NetworkInterfaceHardware *NetworkInterfaceHardwareMonitor::tracked(
    const std::string &identity) const {
  const auto it = tracked_.find(identity);
  return it == tracked_.end() ? nullptr : &it->second;
}

void NetworkInterfaceHardwareMonitor::update(
    const NetworkInterfaceSnapshot &snapshot) {
  const auto now = std::chrono::steady_clock::now();

  std::vector<std::string> present;
  present.reserve(snapshot.interfaces.size());

  for (const NetworkInterfaceInfo &info : snapshot.interfaces) {
    const std::string identity = info.identity();
    present.push_back(identity);

    auto it = tracked_.find(identity);
    NetworkInterfaceHardware base;
    if (it == tracked_.end()) {
      base.identity = identity;
      base.first_seen = now;
    } else {
      base = it->second;  // preserves driver/device across renames
    }

    // Static metadata is cached: re-probe only on a new identity, a rename,
    // a previously failed probe, or after the reread interval elapsed.
    bool need_read = it == tracked_.end();
    if (!need_read && it->second.name != info.name) {
      need_read = true;
    }
    if (!need_read && !it->second.sysfs_read_ok) {
      need_read = true;
    }
    if (!need_read && now - it->second.last_probe >= reread_interval_) {
      need_read = true;
    }

    NetworkInterfaceHardware next;
    if (need_read) {
      const NetworkHardwareRead read =
          readNetworkInterfaceHardware(root_, info.name);
      next = updateNetworkInterfaceHardware(
          base, read, info.name, info.type, snapshot.refreshed_at);
      next.sysfs_read_ok = read.read_ok;
      next.last_probe = now;
    } else {
      next = std::move(base);  // cached metadata — no sysfs reads this tick
      next.name = info.name;
    }
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

void NetworkInterfaceHardwareMonitor::evictOverflow() {
  // Bound the number of retained identities: evict the oldest gone entries;
  // live entries are never evicted.
  if (tracked_.size() <= kMaxTrackedHardwareInterfaces) {
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
  std::size_t excess = tracked_.size() - kMaxTrackedHardwareInterfaces;
  for (const std::string &identity : gone) {
    if (excess == 0) {
      break;
    }
    tracked_.erase(identity);
    --excess;
  }
}

void NetworkInterfaceHardwareMonitor::reset() { tracked_.clear(); }

}  // namespace atm