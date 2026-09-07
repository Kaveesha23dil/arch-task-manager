#pragma once

#include <cstdint>
#include <optional>

namespace atm {

/**
 * Raw memory and swap counters from /proc/meminfo.
 *
 * The kernel reports every one of these values in kibibytes (kB, 1024 bytes).
 * All fields are unsigned so arithmetic cannot go negative; the publicly
 * derived getters additionally clamp, so a caller never observes underflow.
 */
struct MemoryInfo {
  std::uint64_t total = 0;       // MemTotal
  std::uint64_t available = 0;   // MemAvailable
  std::uint64_t free = 0;        // MemFree
  std::uint64_t cached = 0;      // Cached
  std::uint64_t buffers = 0;     // Buffers
  std::uint64_t swap_total = 0;  // SwapTotal
  std::uint64_t swap_free = 0;   // SwapFree

  /// Used RAM = Total − Available, clamped to zero.
  [[nodiscard]] std::uint64_t used() const;

  /// Used swap = SwapTotal − SwapFree, clamped to zero.
  [[nodiscard]] std::uint64_t swapUsed() const;

  /// RAM usage in percent (0.0 – 100.0). Returns 0 if total is zero.
  [[nodiscard]] double usagePercent() const;

  /// Swap usage in percent (0.0 – 100.0). Returns 0 when no swap is
  /// configured (SwapTotal is zero), avoiding a division by zero.
  [[nodiscard]] double swapUsagePercent() const;
};

/**
 * Reads and parses /proc/meminfo.
 *
 * Returns std::nullopt when the file cannot be opened. Missing or malformed
 * lines never crash the parser: only recognized fields are captured and any
 * field the kernel did not provide keeps its default (0). As a safety net,
 * when MemAvailable is absent the classic approximation
 * (MemFree + Buffers + Cached) is used instead.
 */
[[nodiscard]] std::optional<MemoryInfo> readMemoryInfo();

/**
 * Monitors system memory and swap by reading /proc/meminfo.
 *
 * Unlike CPU utilization, memory usage is a point-in-time snapshot — there are
 * no running counters to diff — so the monitor itself holds no state.
 */
class MemoryMonitor {
 public:
  MemoryMonitor() = default;
  ~MemoryMonitor() = default;

  // Mirrors CpuMonitor: monitors are non-copyable, non-movable objects.
  MemoryMonitor(const MemoryMonitor &) = delete;
  MemoryMonitor &operator=(const MemoryMonitor &) = delete;

  /// Returns a fresh /proc/meminfo snapshot, or std::nullopt on error.
  [[nodiscard]] std::optional<MemoryInfo> read();
};

}  // namespace atm