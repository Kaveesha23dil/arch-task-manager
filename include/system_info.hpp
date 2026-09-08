#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "gpu_monitor.hpp"

namespace atm {

/**
 * Static, high-level overview of the machine. Everything here is cached once
 * and only refreshed on demand: hostname, OS/distribution, kernel, CPU model
 * and topology, total memory/swap, GPU names and DMI hardware identity.
 *
 * This deliberately does **not** duplicate the high-frequency monitoring in
 * CpuMonitor / MemoryMonitor / GpuMonitor / SensorMonitor. Those continuously
 * sample /proc or /sys each second; SystemInfo is a point-in-time, mostly
 * immutable snapshot of the hardware and OS identity.
 */
struct SystemInfo {
  std::string hostname;

  std::string operating_system;  // PRETTY_NAME (or NAME) from /etc/os-release
  std::string distribution;      // ID from /etc/os-release (e.g. "arch")
  std::string distribution_version;  // VERSION_ID (e.g. "rolling")
  std::string kernel_version;    // release from uname(2)
  std::string kernel_release;    // full version string from uname(2) (may be "")
  std::string architecture;      // machine from uname(2)

  std::string cpu_model;         // model name from /proc/cpuinfo
  std::string cpu_architecture;  // architecture (from uname machine, fallback)
  std::uint32_t cpu_logical_cores = 0;
  std::uint32_t cpu_physical_cores = 0;  // 0 => unknown (rendered "N/A")

  std::uint64_t total_memory = 0;        // bytes
  std::uint64_t total_swap = 0;          // bytes

  std::string manufacturer;   // /sys/class/dmi/id/sys_vendor
  std::string product_name;   // /sys/class/dmi/id/product_name
  std::string product_version;  // /sys/class/dmi/id/product_version
  std::string motherboard;    // /sys/class/dmi/id/board_name
  std::string motherboard_vendor;  // /sys/class/dmi/id/board_vendor
  std::string motherboard_version;  // /sys/class/dmi/id/board_version
  std::string bios_vendor;    // /sys/class/dmi/id/bios_vendor
  std::string bios_version;   // /sys/class/dmi/id/bios_version
  std::string bios_date;      // /sys/class/dmi/id/bios_date

  /// Uptime in seconds since boot, read from /proc/uptime. 0 if unknown.
  std::uint64_t uptime_seconds = 0;

  /// GPU model names (friendly names resolved from PCI IDs). Empty when none
  /// is detected. Filled from the existing GpuMonitor's cached identity.
  std::vector<std::string> gpus;
};

/// Formats `seconds` of uptime as a compact human-readable string, e.g.
/// "45 seconds", "12 minutes", "4 hours, 25 minutes", "2 days, 7 hours,
/// 10 minutes". Unnecessary precision is avoided; a value of 0 yields
/// "0 seconds".
[[nodiscard]] std::string formatUptime(std::uint64_t seconds);

/**
 * Loads and caches the static system / hardware overview.
 *
 * The object reads every source exactly once inside load(); read() then
 * returns a cheap copy of the cached snapshot on every tick, so the DMI
 * files, /etc/os-release and /proc/cpuinfo are never rescanned continuously.
 *
 * Missing files, permission errors and malformed data never crash the
 * loader: each field simply stays empty (rendered "N/A" by the front-end)
 * or takes a safe default. This module is strictly read-only; it never
 * modifies /proc, /sys, /etc/os-release or any DMI data, and it never invokes
 * shell commands.
 */
class SystemInfoProvider {
 public:
  SystemInfoProvider() = default;
  ~SystemInfoProvider() = default;

  // The provider caches a snapshot; copying/moving would duplicate it.
  SystemInfoProvider(const SystemInfoProvider &) = delete;
  SystemInfoProvider &operator=(const SystemInfoProvider &) = delete;

  /// Re-reads all static information and updates the cached snapshot.
  /// Uses the existing MemoryMonitor totals and GpuMonitor identity where
  /// practical to avoid duplicate RAM/GPU detection. `gpu` may be the current
  /// GpuSnapshot from the application's own GpuMonitor; its device names are
  /// copied into the cached snapshot.
  void load(const GpuSnapshot &gpu);

  /// Returns a copy of the cached snapshot.
  [[nodiscard]] SystemInfo read() const;

 private:
  SystemInfo info_;
};

}  // namespace atm
