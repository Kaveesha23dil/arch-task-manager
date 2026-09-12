#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "disk_health.hpp"

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

void expectNear(double a, double b, double eps, const char *expr,
                const char *file, int line) {
  ++g_checks;
  if (!std::isfinite(a) || !(a > b - eps && a < b + eps)) {
    ++g_failures;
    std::fprintf(stderr, "FAIL %s:%d: %s (%f vs %f)\n", file, line, expr, a, b);
  }
}

void run(const char *name) {
  std::fprintf(stderr, "TEST %s\n", name);
}
}  // namespace

#define CHECK(expr) ::expect((expr), #expr, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, eps) \
  ::expectNear(double(a), double(b), (eps), #a " ~ " #b, __FILE__, __LINE__)

namespace fs = std::filesystem;

/// Per-test temp filesystem root mimicking / for sysfs reads.
struct TestRoot {
  fs::path root =
      fs::temp_directory_path() /
      ("arch-task-manager-health-" + std::to_string(::getpid()) + "-" +
       std::to_string(reinterpret_cast<std::uintptr_t>(&root)));

  TestRoot() { fs::create_directories(root); }
  ~TestRoot() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }

  void write(const fs::path &path, const std::string &content) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path);
    file << content;
  }
};

/// Builds a 512-byte NVMe SMART log; all fields zero by default.
class NvmeLogBuilder {
 public:
  NvmeLogBuilder() { buffer_.fill(0); }

  std::uint8_t &byte(std::size_t offset) { return buffer_.at(offset); }

  void u16(std::size_t offset, std::uint16_t value) {
    buffer_.at(offset) = static_cast<std::uint8_t>(value);
    buffer_.at(offset + 1) = static_cast<std::uint8_t>(value >> 8);
  }

  void u32(std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
      buffer_.at(offset + i) = static_cast<std::uint8_t>(value >> (8 * i));
    }
  }

  void u128(std::size_t offset, std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) {
      buffer_.at(offset + i) = static_cast<std::uint8_t>(value >> (8 * i));
    }
    for (std::size_t i = 8; i < 16; ++i) {
      buffer_.at(offset + i) = 0;
    }
  }

  const std::uint8_t *data() const { return buffer_.data(); }
  std::size_t size() const { return buffer_.size(); }

 private:
  std::array<std::uint8_t, 512> buffer_{};
};

/// Shared, inspectable behavior of a fake provider (the provider itself is
/// freshly owned by the monitor on each refresh).
struct FakeScript {
  explicit FakeScript(atm::DiskHealthSnapshot result)
      : result(std::move(result)) {}
  atm::DiskHealthSnapshot result;
  std::atomic<int> calls{0};
  std::chrono::milliseconds delay{0};
};

/// A fake provider whose behavior is scripted by the test.
struct FakeProvider : atm::DiskHealthProvider {
  explicit FakeProvider(std::shared_ptr<FakeScript> script)
      : script(std::move(script)) {}
  atm::DiskHealthSnapshot readHealth(const atm::DiskHealthSnapshot &base) override {
    ++script->calls;
    if (script->delay.count() > 0) {
      std::this_thread::sleep_for(script->delay);
    }
    atm::DiskHealthSnapshot out = script->result;
    out.device_name = base.device_name;
    out.device_path = base.device_path;
    out.major = base.major;
    out.minor = base.minor;
    return out;
  }
  const char *name() const override { return "fake"; }

  std::shared_ptr<FakeScript> script;
};

/// A good fake result classifying a device as healthy.
atm::DiskHealthSnapshot healthyResult(atm::DiskHealthSource source) {
  atm::DiskHealthSnapshot snap;
  snap.ok = true;
  snap.source = source;
  snap.status = atm::DiskHealthStatus::Healthy;
  snap.model = "FAKE-DISK";
  return snap;
}

int main() {
  // --- ATA SMART raw helpers ----------------------------------------------
  run("parseSmartRawValue reads least-significant-first 48 bits");
  {
    std::array<std::uint8_t, atm::kSmartRawLength> raw{1, 2, 3, 4, 5, 6};
    CHECK(atm::parseSmartRawValue(raw) ==
          (std::uint64_t{0x060504030201ULL}));
  }

  run("parseSmartRawValue zeros and high byte");
  {
    std::array<std::uint8_t, atm::kSmartRawLength> raw{0, 0, 0, 0, 0, 0};
    CHECK(atm::parseSmartRawValue(raw) == 0);
    raw = {0x00, 0x00, 0x00, 0x00, 0x00, 0x80};
    CHECK(atm::parseSmartRawValue(raw) == 0x800000000000ULL);
  }

  run("formatSmartRawHex renders uppercased zero-padded hex");
  {
    std::array<std::uint8_t, atm::kSmartRawLength> raw{0xAB, 0xCD, 0xEF,
                                                      0x12, 0x34, 0x56};
    CHECK(atm::formatSmartRawHex(raw) == "0xABCDEF123456");
    raw = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    CHECK(atm::formatSmartRawHex(raw) == "0x010000000000");
  }

  run("makeSmartAttribute normalizes raw, flags and names");
  {
    std::array<std::uint8_t, atm::kSmartRawLength> raw{0x0A, 0x00, 0x00,
                                                      0x00, 0x00, 0x00};
    const atm::SmartAttribute attr = atm::makeSmartAttribute(
        5, "Reallocated_Sector_Ct", true, 100, 100, 36, raw, 10, "sectors",
        true, false, true, true);
    CHECK(attr.id == 5);
    CHECK(attr.name == "Reallocated_Sector_Ct");
    CHECK(attr.current.value_or(0) == 100);
    CHECK(attr.worst.value_or(0) == 100);
    CHECK(attr.threshold.value_or(0) == 36);
    CHECK(attr.raw_value == 10);
    CHECK(attr.raw_hex == "0x0A0000000000");
    CHECK(attr.pretty_value.value_or(0) == 10);
    CHECK(attr.unit == "sectors");
    CHECK(!attr.warning);
    CHECK(attr.prefailure);
    CHECK(attr.online);
  }

  run("makeSmartAttribute keeps unknown names and non-normalized values");
  {
    std::array<std::uint8_t, atm::kSmartRawLength> raw{0xFF, 0xFF, 0xFF,
                                                      0xFF, 0xFF, 0xFF};
    const atm::SmartAttribute attr =
        atm::makeSmartAttribute(0xC0, "", false, std::nullopt, std::nullopt,
                                std::nullopt, raw, std::nullopt, "",
                                false, true, false, false);
    CHECK(!attr.name_known);
    CHECK(!attr.current.has_value());
    CHECK(!attr.normalized);
    CHECK(attr.warning);
    CHECK(!attr.prefailure);
    CHECK(attr.raw_value.has_value());
    CHECK(*attr.raw_value == 0xFFFFFFFFFFFFULL);
  }

  // --- NVMe helpers -------------------------------------------------------
  run("nvmeReadU128 reads little-endian 64-bit values");
  {
    std::array<std::uint8_t, 16> field{};
    std::uint64_t value = 0x1122334455667788ULL;
    for (int i = 0; i < 8; ++i) {
      field[i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
    CHECK(atm::nvmeReadU128(field) == value);
  }

  run("nvmeReadU128 rejects values beyond 64 bits");
  {
    std::array<std::uint8_t, 16> field{};
    field[0] = 1;
    field[15] = 1;  // upper 64 bits non-zero
    CHECK(!atm::nvmeReadU128(field).has_value());
  }

  run("nvmeDataUnitsToBytes converts thousands of 512-byte units");
  {
    CHECK(atm::nvmeDataUnitsToBytes(1) == 512000);
    CHECK(atm::nvmeDataUnitsToBytes(0) == 0);
    CHECK(atm::nvmeDataUnitsToBytes(1000) == 512000000ULL);
    // Saturation instead of silent wrap.
    CHECK(atm::nvmeDataUnitsToBytes(UINT64_MAX) == UINT64_MAX);
  }

  run("nvmeKelvinToCelsius conversion");
  {
    CHECK_NEAR(atm::nvmeKelvinToCelsius(303), 29.85, 1e-6);
    CHECK_NEAR(atm::nvmeKelvinToCelsius(273), -0.15, 1e-6);
  }

  run("nvmeCriticalWarnings decodes flag bits");
  {
    CHECK(atm::nvmeCriticalWarnings(0).empty());
    const auto warnings = atm::nvmeCriticalWarnings(0x1F);
    CHECK(warnings.size() == 5);
    CHECK(warnings[0].find("spare") != std::string::npos);
    CHECK(warnings[1].find("temperature") != std::string::npos);
    CHECK(warnings[2].find("Reliability") != std::string::npos);
    // Reserved bit 5 is ignored.
    CHECK(atm::nvmeCriticalWarnings(0x20).empty());
  }

  run("parseNvmeHealthLog decodes a full log page");
  {
    NvmeLogBuilder builder;
    builder.byte(0) = 0x01;      // critical warning: spare low
    builder.u16(1, 303);         // 303 K in LE
    builder.byte(3) = 95;        // available spare 95%
    builder.byte(4) = 10;        // threshold 10%
    builder.byte(5) = 7;         // 7% life used
    builder.u128(32, 100);       // data units read
    builder.u128(48, 250);       // data units written
    builder.u128(64, 12345);     // host reads
    builder.u128(80, 54321);     // host writes
    builder.u128(96, 60);        // busy minutes
    builder.u128(112, 42);       // power cycles
    builder.u128(128, 9000);     // power-on hours
    builder.u128(144, 3);        // unsafe shutdowns
    builder.u128(160, 1);        // media errors
    builder.u128(176, 2);        // error log entries
    builder.u32(192, 5);         // warning temp minutes
    builder.u32(196, 1);         // critical temp minutes

    const atm::NvmeParseResult parsed =
        atm::parseNvmeHealthLog(builder.data(), builder.size());
    CHECK(parsed.valid);
    CHECK(parsed.error == atm::HealthErrorCategory::None);
    CHECK(parsed.log.critical_warning == 0x01);
    CHECK(parsed.log.composite_temperature_celsius.has_value());
    CHECK_NEAR(*parsed.log.composite_temperature_celsius, 29.85, 1e-6);
    CHECK(parsed.log.available_spare == 95);
    CHECK(parsed.log.available_spare_threshold == 10);
    CHECK(parsed.log.percentage_used == 7);
    CHECK(parsed.log.data_units_read == 100);
    CHECK(parsed.log.data_units_written == 250);
    CHECK(parsed.log.host_read_commands == 12345);
    CHECK(parsed.log.host_write_commands == 54321);
    CHECK(parsed.log.controller_busy_time_minutes == 60);
    CHECK(parsed.log.power_cycles == 42);
    CHECK(parsed.log.power_on_hours == 9000);
    CHECK(parsed.log.unsafe_shutdowns == 3);
    CHECK(parsed.log.media_and_data_integrity_errors == 1);
    CHECK(parsed.log.error_information_log_entries == 2);
    CHECK(parsed.log.warning_temperature_time_minutes == 5);
    CHECK(parsed.log.critical_temperature_time_minutes == 1);
  }

  run("parseNvmeHealthLog rejects buffers shorter than the log");
  {
    std::array<std::uint8_t, 8> small{};
    const atm::NvmeParseResult parsed =
        atm::parseNvmeHealthLog(small.data(), small.size());
    CHECK(!parsed.valid);
    CHECK(parsed.error == atm::HealthErrorCategory::ParseError);
  }

  run("parseNvmeHealthLog rejects null data");
  {
    const atm::NvmeParseResult parsed = atm::parseNvmeHealthLog(nullptr, 512);
    CHECK(!parsed.valid);
  }

  run("parseNvmeHealthLog zero temperature is treated as unsupported");
  {
    NvmeLogBuilder builder;
    builder.byte(3) = 99;  // compute presence without temperature
    builder.u128(32, 500);
    const atm::NvmeParseResult parsed =
        atm::parseNvmeHealthLog(builder.data(), builder.size());
    CHECK(parsed.valid);
    CHECK(!parsed.log.composite_temperature_celsius.has_value());
  }

  run("parseNvmeHealthLog caps oversized spare/life percentages");
  {
    NvmeLogBuilder builder;
    builder.byte(3) = 250;  // invalid spare
    builder.byte(4) = 250;  // invalid threshold
    builder.byte(5) = 200;  // invalid life
    builder.u128(32, 500);  // keeps the log otherwise valid
    builder.u128(48, 500);
    const atm::NvmeParseResult parsed =
        atm::parseNvmeHealthLog(builder.data(), builder.size());
    CHECK(parsed.valid);
    CHECK(!parsed.log.available_spare.has_value());
    CHECK(!parsed.log.available_spare_threshold.has_value());
    CHECK(!parsed.log.percentage_used.has_value());
  }

  run("parseNvmeHealthLog 128-bit counters beyond u64 are dropped");
  {
    NvmeLogBuilder builder;
    builder.byte(3) = 99;
    builder.byte(32 + 15) = 1;  // upper words of data_units_read set
    builder.u128(48, 500);
    const atm::NvmeParseResult parsed =
        atm::parseNvmeHealthLog(builder.data(), builder.size());
    CHECK(parsed.valid);
    CHECK(!parsed.log.data_units_read.has_value());
    CHECK(parsed.log.data_units_written == 500);
  }

  run("parseNvmeHealthLog reads an all-zero page as a fresh disk");
  {
    NvmeLogBuilder builder;
    const atm::NvmeParseResult parsed =
        atm::parseNvmeHealthLog(builder.data(), builder.size());
    CHECK(parsed.valid);
    CHECK(parsed.log.critical_warning == 0);
    CHECK(parsed.log.available_spare == 0);
    CHECK(parsed.log.percentage_used == 0);
  }

  // --- Source selection and error mapping -----------------------------------
  run("diskHealthSourceForDevice picks NVMe for nvme names only");
  {
    CHECK(atm::diskHealthSourceForDevice("nvme0n1") ==
          atm::DiskHealthSource::Nvme);
    CHECK(atm::diskHealthSourceForDevice("sda") ==
          atm::DiskHealthSource::AtaSmart);
    CHECK(atm::diskHealthSourceForDevice("zram0") ==
          atm::DiskHealthSource::AtaSmart);
  }

  run("healthStatusForError never maps failures to healthy");
  {
    CHECK(atm::healthStatusForError(atm::HealthErrorCategory::Unsupported) ==
          atm::DiskHealthStatus::Unsupported);
    CHECK(atm::healthStatusForError(
              atm::HealthErrorCategory::PermissionDenied) ==
          atm::DiskHealthStatus::PermissionDenied);
    CHECK(atm::healthStatusForError(
              atm::HealthErrorCategory::DeviceUnavailable) ==
          atm::DiskHealthStatus::Unavailable);
    CHECK(atm::healthStatusForError(atm::HealthErrorCategory::TransportError) ==
          atm::DiskHealthStatus::Unavailable);
  }

  run("status and source display names are stable");
  {
    CHECK(std::string(atm::diskHealthStatusName(atm::DiskHealthStatus::Healthy)) ==
          "Healthy");
    CHECK(std::string(atm::diskHealthStatusName(atm::DiskHealthStatus::Failing)) ==
          "Failing");
    CHECK(std::string(atm::diskHealthSourceName(atm::DiskHealthSource::AtaSmart)) ==
          "ATA/SMART");
    CHECK(std::string(atm::diskHealthSourceName(atm::DiskHealthSource::Nvme)) ==
          "NVMe");
  }

  // --- Device identity and metadata ----------------------------------------
  run("readDeviceIdentity reads major:minor and topology id");
  {
    TestRoot root;
    root.write(root.root / "sys" / "class" / "block" / "sda" / "dev", "8:0\n");
    fs::create_directories(root.root / "sys" / "class" / "block" / "sda");
    // device is a symlink in real sysfs; emulate it.
    fs::create_directories(root.root / "sys" / "devices" / "pci0000:00" /
                           "host0");
    fs::create_symlink("../../../devices/pci0000:00/host0",
                       root.root / "sys" / "class" / "block" / "sda" / "device");

    const std::optional<atm::DeviceIdentity> identity =
        atm::readDeviceIdentity("sda", root.root);
    CHECK(identity.has_value());
    CHECK(identity->major == 8);
    CHECK(identity->minor == 0);
    CHECK(identity->sysfs_id.find("host0") != std::string::npos);
  }

  run("readDeviceIdentity accepts missing topology symlink");
  {
    TestRoot root;
    root.write(root.root / "sys" / "class" / "block" / "sda" / "dev", "8:16\n");
    const std::optional<atm::DeviceIdentity> identity =
        atm::readDeviceIdentity("sda", root.root);
    CHECK(identity.has_value());
    CHECK(identity->major == 8);
    CHECK(identity->minor == 16);
  }

  run("readDeviceIdentity rejects a malformed dev file");
  {
    TestRoot root;
    root.write(root.root / "sys" / "class" / "block" / "sda" / "dev", "garbage\n");
    CHECK(!atm::readDeviceIdentity("sda", root.root).has_value());
  }

  run("readDeviceIdentity returns nullopt when the device vanished");
  {
    TestRoot root;
    CHECK(!atm::readDeviceIdentity("ghost", root.root).has_value());
  }

  run("deviceIdentityUnchanged detects a replaced disk");
  {
    TestRoot root;
    root.write(root.root / "sys" / "class" / "block" / "sda" / "dev", "8:0\n");
    const std::optional<atm::DeviceIdentity> original =
        atm::readDeviceIdentity("sda", root.root);
    CHECK(original.has_value());
    CHECK(atm::deviceIdentityUnchanged("sda", *original, root.root));

    // Disk replaced: dev node now a different minor.
    root.write(root.root / "sys" / "class" / "block" / "sda" / "dev", "8:16\n");
    CHECK(!atm::deviceIdentityUnchanged("sda", *original, root.root));

    // Disk gone entirely.
    fs::remove(root.root / "sys" / "class" / "block" / "sda" / "dev");
    CHECK(!atm::deviceIdentityUnchanged("sda", *original, root.root));
  }

  run("readDeviceMetadata reads model, vendor, revision, serial and sizes");
  {
    TestRoot root;
    const fs::path base = root.root / "sys" / "block" / "sda";
    root.write(base / "device" / "model", "My Model  \n");
    root.write(base / "device" / "vendor", "MyVendor\n");
    root.write(base / "device" / "rev", "ABCD\n");
    root.write(base / "device" / "serial", "SECRETXYZ\n");
    root.write(base / "queue" / "logical_block_size", "512\n");
    root.write(base / "queue" / "physical_block_size", "4096\n");

    const atm::DiskMetadata metadata = atm::readDeviceMetadata("sda", root.root);
    CHECK(metadata.model == "My Model");
    CHECK(metadata.vendor == "MyVendor");
    CHECK(metadata.firmware_revision == "ABCD");
    CHECK(metadata.serial_available);
    CHECK(metadata.logical_block_size == 512);
    CHECK(metadata.physical_block_size == 4096);
  }

  run("readDeviceMetadata absence leaves fields empty and flags serial off");
  {
    TestRoot root;
    const fs::path base = root.root / "sys" / "block" / "sda";
    root.write(base / "device" / "model", "NoSerial\n");
    const atm::DiskMetadata metadata = atm::readDeviceMetadata("sda", root.root);
    CHECK(metadata.model == "NoSerial");
    CHECK(metadata.vendor.empty());
    CHECK(!metadata.serial_available);
    CHECK(metadata.logical_block_size == 0);
  }

  // --- Backoff --------------------------------------------------------------
  run("withinRetryBackoff only throttles unsupported and permission denied");
  {
    const auto now = std::chrono::steady_clock::now();
    CHECK(atm::withinRetryBackoff(atm::HealthErrorCategory::Unsupported,
                                  now, now + std::chrono::seconds(29)));
    CHECK(!atm::withinRetryBackoff(atm::HealthErrorCategory::Unsupported,
                                   now, now + std::chrono::seconds(31)));
    CHECK(atm::withinRetryBackoff(atm::HealthErrorCategory::PermissionDenied,
                                  now, now + std::chrono::seconds(29)));
    CHECK(!atm::withinRetryBackoff(atm::HealthErrorCategory::TransportError,
                                   now, now + std::chrono::seconds(1)));
  }

  // --- Provider factory ------------------------------------------------------
  run("default factory maps names to providers");
  {
    std::unique_ptr<atm::DiskHealthProvider> ata =
        atm::defaultDiskHealthProviderFactory("sda");
    std::unique_ptr<atm::DiskHealthProvider> nvme =
        atm::defaultDiskHealthProviderFactory("nvme0n1");
    CHECK(ata != nullptr);
    CHECK(nvme != nullptr);
    CHECK(std::string(ata->name()) == "ata-smart");
    CHECK(std::string(nvme->name()) == "nvme-health");
  }

  // --- Monitor ---------------------------------------------------------------
  run("monitor refreshes devices into a cache on request");
  {
    auto script_ata = std::make_shared<FakeScript>(
        healthyResult(atm::DiskHealthSource::AtaSmart));
    atm::DiskHealthSnapshot failing =
        healthyResult(atm::DiskHealthSource::Nvme);
    failing.status = atm::DiskHealthStatus::Failing;
    failing.ok = true;
    auto script_nvme = std::make_shared<FakeScript>(std::move(failing));

    atm::DiskHealthMonitor monitor(
        [&](const std::string &name) -> std::unique_ptr<atm::DiskHealthProvider> {
          return name.rfind("nvme", 0) == 0
                     ? std::make_unique<FakeProvider>(script_nvme)
                     : std::make_unique<FakeProvider>(script_ata);
        },
        [](const std::string &name) -> std::optional<atm::DeviceIdentity> {
          if (name == "sata0" || name == "nvme0") {
            atm::DeviceIdentity id;
            id.major = 8;
            id.minor = 0;
            id.sysfs_id = "/fake/" + name;
            return id;
          }
          return std::nullopt;
        },
        [](const std::string &, const atm::DeviceIdentity &) { return true; });

    monitor.setDevices({"nvme0", "sata0"});
    CHECK(monitor.requestRefresh(true));
    monitor.waitForIdle();

    const auto entries = monitor.entries();
    CHECK(entries.size() == 2);
    for (const atm::DiskHealthCacheEntry &entry : entries) {
      CHECK(entry.state.ok);
      CHECK(entry.state.status == atm::DiskHealthStatus::Healthy ||
            entry.state.status == atm::DiskHealthStatus::Failing);
      CHECK(entry.last_success != nullptr);
      CHECK(entry.last_error.empty());
      CHECK(entry.last_error_category == atm::HealthErrorCategory::None);
      CHECK(entry.state.model == "FAKE-DISK");
    }
    CHECK(script_ata->calls == 1);
    CHECK(script_nvme->calls == 1);
    CHECK(monitor.refreshing() == false);
  }

  run("monitor preserves the last success when a refresh fails");
  {
    auto script = std::make_shared<FakeScript>(
        healthyResult(atm::DiskHealthSource::AtaSmart));

    atm::DiskHealthMonitor monitor(
        [&](const std::string &) -> std::unique_ptr<atm::DiskHealthProvider> {
          return std::make_unique<FakeProvider>(script);
        },
        [](const std::string &name) -> std::optional<atm::DeviceIdentity> {
          if (name == "sata0") {
            atm::DeviceIdentity id;
            id.major = 8;
            id.minor = 0;
            id.sysfs_id = "/fake/sata0";
            return id;
          }
          return std::nullopt;
        },
        [](const std::string &, const atm::DeviceIdentity &) { return true; });

    monitor.setDevices({"sata0"});
    CHECK(monitor.requestRefresh(true));
    monitor.waitForIdle();
    {
      const auto entry = monitor.entryFor("sata0");
      CHECK(entry.has_value());
      CHECK(entry->last_success != nullptr);
      CHECK(entry->state.ok);
    }

    // Second refresh now fails (transport error).
    atm::DiskHealthSnapshot failure =
        healthyResult(atm::DiskHealthSource::AtaSmart);
    failure.ok = false;
    failure.error = atm::HealthErrorCategory::TransportError;
    failure.status = atm::DiskHealthStatus::Unavailable;
    failure.detail = "The device did not answer the health request";
    script->result = std::move(failure);

    CHECK(monitor.requestRefresh(true));
    monitor.waitForIdle();
    {
      const auto entry = monitor.entryFor("sata0");
      CHECK(entry.has_value());
      CHECK(entry->state.ok);  // stale success still rendered
      CHECK(entry->last_error_category ==
            atm::HealthErrorCategory::TransportError);
      CHECK(!entry->last_error.empty());
      CHECK(entry->last_success != nullptr);
    }
  }

  run("monitor discards results from a vanished/replaced device");
  {
    auto script = std::make_shared<FakeScript>(
        healthyResult(atm::DiskHealthSource::AtaSmart));

    atm::DiskHealthMonitor monitor(
        [&](const std::string &) -> std::unique_ptr<atm::DiskHealthProvider> {
          return std::make_unique<FakeProvider>(script);
        },
        [](const std::string &name) -> std::optional<atm::DeviceIdentity> {
          if (name == "sata0") {
            atm::DeviceIdentity id;
            id.major = 8;
            id.minor = 0;
            id.sysfs_id = "/fake/sata0";
            return id;
          }
          return std::nullopt;
        },
        [](const std::string &, const atm::DeviceIdentity &) { return false; });

    monitor.setDevices({"sata0"});
    CHECK(monitor.requestRefresh(true));
    monitor.waitForIdle();
    {
      const auto entry = monitor.entryFor("sata0");
      CHECK(entry.has_value());
      CHECK(!entry->state.ok);  // identity changed -> out-of-date result dropped
      CHECK(entry->state.error ==
            atm::HealthErrorCategory::DeviceUnavailable);
      CHECK(entry->last_success == nullptr);
      CHECK(entry->last_error_category ==
            atm::HealthErrorCategory::DeviceUnavailable);
    }
    CHECK(script->calls == 1);
  }

  run("monitor surfaces missing devices immediately");
  {
    auto script = std::make_shared<FakeScript>(
        healthyResult(atm::DiskHealthSource::AtaSmart));
    atm::DiskHealthMonitor monitor(
        [&](const std::string &) -> std::unique_ptr<atm::DiskHealthProvider> {
          return std::make_unique<FakeProvider>(script);
        },
        [](const std::string &) -> std::optional<atm::DeviceIdentity> {
          return std::nullopt;  // identity reader never sees the device
        },
        [](const std::string &, const atm::DeviceIdentity &) { return true; });

    monitor.setDevices({"ghost"});
    CHECK(monitor.requestRefresh(true));
    monitor.waitForIdle();
    const auto entry = monitor.entryFor("ghost");
    CHECK(entry.has_value());
    CHECK(!entry->state.ok);
    CHECK(entry->state.status == atm::DiskHealthStatus::Unavailable);
  }

  run("monitor refuses overlapping refresh requests");
  {
    auto script = std::make_shared<FakeScript>(
        healthyResult(atm::DiskHealthSource::AtaSmart));
    script->delay = std::chrono::milliseconds(150);

    atm::DiskHealthMonitor monitor(
        [&](const std::string &) -> std::unique_ptr<atm::DiskHealthProvider> {
          return std::make_unique<FakeProvider>(script);
        },
        [](const std::string &name) -> std::optional<atm::DeviceIdentity> {
          if (name == "sata0") {
            atm::DeviceIdentity id;
            id.major = 8;
            id.minor = 0;
            id.sysfs_id = "/fake/sata0";
            return id;
          }
          return std::nullopt;
        },
        [](const std::string &, const atm::DeviceIdentity &) { return true; });

    monitor.setDevices({"sata0"});
    CHECK(monitor.requestRefresh(true));
    // Immediately request again while the worker is still reading.
    CHECK(!monitor.requestRefresh(true));
    monitor.waitForIdle();
    CHECK(script->calls == 1);
  }

  run("monitor prunes caches of removed devices");
  {
    auto script = std::make_shared<FakeScript>(
        healthyResult(atm::DiskHealthSource::AtaSmart));
    atm::DiskHealthMonitor monitor(
        [&](const std::string &) -> std::unique_ptr<atm::DiskHealthProvider> {
          return std::make_unique<FakeProvider>(script);
        },
        [](const std::string &name) -> std::optional<atm::DeviceIdentity> {
          if (!name.empty()) {
            atm::DeviceIdentity id;
            id.major = 8;
            id.minor = 0;
            id.sysfs_id = "/fake/" + name;
            return id;
          }
          return std::nullopt;
        },
        [](const std::string &, const atm::DeviceIdentity &) { return true; });

    monitor.setDevices({"sda", "sdb"});
    CHECK(monitor.requestRefresh(true));
    monitor.waitForIdle();
    CHECK(monitor.entries().size() == 2);

    monitor.setDevices({"sda"});
    CHECK(!monitor.entryFor("sdb").has_value());
    CHECK(monitor.entryFor("sda").has_value());
  }

  run("monitor backoff skips recently-unsupported devices on non-forced refresh");
  {
    auto script = std::make_shared<FakeScript>(
        healthyResult(atm::DiskHealthSource::AtaSmart));
    atm::DiskHealthSnapshot unsupported =
        healthyResult(atm::DiskHealthSource::AtaSmart);
    unsupported.ok = false;
    unsupported.error = atm::HealthErrorCategory::Unsupported;
    unsupported.status = atm::DiskHealthStatus::Unsupported;

    atm::DiskHealthMonitor monitor(
        [&](const std::string &) -> std::unique_ptr<atm::DiskHealthProvider> {
          return std::make_unique<FakeProvider>(script);
        },
        [](const std::string &name) -> std::optional<atm::DeviceIdentity> {
          if (!name.empty()) {
            atm::DeviceIdentity id;
            id.major = 8;
            id.minor = 0;
            id.sysfs_id = "/fake/" + name;
            return id;
          }
          return std::nullopt;
        },
        [](const std::string &, const atm::DeviceIdentity &) { return true; });

    monitor.setDevices({"sata0"});
    script->result = unsupported;
    CHECK(monitor.requestRefresh(true));  // forced: reported as unsupported
    monitor.waitForIdle();
    CHECK(script->calls == 1);

    CHECK(monitor.requestRefresh(false));  // non-forced: within backoff
    monitor.waitForIdle();
    CHECK(script->calls == 1);  // not re-attempted during backoff

    CHECK(monitor.requestRefresh(true));  // forced: re-attempted
    monitor.waitForIdle();
    CHECK(script->calls == 2);
  }

  std::fprintf(stderr, "\n%s (%d checks)\n",
               g_failures == 0 ? "PASS" : "FAIL", g_checks);
  return g_failures == 0 ? 0 : 1;
}