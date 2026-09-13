#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "network_interface_hardware.hpp"
#include "network_interface_details.hpp"
#include "network_link_state.hpp"

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
void run(const char *name) { std::fprintf(stderr, "TEST %s\n", name); }
}  // namespace

#define CHECK(expr) ::expect((expr), #expr, __FILE__, __LINE__)

using atm::NetworkDeviceKind;
using atm::NetworkHardwareRead;
using atm::NetworkHardwareState;
using atm::NetworkInterfaceHardware;
using atm::NetworkInterfaceInfo;
using atm::NetworkInterfaceSnapshot;
using atm::NetworkInterfaceType;

// -------------------------------------------------------------------------
// Helpers: hermetic sysfs trees
// -------------------------------------------------------------------------

namespace {

/// Writes a small UTF-8 text file; helper for building fake sysfs attributes.
void writeFile(const std::filesystem::path &path, const std::string &content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << content;
}

/// Builds a realistic physical NIC tree:
///   <root>/sys/class/net/<name>/device -> <root>/devices/pci0000:00/.../net/<name>
///   device/subsystem -> .../sys/bus/pci
///   device/driver     -> .../sys/bus/pci/drivers/e1000e
/// plus dev_id / name_assign_type attributes. Returns the root.
std::filesystem::path makePhysicalTree(std::filesystem::path root,
                                       const std::string &name) {
  const std::filesystem::path iface =
      root / "sys" / "class" / "net" / name;
  const std::filesystem::path dev =
      root / "devices" / "pci0000:00" / "0000:00:1f.6" / "net" / name;
  std::filesystem::create_directories(iface);
  std::filesystem::create_directories(dev);
  const std::filesystem::path drivers =
      root / "sys" / "bus" / "pci" / "drivers" / "e1000e";
  std::filesystem::create_directories(drivers);

  std::filesystem::create_symlink(dev, iface / "device");
  std::filesystem::create_symlink(root / "sys" / "bus" / "pci",
                                  dev / "subsystem");
  std::filesystem::create_symlink(drivers, dev / "driver");
  writeFile(iface / "dev_id", "0x0\n");
  writeFile(iface / "name_assign_type", "1\n");
  return root;
}

/// Builds a virtual-device tree (<root>/devices/virtual/net/<name> with a
/// subsystem but no driver) — real interfaces like veth0/docker0 look like
/// this.
std::filesystem::path makeVirtualTree(std::filesystem::path root,
                                      const std::string &name) {
  const std::filesystem::path iface =
      root / "sys" / "class" / "net" / name;
  const std::filesystem::path dev =
      root / "devices" / "virtual" / "net" / name;
  std::filesystem::create_directories(iface);
  std::filesystem::create_directories(dev);
  std::filesystem::create_directories(root / "sys" / "bus" / "virtual");
  std::filesystem::create_symlink(dev, iface / "device");
  std::filesystem::create_symlink(root / "sys" / "bus" / "virtual",
                                  dev / "subsystem");
  writeFile(iface / "dev_id", "0x0\n");
  return root;
}

/// Builds a loopback-like tree: /sys/class/net/<name> with no /device and no
/// driver (as for real `lo`).
std::filesystem::path makeNoDeviceTree(std::filesystem::path root,
                                       const std::string &name) {
  std::filesystem::create_directories(root / "sys" / "class" / "net" / name);
  return root;
}

NetworkInterfaceInfo makeInfo(const std::string &name, int ifindex,
                              NetworkInterfaceType type) {
  NetworkInterfaceInfo info;
  info.name = name;
  info.type = type;
  info.link.ifindex = ifindex;
  info.refreshed_at = std::chrono::system_clock::now();
  return info;
}

NetworkInterfaceSnapshot makeSnapshot(
    const std::vector<NetworkInterfaceInfo> &interfaces) {
  NetworkInterfaceSnapshot snapshot;
  snapshot.interfaces = interfaces;
  snapshot.sysfs_readable = true;
  snapshot.addresses_readable = true;
  snapshot.refreshed_at = std::chrono::system_clock::now();
  return snapshot;
}

}  // namespace

// -------------------------------------------------------------------------
// classification
// -------------------------------------------------------------------------

void test_classify_physical() {
  run("ClassifyPhysical");
  CHECK(atm::classifyNetworkDeviceKind(
            NetworkInterfaceType::Ethernet, true,
            "/sys/devices/pci0000:00/0000:00:1f.6/net/eth0") ==
        NetworkDeviceKind::Physical);
  CHECK(atm::classifyNetworkDeviceKind(
            NetworkInterfaceType::InfiniBand, true,
            "/sys/devices/pci0000:00/0000:04:00.0/net/ib0") ==
        NetworkDeviceKind::Physical);
}

void test_classify_virtual_path() {
  run("ClassifyVirtualPath");
  // A device under /devices/virtual/ is software even for an Ethernet-looking
  // ARPHRD value.
  CHECK(atm::classifyNetworkDeviceKind(
            NetworkInterfaceType::Ethernet, true,
            "/sys/devices/virtual/net/veth0") == NetworkDeviceKind::Virtual);
}

void test_classify_synthetic_types() {
  run("ClassifySyntheticTypes");
  CHECK(atm::classifyNetworkDeviceKind(
            NetworkInterfaceType::Loopback, true,
            "/sys/devices/virtual/net/lo") == NetworkDeviceKind::Virtual);
  CHECK(atm::classifyNetworkDeviceKind(
            NetworkInterfaceType::Bridge, true,
            "/sys/devices/virtual/net/br0") == NetworkDeviceKind::Virtual);
  CHECK(atm::classifyNetworkDeviceKind(
            NetworkInterfaceType::Bond, true,
            "/sys/devices/virtual/net/bond0") == NetworkDeviceKind::Virtual);
  CHECK(atm::classifyNetworkDeviceKind(
            NetworkInterfaceType::Tunnel, true,
            "/sys/devices/virtual/net/tun0") == NetworkDeviceKind::Virtual);
  CHECK(atm::classifyNetworkDeviceKind(
            NetworkInterfaceType::Virtual, true,
            "/sys/devices/virtual/net/docker0") == NetworkDeviceKind::Virtual);
}

void test_classify_no_device_never_guesses() {
  run("ClassifyNoDeviceNeverGuesses");
  // No device relationship: honest Unknown, never "physical".
  CHECK(atm::classifyNetworkDeviceKind(NetworkInterfaceType::Ethernet, false,
                                       "") == NetworkDeviceKind::Unknown);
  CHECK(atm::classifyNetworkDeviceKind(NetworkInterfaceType::Unknown, true,
                                       "/sys/devices/platform/foo") ==
        NetworkDeviceKind::Unknown);
}

void test_kind_state_names() {
  run("KindStateNames");
  CHECK(std::string(atm::networkDeviceKindName(NetworkDeviceKind::Unknown)) ==
        "unknown");
  CHECK(std::string(atm::networkDeviceKindName(NetworkDeviceKind::Physical)) ==
        "physical");
  CHECK(std::string(atm::networkDeviceKindName(NetworkDeviceKind::Virtual)) ==
        "virtual");
  CHECK(std::string(atm::networkHardwareStateName(NetworkHardwareState::Unknown)) ==
        "unknown");
  CHECK(std::string(atm::networkHardwareStateName(NetworkHardwareState::Available)) ==
        "available");
  CHECK(std::string(atm::networkHardwareStateName(NetworkHardwareState::Stale)) ==
        "stale");
  CHECK(std::string(atm::networkHardwareStateName(NetworkHardwareState::Unavailable)) ==
        "unavailable");
}

// -------------------------------------------------------------------------
// readNetworkInterfaceHardware: fake sysfs trees
// -------------------------------------------------------------------------

void test_read_physical_tree() {
  run("ReadPhysicalTree");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      "atm-nihw-test-physical";
  std::filesystem::remove_all(root);
  makePhysicalTree(root, "eth0");

  const NetworkHardwareRead read =
      atm::readNetworkInterfaceHardware(root, "eth0");
  CHECK(read.read_ok);
  CHECK(!read.access_denied);
  CHECK(read.device_related);
  CHECK(read.device_path.find("/devices/pci0000:00/") != std::string::npos);
  CHECK(read.device_bus == "pci");
  CHECK(read.has_driver);
  CHECK(read.driver == "e1000e");
  CHECK(read.device_id == "0x0");
  CHECK(read.name_assign_type.has_value() && *read.name_assign_type == 1);
  std::filesystem::remove_all(root);
}

void test_read_virtual_tree() {
  run("ReadVirtualTree");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-nihw-test-virtual";
  std::filesystem::remove_all(root);
  makeVirtualTree(root, "veth0");

  const NetworkHardwareRead read =
      atm::readNetworkInterfaceHardware(root, "veth0");
  CHECK(read.read_ok);
  CHECK(read.device_related);
  CHECK(read.device_path.find("/devices/virtual/") != std::string::npos);
  CHECK(read.device_bus == "virtual");
  CHECK(!read.has_driver);
  CHECK(read.driver.empty());
  CHECK(read.device_id == "0x0");
  std::filesystem::remove_all(root);
}

void test_read_no_device() {
  run("ReadNoDevice");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-nihw-test-nodevice";
  std::filesystem::remove_all(root);
  makeNoDeviceTree(root, "lo");

  const NetworkHardwareRead read =
      atm::readNetworkInterfaceHardware(root, "lo");
  CHECK(read.read_ok);
  CHECK(!read.access_denied);
  CHECK(!read.device_related);
  CHECK(!read.has_driver);
  std::filesystem::remove_all(root);
}

void test_read_broken_driver_symlink() {
  run("ReadBrokenDriverSymlink");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-nihw-test-brokendriver";
  std::filesystem::remove_all(root);
  const std::filesystem::path iface =
      root / "sys" / "class" / "net" / "eth0";
  const std::filesystem::path dev =
      root / "devices" / "pci0000:00" / "eth0";
  std::filesystem::create_directories(iface);
  std::filesystem::create_directories(dev);
  std::filesystem::create_symlink(dev, iface / "device");
  // A dangling driver symlink (the driver was unbound): clean "no driver".
  std::filesystem::create_symlink(root / "sys" / "bus" / "pci" / "drivers" /
                                      "missing-driver",
                                  dev / "driver");

  const NetworkHardwareRead read =
      atm::readNetworkInterfaceHardware(root, "eth0");
  CHECK(read.read_ok);
  CHECK(!read.access_denied);
  CHECK(read.device_related);
  CHECK(!read.has_driver);
  CHECK(read.driver.empty());
  std::filesystem::remove_all(root);
}

void test_read_missing_interface_dir() {
  run("ReadMissingInterfaceDir");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-nihw-test-missing";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "sys" / "class" / "net");

  const NetworkHardwareRead read =
      atm::readNetworkInterfaceHardware(root, "no-such-iface");
  CHECK(read.read_ok);  // ENOENT is a clean result, not an error
  CHECK(!read.access_denied);
  CHECK(!read.device_related);
  CHECK(!read.has_driver);
  std::filesystem::remove_all(root);
}

void test_read_missing_root() {
  run("ReadMissingRoot");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-nihw-test-missingroot";
  std::filesystem::remove_all(root);
  const NetworkHardwareRead read =
      atm::readNetworkInterfaceHardware(root, "eth0");
  CHECK(read.read_ok);
  CHECK(!read.access_denied);
  CHECK(!read.device_related);
  std::filesystem::remove_all(root);
}

// -------------------------------------------------------------------------
// updateNetworkInterfaceHardware: pure merge semantics
// -------------------------------------------------------------------------

NetworkInterfaceHardware physicalHardware() {
  NetworkInterfaceHardware hw;
  hw.identity = "idx:2";
  hw.name = "eth0";
  hw.device_related = true;
  hw.device_path = "/sys/devices/pci0000:00/net/eth0";
  hw.device_bus = "pci";
  hw.device_id = "0x0";
  hw.name_assign_type = 1;
  hw.device_kind = NetworkDeviceKind::Physical;
  hw.device_state = NetworkHardwareState::Available;
  hw.driver = "e1000e";
  hw.driver_state = NetworkHardwareState::Available;
  hw.last_read = std::chrono::system_clock::now();
  return hw;
}

NetworkHardwareRead goodRead() {
  NetworkHardwareRead read;
  read.device_related = true;
  read.device_path = "/sys/devices/pci0000:00/net/eth0";
  read.device_bus = "pci";
  read.device_id = "0x0";
  read.name_assign_type = 1;
  read.has_driver = true;
  read.driver = "e1000e";
  return read;
}

void test_update_fresh_physical() {
  run("UpdateFreshPhysical");
  const auto now = std::chrono::system_clock::now();
  const NetworkInterfaceHardware hw = atm::updateNetworkInterfaceHardware(
      NetworkInterfaceHardware{}, goodRead(), "eth0",
      NetworkInterfaceType::Ethernet, now);
  CHECK(hw.device_related);
  CHECK(hw.device_bus == "pci");
  CHECK(hw.device_id == "0x0");
  CHECK(hw.name_assign_type.has_value() && *hw.name_assign_type == 1);
  CHECK(hw.device_state == NetworkHardwareState::Available);
  CHECK(hw.driver == "e1000e");
  CHECK(hw.driver_state == NetworkHardwareState::Available);
  CHECK(hw.device_kind == NetworkDeviceKind::Physical);
  CHECK(hw.last_read == now);
  CHECK(hw.present);
}

void test_update_read_failure_preserves_stale() {
  run("UpdateReadFailurePreservesStale");
  NetworkInterfaceHardware previous = physicalHardware();
  previous.driver_state = NetworkHardwareState::Available;
  const auto last = previous.last_read;

  NetworkHardwareRead denied;
  denied.read_ok = false;
  denied.access_denied = true;
  const auto now = last + std::chrono::seconds(1);
  const NetworkInterfaceHardware hw = atm::updateNetworkInterfaceHardware(
      previous, denied, "eth0", NetworkInterfaceType::Ethernet, now);
  // Values preserved, marked stale — never dropped, never zeroed.
  CHECK(hw.device_related);
  CHECK(hw.device_state == NetworkHardwareState::Stale);
  CHECK(hw.driver == "e1000e");
  CHECK(hw.driver_state == NetworkHardwareState::Stale);
  CHECK(hw.device_kind == NetworkDeviceKind::Physical);
}

void test_update_clean_absence_unavailable() {
  run("UpdateCleanAbsenceUnavailable");
  NetworkInterfaceHardware previous = physicalHardware();
  const auto now = previous.last_read + std::chrono::seconds(1);

  NetworkHardwareRead clean;  // loopback / virtual / no device
  const NetworkInterfaceHardware hw = atm::updateNetworkInterfaceHardware(
      previous, clean, "lo", NetworkInterfaceType::Loopback, now);
  CHECK(!hw.device_related);
  CHECK(hw.device_state == NetworkHardwareState::Unavailable);
  CHECK(hw.driver.empty());
  CHECK(hw.driver_state == NetworkHardwareState::Unavailable);
  CHECK(hw.device_kind == NetworkDeviceKind::Virtual);
}

void test_update_driver_denied_preserves_last_driver() {
  run("UpdateDriverDeniedPreservesLastDriver");
  NetworkInterfaceHardware previous = physicalHardware();
  const auto now = previous.last_read + std::chrono::seconds(1);

  // Device readable but the driver symlink was temporarily unreadable.
  NetworkHardwareRead read;
  read.device_related = true;
  read.device_path = previous.device_path;
  read.device_bus = "pci";
  read.access_denied = true;
  const NetworkInterfaceHardware hw = atm::updateNetworkInterfaceHardware(
      previous, read, "eth0", NetworkInterfaceType::Ethernet, now);
  CHECK(hw.device_state == NetworkHardwareState::Available);  // device fresh
  CHECK(hw.driver == "e1000e");
  CHECK(hw.driver_state == NetworkHardwareState::Stale);  // driver preserved
  CHECK(hw.last_read == now);
}

void test_update_driver_denied_without_previous() {
  run("UpdateDriverDeniedWithoutPrevious");
  NetworkHardwareRead read;
  read.device_related = true;
  read.device_path = "/sys/devices/pci0000:00/net/eth0";
  read.access_denied = true;
  const auto now = std::chrono::system_clock::now();
  const NetworkInterfaceHardware hw = atm::updateNetworkInterfaceHardware(
      NetworkInterfaceHardware{}, read, "eth0",
      NetworkInterfaceType::Ethernet, now);
  CHECK(hw.driver.empty());
  CHECK(hw.driver_state == NetworkHardwareState::Unavailable);
}

void test_update_driver_unbound_clears() {
  run("UpdateDriverUnboundClears");
  NetworkInterfaceHardware previous = physicalHardware();
  NetworkHardwareRead read;
  read.device_related = true;
  read.device_path = previous.device_path;
  read.device_bus = "pci";
  // A clean read where the driver symlink is gone: the driver was unbound.
  const auto now = previous.last_read + std::chrono::seconds(1);
  const NetworkInterfaceHardware hw = atm::updateNetworkInterfaceHardware(
      previous, read, "eth0", NetworkInterfaceType::Ethernet, now);
  CHECK(hw.driver.empty());
  CHECK(hw.driver_state == NetworkHardwareState::Unavailable);
}

// -------------------------------------------------------------------------
// formatMacAddress
// -------------------------------------------------------------------------

void test_format_mac() {
  run("FormatMac");
  CHECK(atm::formatMacAddress(std::optional<std::string>("aa:bb:cc:dd:ee:ff")) ==
        "aa:bb:cc:dd:ee:ff");
  CHECK(atm::formatMacAddress(std::optional<std::string>("AA:BB:CC:DD:EE:FF")) ==
        "aa:bb:cc:dd:ee:ff");
  CHECK(atm::formatMacAddress(std::optional<std::string>("00:11:22:aA:bB:cC")) ==
        "00:11:22:aa:bb:cc");
  CHECK(atm::formatMacAddress(std::nullopt).empty());
  CHECK(atm::formatMacAddress(std::optional<std::string>("")).empty());
  CHECK(atm::formatMacAddress(std::optional<std::string>("aa:bb:cc:dd:ee")).empty());
  CHECK(atm::formatMacAddress(std::optional<std::string>("aa:bb:cc:dd:ee:ff:00")).empty());
  CHECK(atm::formatMacAddress(std::optional<std::string>("zz:bb:cc:dd:ee:ff")).empty());
  CHECK(atm::formatMacAddress(std::optional<std::string>("aabbccddeeff")).empty());
}

// -------------------------------------------------------------------------
// Monitor behaviour: caching, renames, disappearance, eviction
// -------------------------------------------------------------------------

void test_monitor_initial_read_and_cache() {
  run("MonitorInitialReadAndCache");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-nihw-test-cache";
  std::filesystem::remove_all(root);
  makePhysicalTree(root, "eth0");

  atm::NetworkInterfaceHardwareMonitor monitor(root,
                                               std::chrono::hours(1));
  monitor.update(makeSnapshot(
      {makeInfo("eth0", 2, NetworkInterfaceType::Ethernet)}));
  const NetworkInterfaceHardware *hw = monitor.tracked("idx:2");
  CHECK(hw != nullptr);
  CHECK(hw->present);
  CHECK(hw->driver == "e1000e");
  CHECK(hw->device_bus == "pci");
  CHECK(hw->device_kind == NetworkDeviceKind::Physical);

  // Rebind the driver on disk; the second update happens within the 1-hour
  // cooldown with the same name and a successful probe, so the cached metadata
  // must be reused — never re-read per update/render.
  std::filesystem::remove(root / "devices" / "pci0000:00" / "0000:00:1f.6" /
                          "net" / "eth0" / "driver");
  monitor.update(makeSnapshot(
      {makeInfo("eth0", 2, NetworkInterfaceType::Ethernet)}));
  hw = monitor.tracked("idx:2");
  CHECK(hw != nullptr);
  CHECK(hw->driver == "e1000e");

  std::filesystem::remove_all(root);
}

void test_monitor_rereads_after_cooldown() {
  run("MonitorRereadsAfterCooldown");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-nihw-test-cooldown";
  std::filesystem::remove_all(root);
  makePhysicalTree(root, "eth0");

  atm::NetworkInterfaceHardwareMonitor monitor(root,
                                               std::chrono::milliseconds(50));
  monitor.update(makeSnapshot(
      {makeInfo("eth0", 2, NetworkInterfaceType::Ethernet)}));
  CHECK(monitor.tracked("idx:2")->driver == "e1000e");

  // After the 50 ms interval elapses the metadata is re-probed.
  const std::filesystem::path dev =
      root / "devices" / "pci0000:00" / "0000:00:1f.6" / "net" / "eth0";
  std::filesystem::remove(dev / "driver");
  std::filesystem::create_directories(
      root / "sys" / "bus" / "pci" / "drivers" / "e1000f");
  std::filesystem::create_symlink(
      root / "sys" / "bus" / "pci" / "drivers" / "e1000f", dev / "driver");
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  monitor.update(makeSnapshot(
      {makeInfo("eth0", 2, NetworkInterfaceType::Ethernet)}));
  const NetworkInterfaceHardware *hw = monitor.tracked("idx:2");
  CHECK(hw != nullptr);
  CHECK(hw->driver == "e1000f");

  std::filesystem::remove_all(root);
}

void test_monitor_force_reread_on_rename() {
  run("MonitorForceRereadOnRename");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-nihw-test-rename";
  std::filesystem::remove_all(root);
  makePhysicalTree(root, "eth0");

  atm::NetworkInterfaceHardwareMonitor monitor(root,
                                               std::chrono::hours(1));
  monitor.update(makeSnapshot(
      {makeInfo("eth0", 2, NetworkInterfaceType::Ethernet)}));
  CHECK(monitor.tracked("idx:2")->driver == "e1000e");

  // Same ifindex, new name: the identity is preserved but the metadata is
  // re-probed under the new kernel name (the on-disk tree has no eth5, so the
  // clean result is a conservative "no device" rather than stale data).
  monitor.update(makeSnapshot(
      {makeInfo("eth5", 2, NetworkInterfaceType::Ethernet)}));
  const NetworkInterfaceHardware *hw = monitor.tracked("idx:2");
  CHECK(hw != nullptr);
  CHECK(hw->name == "eth5");
  CHECK(!hw->device_related);
  CHECK(hw->device_state == NetworkHardwareState::Unavailable);

  std::filesystem::remove_all(root);
}

void test_monitor_clean_absence_cached_then_refreshed() {
  run("MonitorCleanAbsenceCachedThenRefreshed");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-nihw-test-retry";
  std::filesystem::remove_all(root);
  // No eth0 tree yet: a missing /device is a *clean result* (like loopback),
  // never an error, so it is cached like any other metadata.
  std::filesystem::create_directories(root / "sys" / "class" / "net");

  atm::NetworkInterfaceHardwareMonitor monitor(
      root, std::chrono::milliseconds(30));
  monitor.update(makeSnapshot(
      {makeInfo("eth0", 2, NetworkInterfaceType::Ethernet)}));
  CHECK(monitor.tracked("idx:2") != nullptr);
  CHECK(!monitor.tracked("idx:2")->device_related);

  // A device that appears later (hotplug/driver load) is picked up once the
  // reread interval elapses — no per-update sysfs probe on loopback-like
  // interfaces either.
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  makePhysicalTree(root, "eth0");
  monitor.update(makeSnapshot(
      {makeInfo("eth0", 2, NetworkInterfaceType::Ethernet)}));
  const NetworkInterfaceHardware *hw = monitor.tracked("idx:2");
  CHECK(hw != nullptr);
  CHECK(hw->device_related);
  CHECK(hw->driver == "e1000e");

  std::filesystem::remove_all(root);
}

void test_monitor_new_identity_reads() {
  run("MonitorNewIdentityReads");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-nihw-test-identity";
  std::filesystem::remove_all(root);
  makePhysicalTree(root, "eth0");
  makeNoDeviceTree(root, "lo");

  atm::NetworkInterfaceHardwareMonitor monitor(root,
                                               std::chrono::hours(1));
  monitor.update(makeSnapshot({makeInfo("eth0", 2, NetworkInterfaceType::Ethernet),
                               makeInfo("lo", 1, NetworkInterfaceType::Loopback)}));
  CHECK(monitor.tracked("idx:2")->driver == "e1000e");
  CHECK(monitor.tracked("idx:2")->device_kind == NetworkDeviceKind::Physical);
  CHECK(monitor.tracked("idx:1")->device_kind == NetworkDeviceKind::Virtual);
  CHECK(monitor.tracked("idx:1")->driver_state ==
        NetworkHardwareState::Unavailable);

  std::filesystem::remove_all(root);
}

void test_monitor_disappearance_preserved() {
  run("MonitorDisappearancePreserved");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-nihw-test-gone";
  std::filesystem::remove_all(root);
  makePhysicalTree(root, "eth0");

  atm::NetworkInterfaceHardwareMonitor monitor(root,
                                               std::chrono::hours(1));
  monitor.update(makeSnapshot(
      {makeInfo("eth0", 2, NetworkInterfaceType::Ethernet)}));
  CHECK(monitor.tracked("idx:2")->present);

  monitor.update(makeSnapshot({}));
  CHECK(monitor.tracked("idx:2") != nullptr);
  CHECK(!monitor.tracked("idx:2")->present);

  std::filesystem::remove_all(root);
}

void test_monitor_recreation_starts_fresh() {
  run("MonitorRecreationStartsFresh");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-nihw-test-recreate";
  std::filesystem::remove_all(root);
  makePhysicalTree(root, "eth0");

  atm::NetworkInterfaceHardwareMonitor monitor(root,
                                               std::chrono::hours(1));
  monitor.update(makeSnapshot(
      {makeInfo("eth0", 2, NetworkInterfaceType::Ethernet)}));
  CHECK(monitor.tracked("idx:2")->driver == "e1000e");

  // The device is removed and a different device reuses the name with a new
  // ifindex: a new identity must not inherit the old ones' metadata.
  std::filesystem::remove_all(root);
  makePhysicalTree(root, "eth0");
  monitor.update(makeSnapshot(
      {makeInfo("eth0", 7, NetworkInterfaceType::Ethernet)}));
  const NetworkInterfaceHardware *old = monitor.tracked("idx:2");
  const NetworkInterfaceHardware *fresh = monitor.tracked("idx:7");
  CHECK(old != nullptr && !old->present);
  CHECK(fresh != nullptr && fresh->present);
  CHECK(fresh->driver == "e1000e");

  std::filesystem::remove_all(root);
}

void test_monitor_eviction_bound() {
  run("MonitorEvictionBound");
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "atm-nihw-test-evict";
  std::filesystem::remove_all(root);

  atm::NetworkInterfaceHardwareMonitor monitor(root,
                                               std::chrono::hours(1));
  std::vector<NetworkInterfaceInfo> many;
  for (int i = 1; i <= 70; ++i) {
    many.push_back(makeInfo("veth" + std::to_string(i), i,
                            NetworkInterfaceType::Virtual));
  }
  monitor.update(makeSnapshot(many));
  // All 70 are live: a live entry is never evicted, so all are retained.
  CHECK(monitor.entries().size() == 70);

  monitor.update(makeSnapshot({}));
  CHECK(monitor.entries().size() <=
        atm::NetworkInterfaceHardwareMonitor::kMaxTrackedHardwareInterfaces);
  for (const auto &kv : monitor.entries()) {
    CHECK(!kv.second.present);
  }

  std::filesystem::remove_all(root);
}

void test_monitor_reset() {
  run("MonitorReset");
  atm::NetworkInterfaceHardwareMonitor monitor;
  monitor.update(makeSnapshot(
      {makeInfo("eth0", 2, NetworkInterfaceType::Ethernet)}));
  CHECK(monitor.entries().size() == 1);
  monitor.reset();
  CHECK(monitor.entries().empty());
  CHECK(monitor.tracked("idx:2") == nullptr);
}

int main() {
  test_classify_physical();
  test_classify_virtual_path();
  test_classify_synthetic_types();
  test_classify_no_device_never_guesses();
  test_kind_state_names();

  test_read_physical_tree();
  test_read_virtual_tree();
  test_read_no_device();
  test_read_broken_driver_symlink();
  test_read_missing_interface_dir();
  test_read_missing_root();

  test_update_fresh_physical();
  test_update_read_failure_preserves_stale();
  test_update_clean_absence_unavailable();
  test_update_driver_denied_preserves_last_driver();
  test_update_driver_denied_without_previous();
  test_update_driver_unbound_clears();

  test_format_mac();

  test_monitor_initial_read_and_cache();
  test_monitor_rereads_after_cooldown();
  test_monitor_force_reread_on_rename();
  test_monitor_clean_absence_cached_then_refreshed();
  test_monitor_new_identity_reads();
  test_monitor_disappearance_preserved();
  test_monitor_recreation_starts_fresh();
  test_monitor_eviction_bound();
  test_monitor_reset();

  std::fprintf(stderr, "\n%zu checks, %d failures\n", g_checks + 0UL,
               g_failures);
  if (g_failures != 0) {
    std::fprintf(stderr, "RESULT: FAIL\n");
    return 1;
  }
  std::fprintf(stderr, "RESULT: PASS\n");
  return 0;
}