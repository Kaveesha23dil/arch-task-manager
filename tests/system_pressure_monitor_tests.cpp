#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "history_manager.hpp"
#include "resource_history.hpp"
#include "system_pressure_monitor.hpp"

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

/// Per-test temp filesystem root mimicking / with a proc/pressure tree.
struct TestRoot {
  fs::path root =
      fs::temp_directory_path() /
      ("arch-task-manager-pressure-" + std::to_string(::getpid()) + "-" +
       std::to_string(reinterpret_cast<std::uintptr_t>(&root)));

  TestRoot() { fs::create_directories(root); }
  ~TestRoot() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }

  fs::path pressureFile(const char *name) const {
    return root / "proc" / "pressure" / name;
  }

  void write(const fs::path &path, const std::string &content) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path);
    file << content;
  }

  void writePressure(const char *name, const std::string &content) {
    write(pressureFile(name), content);
  }
};

int main() {
  // --- PSI line parsing ----------------------------------------------------
  run("parse some line");
  {
    bool is_full = true;
    atm::PressureMetric metric;
    CHECK(atm::parsePressureLine(
        "some avg10=0.00 avg60=0.00 avg300=0.00 total=123456", is_full,
        metric));
    CHECK(!is_full);
    CHECK(metric.line_present);
    CHECK(metric.avg10.has_value() && metric.avg10.value() == 0.0);
    CHECK(metric.avg60.has_value() && metric.avg60.value() == 0.0);
    CHECK(metric.avg300.has_value() && metric.avg300.value() == 0.0);
    CHECK(metric.total.has_value() && metric.total.value() == 123456ULL);
    CHECK(!metric.rate_usec_per_second.has_value());
  }

  run("parse full line");
  {
    bool is_full = false;
    atm::PressureMetric metric;
    CHECK(atm::parsePressureLine(
        "full avg10=2.50 avg60=1.20 avg300=0.90 total=5000", is_full, metric));
    CHECK(is_full);
    CHECK(metric.line_present);
    // Averages are preserved as percentages: 2.50 stays 2.50, never 0.025.
    CHECK_NEAR(metric.avg10.value(), 2.50, 1e-9);
    CHECK_NEAR(metric.avg60.value(), 1.20, 1e-9);
    CHECK_NEAR(metric.avg300.value(), 0.90, 1e-9);
    CHECK(metric.total.has_value() && metric.total.value() == 5000ULL);
  }

  run("parse multiple lines some then full");
  {
    std::istringstream in(
        "some avg10=1.0 avg60=2.0 avg300=3.0 total=111\n"
        "full avg10=4.0 avg60=5.0 avg300=6.0 total=222\n");
    atm::PressureMetric some;
    atm::PressureMetric full;
    CHECK(atm::parsePressureFile(in, some, full) == 2);
    CHECK(some.line_present && full.line_present);
    CHECK_NEAR(some.avg10.value(), 1.0, 1e-9);
    CHECK(some.total.value() == 111ULL);
    CHECK_NEAR(full.avg10.value(), 4.0, 1e-9);
    CHECK(full.total.value() == 222ULL);
  }

  run("parse multiple lines full before some");
  {
    std::istringstream in(
        "full avg10=9.0 total=99\n"
        "some avg10=8.0 total=88\n");
    atm::PressureMetric some;
    atm::PressureMetric full;
    CHECK(atm::parsePressureFile(in, some, full) == 2);
    CHECK(full.line_present && some.line_present);
    CHECK_NEAR(some.avg10.value(), 8.0, 1e-9);
    CHECK_NEAR(full.avg10.value(), 9.0, 1e-9);
    CHECK(some.total.value() == 88ULL);
    CHECK(full.total.value() == 99ULL);
  }

  run("parse fields in any order");
  {
    bool is_full = false;
    atm::PressureMetric metric;
    CHECK(atm::parsePressureLine(
        "some total=777 avg300=3.5 avg10=1.5 avg60=2.5", is_full, metric));
    CHECK(!is_full);
    CHECK_NEAR(metric.avg10.value(), 1.5, 1e-9);
    CHECK_NEAR(metric.avg60.value(), 2.5, 1e-9);
    CHECK_NEAR(metric.avg300.value(), 3.5, 1e-9);
    CHECK(metric.total.value() == 777ULL);
  }

  run("parse missing fields leaves them unavailable");
  {
    bool is_full = false;
    atm::PressureMetric metric;
    CHECK(atm::parsePressureLine("some avg10=1.0", is_full, metric));
    CHECK(metric.line_present);
    CHECK(metric.avg10.has_value());
    CHECK(!metric.avg60.has_value());
    CHECK(!metric.avg300.has_value());
    CHECK(!metric.total.has_value());
  }

  run("parse ignores unknown future keys");
  {
    bool is_full = false;
    atm::PressureMetric metric;
    CHECK(atm::parsePressureLine(
        "some avg10=1.0 avg60=2.0 avg300=3.0 total=4 newkey=99.5 next=5",
        is_full, metric));
    CHECK_NEAR(metric.avg10.value(), 1.0, 1e-9);
    CHECK_NEAR(metric.avg60.value(), 2.0, 1e-9);
    CHECK_NEAR(metric.avg300.value(), 3.0, 1e-9);
    CHECK(metric.total.value() == 4ULL);
  }

  run("parse unknown line type is ignored without crashing");
  {
    bool is_full = false;
    atm::PressureMetric metric;
    CHECK(!atm::parsePressureLine("bogus avg10=1.0 avg60=2.0", is_full,
                                  metric));
    CHECK(!metric.line_present);
    std::istringstream in(
        "some avg10=1.0 total=1\n"
        "future_type avg10=9.0\n"
        "full avg10=2.0 total=2\n");
    atm::PressureMetric some;
    atm::PressureMetric full;
    CHECK(atm::parsePressureFile(in, some, full) == 2);
    CHECK(some.line_present && full.line_present);
    CHECK_NEAR(some.avg10.value(), 1.0, 1e-9);
    CHECK_NEAR(full.avg10.value(), 2.0, 1e-9);
  }

  run("parse empty input and blank lines");
  {
    bool is_full = false;
    atm::PressureMetric metric;
    CHECK(!atm::parsePressureLine("", is_full, metric));
    CHECK(!atm::parsePressureLine("   \t ", is_full, metric));

    std::istringstream empty;
    atm::PressureMetric some;
    atm::PressureMetric full;
    CHECK(atm::parsePressureFile(empty, some, full) == 0);

    std::istringstream blanks("\n\n\n");
    atm::PressureMetric s2;
    atm::PressureMetric f2;
    CHECK(atm::parsePressureFile(blanks, s2, f2) == 0);
    CHECK(!s2.line_present && !f2.line_present);
  }

  run("parse extra whitespace variations");
  {
    bool is_full = false;
    atm::PressureMetric metric;
    CHECK(atm::parsePressureLine(
        "  some   avg10=1.5   avg60=2.5  total=100  \t", is_full, metric));
    CHECK(!is_full);
    CHECK_NEAR(metric.avg10.value(), 1.5, 1e-9);
    CHECK_NEAR(metric.avg60.value(), 2.5, 1e-9);
    CHECK(metric.total.value() == 100ULL);
  }

  run("parse malformed averages leave field unavailable");
  {
    bool is_full = false;
    atm::PressureMetric metric;
    CHECK(atm::parsePressureLine("some avg10=abc avg60=1.2.x avg300=1e",
                                 is_full, metric));
    CHECK(metric.line_present);
    CHECK(!metric.avg10.has_value());
    CHECK(!metric.avg60.has_value());
    CHECK(!metric.avg300.has_value());
  }

  run("parse negative values rejected");
  {
    bool is_full = false;
    atm::PressureMetric metric;
    CHECK(atm::parsePressureLine("some avg10=-1.0 avg60=-0.25 total=4",
                                 is_full, metric));
    CHECK(metric.line_present);
    CHECK(!metric.avg10.has_value());
    CHECK(!metric.avg60.has_value());
    CHECK(metric.total.value() == 4ULL);
  }

  run("parse NaN and infinity rejected");
  {
    bool is_full = false;
    atm::PressureMetric metric;
    CHECK(atm::parsePressureLine(
        "some avg10=nan avg60=inf avg300=infinity total=5", is_full, metric));
    CHECK(metric.line_present);
    CHECK(!metric.avg10.has_value());
    CHECK(!metric.avg60.has_value());
    CHECK(!metric.avg300.has_value());
    CHECK(metric.total.value() == 5ULL);
  }

  run("parse malformed and overflowing totals rejected");
  {
    bool is_full = false;
    atm::PressureMetric metric;
    CHECK(atm::parsePressureLine("some avg10=1.0 total=12x", is_full, metric));
    CHECK(!metric.total.has_value());

    bool is_full2 = false;
    atm::PressureMetric metric2;
    CHECK(atm::parsePressureLine("some avg10=1.0 total=", is_full2, metric2));
    CHECK(!metric2.total.has_value());

    bool is_full3 = false;
    atm::PressureMetric metric3;
    CHECK(atm::parsePressureLine("some avg10=1.0 total=-5", is_full3, metric3));
    CHECK(!metric3.total.has_value());

    bool is_full4 = false;
    atm::PressureMetric metric4;
    CHECK(atm::parsePressureLine(
        "some avg10=1.0 total=18446744073709551615", is_full4, metric4));
    CHECK(metric4.total.has_value() &&
          metric4.total.value() == 18446744073709551615ULL);

    bool is_full5 = false;
    atm::PressureMetric metric5;
    CHECK(atm::parsePressureLine(
        "some avg10=1.0 total=18446744073709551616", is_full5, metric5));
    CHECK(!metric5.total.has_value());
  }

  run("parse duplicate keys keep the last occurrence");
  {
    bool is_full = false;
    atm::PressureMetric metric;
    CHECK(atm::parsePressureLine(
        "some avg10=1.0 avg10=9.0 total=1 total=42", is_full, metric));
    CHECK_NEAR(metric.avg10.value(), 9.0, 1e-9);
    CHECK(metric.total.value() == 42ULL);
  }

  run("parse line without fields stays present");
  {
    bool is_full = true;
    atm::PressureMetric metric;
    CHECK(atm::parsePressureLine("some", is_full, metric));
    CHECK(!is_full);
    CHECK(metric.line_present);
    CHECK(!metric.avg10.has_value() && !metric.total.has_value());
  }

  // --- Availability / file handling ---------------------------------------
  run("all three PSI files available");
  {
    TestRoot root;
    root.writePressure("cpu", "some avg10=1.0 avg60=0.5 avg300=0.2 total=100\n"
                              "full avg10=0.3 avg60=0.1 avg300=0.0 total=20\n");
    root.writePressure("memory",
                       "some avg10=2.0 avg60=1.0 avg300=0.5 total=500\n");
    root.writePressure("io",
                       "some avg10=0.0 avg60=0.0 avg300=0.0 total=0\n"
                       "full avg10=0.0 avg60=0.0 avg300=0.0 total=0\n");

    atm::SystemPressureMonitor monitor(root.root);
    const atm::SystemPressureSnapshot snapshot = monitor.read();
    CHECK(snapshot.anyAvailable());
    CHECK(snapshot.cpu.status == atm::PressureReadStatus::Read);
    CHECK(snapshot.memory.status == atm::PressureReadStatus::Read);
    CHECK(snapshot.io.status == atm::PressureReadStatus::Read);
    CHECK(snapshot.cpu.some.line_present);
    CHECK(snapshot.cpu.full.line_present);
    CHECK(snapshot.memory.some.line_present);
    CHECK(!snapshot.memory.full.line_present);
    CHECK_NEAR(snapshot.cpu.some.avg10.value(), 1.0, 1e-9);
    CHECK_NEAR(snapshot.cpu.full.avg10.value(), 0.3, 1e-9);
    CHECK(snapshot.io.some.total.value() == 0ULL);
    CHECK(snapshot.io.some.avg10.has_value() &&
          snapshot.io.some.avg10.value() == 0.0);
  }

  run("single PSI file missing keeps other categories");
  {
    TestRoot root;
    root.writePressure("cpu", "some avg10=1.0 total=1\n");
    atm::SystemPressureMonitor monitor(root.root);
    const atm::SystemPressureSnapshot snapshot = monitor.read();
    CHECK(snapshot.anyAvailable());
    CHECK(snapshot.cpu.status == atm::PressureReadStatus::Read);
    CHECK(snapshot.memory.status == atm::PressureReadStatus::Unsupported);
    CHECK(snapshot.io.status == atm::PressureReadStatus::Unsupported);
    CHECK(snapshot.hasLine(atm::PressureCategory::Memory, false) == false);
  }

  run("all PSI files missing");
  {
    TestRoot root;  // nothing under proc/pressure
    atm::SystemPressureMonitor monitor(root.root);
    const atm::SystemPressureSnapshot snapshot = monitor.read();
    CHECK(!snapshot.anyAvailable());
    CHECK(snapshot.cpu.status == atm::PressureReadStatus::Unsupported);
    CHECK(snapshot.memory.status == atm::PressureReadStatus::Unsupported);
    CHECK(snapshot.io.status == atm::PressureReadStatus::Unsupported);
  }

  run("partial category data exposes only present lines");
  {
    TestRoot root;
    root.writePressure("cpu", "some avg10=7.5 total=9\n");
    root.writePressure("memory", "full avg10=0.2 total=3\n");
    atm::SystemPressureMonitor monitor(root.root);
    const atm::SystemPressureSnapshot snapshot = monitor.read();
    CHECK(snapshot.cpu.some.line_present);
    CHECK(!snapshot.cpu.full.line_present);
    CHECK(!snapshot.memory.some.line_present);
    CHECK(snapshot.memory.full.line_present);
    // headlineAvg10 prefers some pressure when present, else falls back to full.
    CHECK_NEAR(snapshot.headlineAvg10(atm::PressureCategory::Cpu).value(),
               7.5, 1e-9);
    CHECK_NEAR(snapshot.headlineAvg10(atm::PressureCategory::Memory).value(),
               0.2, 1e-9);
    // No fake full pressure is invented for CPU.
    CHECK(!snapshot.cpu.full.avg10.has_value());
  }

  run("unreadable PSI file reports unreadable");
  {
    if (::geteuid() == 0) {
      std::fprintf(stderr, "  (skipped: running as root)\n");
      ++g_checks;
    } else {
      TestRoot root;
      root.writePressure("cpu", "some avg10=1.0 total=1\n");
      std::error_code ec;
      // Drop the read bit (mode 0200): the file exists but cannot be opened.
      fs::permissions(root.pressureFile("cpu"), fs::perms::owner_write,
                      fs::perm_options::replace, ec);
      if (ec) {
        std::fprintf(stderr, "  (skipped: cannot chmod here)\n");
        ++g_checks;
      } else {
        atm::SystemPressureMonitor monitor(root.root);
        const atm::SystemPressureSnapshot snapshot = monitor.read();
        CHECK(snapshot.cpu.status == atm::PressureReadStatus::Unreadable);
        CHECK(!snapshot.anyAvailable());
        // Restore so the temp dir can be cleaned up.
        fs::permissions(root.pressureFile("cpu"),
                        fs::perms::owner_read | fs::perms::owner_write, ec);
      }
    }
  }

  run("empty PSI file reports empty, not read");
  {
    TestRoot root;
    root.writePressure("cpu", "");
    atm::SystemPressureMonitor monitor(root.root);
    const atm::SystemPressureSnapshot snapshot = monitor.read();
    CHECK(snapshot.cpu.status == atm::PressureReadStatus::Empty);
    CHECK(!snapshot.anyAvailable());
  }

  run("PSI file disappearing during refresh is handled gracefully");
  {
    TestRoot root;
    root.writePressure("cpu",
                       "some avg10=1.0 avg60=0.5 avg300=0.2 total=100\n");
    atm::SystemPressureMonitor monitor(root.root);

    const atm::SystemPressureSnapshot first = monitor.read();
    CHECK(first.cpu.status == atm::PressureReadStatus::Read);
    CHECK(first.cpu.some.total.value() == 100ULL);

    std::error_code ec;
    fs::remove(root.pressureFile("cpu"), ec);

    const atm::SystemPressureSnapshot second = monitor.read();
    CHECK(second.cpu.status == atm::PressureReadStatus::Unsupported);
    CHECK(!second.cpu.some.line_present);
    // No crash, no negative rate: vanished metric simply stays unavailable.
    CHECK(!second.cpu.some.rate_usec_per_second.has_value());
    CHECK(!second.anyAvailable());
  }

  // --- Derived metrics / cumulative totals --------------------------------
  run("rate computed on counter increase");
  {
    TestRoot root;
    root.writePressure("cpu", "some avg10=1.0 avg60=0.5 avg300=0.2 total=100\n");
    atm::SystemPressureMonitor monitor(root.root);

    const atm::SystemPressureSnapshot first = monitor.read();
    CHECK(first.cpu.some.rate_usec_per_second.has_value() == false);

    root.writePressure("cpu",
                       "some avg10=1.0 avg60=0.5 avg300=0.2 total=1100\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const atm::SystemPressureSnapshot second = monitor.read();
    CHECK(second.cpu.some.rate_usec_per_second.has_value());
    CHECK(second.cpu.some.rate_usec_per_second.value() > 0.0);
    CHECK(second.cpu.some.total.value() == 1100ULL);
  }

  run("no negative rate on counter decrease");
  {
    TestRoot root;
    root.writePressure("cpu", "some avg10=1.0 avg60=0.5 avg300=0.2 total=100\n");
    atm::SystemPressureMonitor monitor(root.root);
    static_cast<void>(monitor.read());

    root.writePressure("cpu", "some avg10=1.0 avg60=0.5 avg300=0.2 total=50\n");
    const atm::SystemPressureSnapshot second = monitor.read();
    CHECK(second.cpu.some.total.value() == 50ULL);
    CHECK(!second.cpu.some.rate_usec_per_second.has_value());
  }

  run("counter reset re-anchors baseline");
  {
    TestRoot root;
    root.writePressure("cpu", "some avg10=1.0 avg60=0.5 avg300=0.2 total=100\n");
    atm::SystemPressureMonitor monitor(root.root);
    static_cast<void>(monitor.read());

    // Counter reset to 40, then a small increase to 50. The delta must be
    // measured from the re-anchored baseline (50-40), never an artefact of the
    // pre-reset value.
    root.writePressure("cpu", "some avg10=1.0 avg60=0.5 avg300=0.2 total=40\n");
    static_cast<void>(monitor.read());

    root.writePressure("cpu", "some avg10=1.0 avg60=0.5 avg300=0.2 total=50\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const atm::SystemPressureSnapshot third = monitor.read();
    CHECK(third.cpu.some.rate_usec_per_second.has_value());
    CHECK(third.cpu.some.rate_usec_per_second.value() > 0.0);
  }

  run("missing full pressure is not fabricated");
  {
    TestRoot root;
    root.writePressure("cpu", "some avg10=2.50 avg60=1.0 avg300=0.5 total=9\n");
    atm::SystemPressureMonitor monitor(root.root);
    const atm::SystemPressureSnapshot snapshot = monitor.read();
    CHECK(!snapshot.cpu.full.line_present);
    CHECK(!snapshot.cpu.full.avg10.has_value());
    CHECK(!snapshot.cpu.full.total.has_value());
    CHECK(snapshot.severity(atm::PressureCategory::Cpu).has_value());
    CHECK(snapshot.severity(atm::PressureCategory::Io) == std::nullopt);
  }

  // --- Severity classification --------------------------------------------
  run("severity classification thresholds");
  {
    CHECK(atm::classifyPressureSeverity(0.0) == atm::PressureSeverity::Normal);
    CHECK(atm::classifyPressureSeverity(9.99) == atm::PressureSeverity::Normal);
    CHECK(atm::classifyPressureSeverity(10.0) == atm::PressureSeverity::Elevated);
    CHECK(atm::classifyPressureSeverity(29.99) == atm::PressureSeverity::Elevated);
    CHECK(atm::classifyPressureSeverity(30.0) == atm::PressureSeverity::High);
    CHECK(atm::classifyPressureSeverity(49.99) == atm::PressureSeverity::High);
    CHECK(atm::classifyPressureSeverity(50.0) == atm::PressureSeverity::Critical);
    CHECK(atm::classifyPressureSeverity(99.99) == atm::PressureSeverity::Critical);
    CHECK(atm::classifyPressureSeverity(-1.0) == atm::PressureSeverity::Normal);
  }

  // --- History integration -------------------------------------------------
  run("history pressure records one sample per available metric");
  {
    atm::HistoryManager history;
    atm::PressureMetrics metrics;
    metrics.cpu_some = 1.0;
    metrics.cpu_full = 0.5;
    metrics.memory_some = 2.0;
    metrics.memory_full = 1.5;
    metrics.io_some = 0.0;
    metrics.io_full = 0.0;
    history.updatePressure(metrics);
    CHECK(history.cpuPressureSomeHistory().size() == 1);
    CHECK(history.cpuPressureFullHistory().size() == 1);
    CHECK(history.memoryPressureSomeHistory().size() == 1);
    CHECK(history.memoryPressureFullHistory().size() == 1);
    CHECK(history.ioPressureSomeHistory().size() == 1);
    CHECK(history.ioPressureFullHistory().size() == 1);
    CHECK_NEAR(history.cpuPressureSomeHistory().samples().back().value, 1.0,
               1e-9);
    // A present zero is recorded, not treated as missing.
    CHECK(history.ioPressureSomeHistory().samples().back().value == 0.0);
  }

  run("history pressure skips unavailable metrics");
  {
    atm::HistoryManager history;
    atm::PressureMetrics partial;
    partial.cpu_some = 5.0;  // only this metric available
    history.updatePressure(partial);
    CHECK(history.cpuPressureSomeHistory().size() == 1);
    CHECK(history.cpuPressureFullHistory().empty());
    CHECK(history.memoryPressureSomeHistory().empty());
    CHECK(history.memoryPressureFullHistory().empty());
    CHECK(history.ioPressureSomeHistory().empty());
    CHECK(history.ioPressureFullHistory().empty());
  }

  run("history pressure empty update contributes no samples");
  {
    atm::HistoryManager history;
    atm::PressureMetrics none;
    history.updatePressure(none);
    CHECK(history.cpuPressureSomeHistory().empty());
    CHECK(history.ioPressureFullHistory().empty());
  }

  run("history pressure respects pause");
  {
    atm::HistoryManager history;
    history.setPaused(true);
    atm::PressureMetrics metrics;
    metrics.cpu_some = 3.0;
    metrics.io_some = 4.0;
    history.updatePressure(metrics);
    history.updatePressure(metrics);
    CHECK(history.cpuPressureSomeHistory().empty());
    CHECK(history.ioPressureSomeHistory().empty());

    history.setPaused(false);
    history.updatePressure(metrics);
    CHECK(history.cpuPressureSomeHistory().size() == 1);
  }

  run("history pressure is bounded by max samples");
  {
    atm::HistoryManager history(4);
    atm::PressureMetrics metrics;
    metrics.cpu_some = 1.0;
    metrics.io_some = 2.0;
    for (int i = 0; i < 10; ++i) {
      history.updatePressure(metrics);
    }
    CHECK(history.cpuPressureSomeHistory().size() == 4);
    CHECK(history.ioPressureSomeHistory().size() == 4);
  }

  run("history pressure has no duplicate samples per refresh");
  {
    atm::HistoryManager history;
    atm::PressureMetrics metrics;
    metrics.cpu_some = 42.0;
    metrics.memory_some = 12.0;
    history.updatePressure(metrics);
    CHECK(history.cpuPressureSomeHistory().size() == 1);
    CHECK(history.memoryPressureSomeHistory().size() == 1);
  }

  run("history pressure handles unavailable metric transitions");
  {
    atm::HistoryManager history;
    atm::PressureMetrics with_cpu;
    with_cpu.cpu_some = 1.0;
    with_cpu.memory_some = 2.0;
    history.updatePressure(with_cpu);

    // cpu_some disappears; memory continues to be recorded.
    atm::PressureMetrics without_cpu;
    without_cpu.memory_some = 2.5;
    history.updatePressure(without_cpu);

    CHECK(history.cpuPressureSomeHistory().size() == 1);
    CHECK(history.memoryPressureSomeHistory().size() == 2);
    CHECK_NEAR(history.memoryPressureSomeHistory().samples().back().value, 2.5,
               1e-9);
  }

  run("history pressure timestamps are consistent within a refresh");
  {
    atm::HistoryManager history;
    atm::PressureMetrics metrics;
    metrics.cpu_some = 1.0;
    metrics.cpu_full = 0.5;
    metrics.memory_some = 2.0;
    metrics.memory_full = 1.5;
    metrics.io_some = 0.3;
    metrics.io_full = 0.2;
    history.updatePressure(metrics);
    const auto init = std::chrono::steady_clock::now();
    atm::PressureMetrics next;
    next.cpu_some = 2.0;
    next.cpu_full = 1.0;
    next.memory_some = 3.0;
    next.memory_full = 2.0;
    next.io_some = 0.5;
    next.io_full = 0.4;
    history.updatePressure(next);

    const auto &cpu_some = history.cpuPressureSomeHistory().samples();
    const auto &cpu_full = history.cpuPressureFullHistory().samples();
    const auto &mem_some = history.memoryPressureSomeHistory().samples();
    const auto &mem_full = history.memoryPressureFullHistory().samples();
    const auto &io_some = history.ioPressureSomeHistory().samples();
    const auto &io_full = history.ioPressureFullHistory().samples();

    CHECK(cpu_some.size() == 2 && cpu_full.size() == 2 && mem_some.size() == 2 &&
          mem_full.size() == 2 && io_some.size() == 2 && io_full.size() == 2);
    // One clock read is shared across the batch; samples from the same refresh
    // carry identical timestamps.
    CHECK(cpu_some.back().timestamp == cpu_full.back().timestamp);
    CHECK(cpu_full.back().timestamp == mem_some.back().timestamp);
    CHECK(mem_some.back().timestamp == mem_full.back().timestamp);
    CHECK(mem_full.back().timestamp == io_some.back().timestamp);
    CHECK(io_some.back().timestamp == io_full.back().timestamp);
    CHECK(cpu_some.front().timestamp == io_full.front().timestamp);
    CHECK(cpu_some.back().timestamp >= init);
    CHECK(cpu_some.back().timestamp >= cpu_some.front().timestamp);
  }

  run("history pressure survives clearAll");
  {
    atm::HistoryManager history;
    atm::PressureMetrics metrics;
    metrics.cpu_some = 5.0;
    metrics.io_some = 6.0;
    history.updatePressure(metrics);
    history.clearAll();
    CHECK(history.cpuPressureSomeHistory().empty());
    CHECK(history.cpuPressureFullHistory().empty());
    CHECK(history.memoryPressureSomeHistory().empty());
    CHECK(history.ioPressureFullHistory().empty());
  }

  std::fprintf(stderr, "\n%s (%d checks)\n",
               g_failures == 0 ? "PASS" : "FAIL", g_checks);
  return g_failures == 0 ? 0 : 1;
}