#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace atm {

/// GPU vendor, derived from the PCI vendor ID exposed by the kernel under
/// /sys/class/drm/card*/device/vendor. `Unknown` covers every vendor the
/// application does not special-case (including unrecognized device IDs).
enum class GpuVendor {
  Unknown = 0,
  Amd,
  Intel,
  Nvidia,
};

/// Human-readable display name of a vendor ("AMD", "Intel", "NVIDIA",
/// "Unknown").
[[nodiscard]] const char *gpuVendorName(GpuVendor vendor);

/// One graphics device and its live metrics.
///
/// Static identity (card node, PCI slot, name, vendor, driver) is discovered
/// once and cached. Every dynamic field is `std::optional`: it is **unset**
/// (rendered as "N/A") whenever the driver or kernel does not expose the
/// value, so an unavailable metric is never faked as 0.
struct GpuStats {
  std::string card;      // DRM primary node, e.g. "card0", "card1"
  std::string pci_slot;  // PCI address, e.g. "0000:00:02.0" ("" if unknown)
  std::string name;      // model resolved from the PCI ID database,
                         // else "Vendor 0x<device-id>" or just the vendor
  GpuVendor vendor = GpuVendor::Unknown;
  std::string driver;  // kernel driver: "i915", "amdgpu", "nouveau", ... ("" if none)

  std::optional<double> utilization_percent;  // GT utilization, 0.0 – 100.0
  std::optional<std::uint64_t> memory_total_bytes;
  std::optional<std::uint64_t> memory_used_bytes;
  std::optional<std::uint64_t> memory_free_bytes;
  std::optional<double> memory_usage_percent;  // used / total * 100
  std::optional<std::uint64_t> frequency_hz;
  std::optional<double> power_watts;
};

/// One complete GPU snapshot, produced by GpuMonitor::read(). Devices are
/// ordered by DRM card number ascending (card0, card1, ...).
struct GpuSnapshot {
  std::vector<GpuStats> devices;
};

/// Formats a frequency in Hz for the UI: "1200 MHz" below 1 GHz and
/// "1.20 GHz" at/above it. Returns "N/A" when unset.
[[nodiscard]] std::string formatGpuFrequency(
    const std::optional<std::uint64_t> &frequency_hz);

/// Formats driver-reported power for the UI: "45.2 W". Returns "N/A" when
/// unset, so power is never estimated.
[[nodiscard]] std::string formatGpuPower(const std::optional<double> &watts);

/**
 * Monitors every GPU exposed by the DRM subsystem.
 *
 * Devices are discovered from /sys/class/drm/card* (the DRM primary nodes);
 * the kernel-provided PCI vendor/device IDs and the bound driver identify each
 * one, and a small read-only PCI ID database (pci.ids when installed) is used
 * to resolve a friendly model name. Dynamic metrics are read directly from
 * kernel interfaces — no shell commands, no nvidia-smi, no external tools and
 * no GPU control of any kind.
 *
 * Supported sources per vendor (whatever the local driver actually exposes):
 *   - AMD (amdgpu): `gpu_busy_percent`, `mem_info_vram_total`/`used`,
 *     `pp_dpm_sclk` current clock, hwmon `power1_input`.
 *   - Intel (i915): per-GT `rps_act_freq_mhz`, card-level `gt_act_freq_mhz`,
 *     and GT utilization derived from the two-sample RC6 residency delta.
 *   - NVIDIA and unknown vendors: identity only; metrics stay "N/A" unless a
 *     generic hwmon power sensor is present.
 *
 * Any metric the driver does not expose is simply left unset. The monitor
 * never terminates or throws because a sysfs value is missing — it retries
 * discovery automatically the next read if the card set ever changes.
 *
 * The refresh cadence is owned by the application's main loop; this class
 * never starts its own thread.
 */
class GpuMonitor {
 public:
  GpuMonitor() = default;
  ~GpuMonitor() = default;

  // Monitors hold per-device diffing state; copying/moving would duplicate
  // the baselines.
  GpuMonitor(const GpuMonitor &) = delete;
  GpuMonitor &operator=(const GpuMonitor &) = delete;

  /// Refreshes every known device's dynamic metrics and returns the snapshot.
  /// The first read establishes baselines for derived metrics (Intel GT busy
  /// from RC6), which then report real values from the second read on.
  [[nodiscard]] GpuSnapshot read();

 private:
  struct GpuDevice {
    GpuStats stats;              // identity (cached) + freshly refreshed metrics
    std::string sysfs_path;      // /sys/class/drm/<card>
    std::optional<std::uint64_t> rc6_ms;  // Intel RC6 residency baseline
    std::optional<std::chrono::steady_clock::time_point> rc6_time;
  };

  std::vector<std::string> discovered_cards_;  // card names seen at last discovery
  std::vector<GpuDevice> devices_;

  void discover(const std::vector<std::string> &cards);
  static void refreshMetrics(GpuDevice &device);
};

}  // namespace atm