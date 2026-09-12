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
#include <sys/stat.h>

#include "history_manager.hpp"
#include "resource_history.hpp"
#include "system_load_monitor.hpp"

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

/// Per-test temp filesystem root mimicking / with proc/loadavg and proc/uptime.
struct TestRoot {
  fs::path root =
      fs::temp_directory_path() /
      ("arch-task-manager-load-" + std::to_string(::getpid()) + "-" +
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

  void writeLoadAvg(const std::string &content) {
    write(root / "proc" / "loadavg", content);
  }

  void writeUptime(const std::string &content) {
    write(root / "proc" / "uptime", content);
  }
};

int main() {
  // --- /proc/loadavg parsing ----------------------------------------------
  run("parse standard loadavg line");
  {
    atm::LoadAverageSnapshot snap;
    CHECK(atm::parseLoadAverage("3.36 2.98 2.51 7/1023 19041\n", snap));
    CHECK(snap.readable);
    CHECK_NEAR(snap.load1, 3.36, 1e-9);
    CHECK_NEAR(snap.load5, 2.98, 1e-9);
    CHECK_NEAR(snap.load15, 2.51, 1e-9);
    CHECK(snap.running_total_available);
    CHECK_NEAR(snap.running, 7.0, 1e-9);
    CHECK_NEAR(snap.total, 1023.0, 1e-9);
    CHECK(snap.last_pid_available);
    CHECK(snap.last_pid == 19041ULL);
  }

  run("parse zero loadavg line");
  {
    atm::LoadAverageSnapshot snap;
    CHECK(atm::parseLoadAverage("0.00 0.00 0.00 1/102\n", snap));
    CHECK(snap.readable);
    // A present zero is preserved, not treated as missing.
    CHECK_NEAR(snap.load1, 0.0, 1e-9);
    CHECK_NEAR(snap.total, 102.0, 1e-9);
    CHECK(snap.last_pid_available == false);
  }

  run("parse loadavg without trailing fields");
  {
    atm::LoadAverageSnapshot snap;
    CHECK(atm::parseLoadAverage("1.00 0.80 0.60\n", snap));
    CHECK(snap.readable);
    CHECK_NEAR(snap.load1, 1.0, 1e-9);
    CHECK(!snap.running_total_available);
    CHECK(!snap.last_pid_available);
  }

  run("parse loadavg with only running/total, no last pid");
  {
    atm::LoadAverageSnapshot snap;
    CHECK(atm::parseLoadAverage("0.50 0.40 0.30 3/99\n", snap));
    CHECK(snap.readable);
    CHECK(snap.running_total_available);
    CHECK_NEAR(snap.running, 3.0, 1e-9);
    CHECK(!snap.last_pid_available);
  }

  run("malformed running/total leaves the field unavailable");
  {
    atm::LoadAverageSnapshot snap;
    CHECK(atm::parseLoadAverage("1.00 0.90 0.80 3/99x 12\n", snap));
    // 3/99x is not strictly "unsigned/slash/unsigned".
    CHECK(!snap.running_total_available);
    CHECK_NEAR(snap.load1, 1.0, 1e-9);
    // The rest of the line still parses.
    CHECK(snap.last_pid_available);
    CHECK(snap.last_pid == 12ULL);
  }

  run("malformed last pid leaves it unavailable");
  {
    atm::LoadAverageSnapshot snap;
    CHECK(atm::parseLoadAverage("1.00 0.90 0.80 3/99 abc\n", snap));
    CHECK(snap.running_total_available);
    CHECK(!snap.last_pid_available);
  }

  run("negative and NaN load averages are rejected");
  {
    atm::LoadAverageSnapshot snap;
    CHECK(!atm::parseLoadAverage("-1.00 0.90 0.80 3/99\n", snap));
    CHECK(!atm::parseLoadAverage("nan 0.90 0.80 3/99\n", snap));
    CHECK(!atm::parseLoadAverage("inf 0.90 0.80 3/99\n", snap));
  }

  run("empty and truncated loadavg lines are rejected");
  {
    atm::LoadAverageSnapshot snap;
    CHECK(!atm::parseLoadAverage("", snap));
    CHECK(!atm::parseLoadAverage("   \n", snap));
    CHECK(!atm::parseLoadAverage("1.00 0.90\n", snap));
    CHECK(!atm::parseLoadAverage("1.00 0.90 0.80x 3/99\n", snap));
  }

  run("extra trailing loadavg fields are ignored");
  {
    atm::LoadAverageSnapshot snap;
    CHECK(atm::parseLoadAverage("1.00 0.90 0.80 3/99 123 456 extra\n", snap));
    CHECK(snap.last_pid_available);
    CHECK(snap.last_pid == 123ULL);
  }

  // --- /proc/uptime parsing ------------------------------------------------
  run("parse standard uptime line");
  {
    atm::UptimeSnapshot snap;
    CHECK(atm::parseUptime("1625962.72 4750871.82\n", snap));
    CHECK(snap.readable);
    CHECK_NEAR(snap.uptime_seconds, 1625962.72, 1e-6);
    CHECK_NEAR(snap.idle_seconds, 4750871.82, 1e-6);
  }

  run("uptime idle may exceed uptime (SMP)");
  {
    atm::UptimeSnapshot snap;
    CHECK(atm::parseUptime("1000.50 8000.25\n", snap));
    CHECK_NEAR(snap.idle_seconds, 8000.25, 1e-6);
  }

  run("uptime without idle field still parses");
  {
    atm::UptimeSnapshot snap;
    CHECK(atm::parseUptime("1234.5\n", snap));
    CHECK(snap.readable);
    CHECK_NEAR(snap.uptime_seconds, 1234.5, 1e-6);
    CHECK_NEAR(snap.idle_seconds, 0.0, 1e-9);
  }

  run("invalid uptime values are rejected");
  {
    atm::UptimeSnapshot snap;
    CHECK(!atm::parseUptime("", snap));
    CHECK(!atm::parseUptime("abc 123\n", snap));
    CHECK(!atm::parseUptime("-5.0 100\n", snap));
    CHECK(!atm::parseUptime("nan 100\n", snap));
  }

  // --- Normalization and severity ------------------------------------------
  run("normalizeLoad divides by online CPUs");
  {
    const std::optional<double> n1 = atm::normalizeLoad(3.36, 8);
    CHECK(n1.has_value());
    CHECK_NEAR(*n1, 0.42, 1e-9);
    const std::optional<double> n2 = atm::normalizeLoad(8.0, 2);
    CHECK(n2.has_value());
    CHECK_NEAR(*n2, 4.0, 1e-9);
    const std::optional<double> n3 = atm::normalizeLoad(0.0, 4);
    CHECK(n3.has_value() && *n3 == 0.0);
  }

  run("normalizeLoad with unknown CPU count yields nullopt");
  {
    CHECK(!atm::normalizeLoad(3.36, 0).has_value());
  }

  run("load severity classification thresholds");
  {
    CHECK(atm::classifyLoadSeverity(0.0) == atm::LoadSeverity::Normal);
    CHECK(atm::classifyLoadSeverity(0.69) == atm::LoadSeverity::Normal);
    CHECK(atm::classifyLoadSeverity(0.70) == atm::LoadSeverity::Elevated);
    CHECK(atm::classifyLoadSeverity(0.99) == atm::LoadSeverity::Elevated);
    CHECK(atm::classifyLoadSeverity(1.00) == atm::LoadSeverity::High);
    CHECK(atm::classifyLoadSeverity(1.99) == atm::LoadSeverity::High);
    CHECK(atm::classifyLoadSeverity(2.00) == atm::LoadSeverity::Critical);
    CHECK(atm::classifyLoadSeverity(10.0) == atm::LoadSeverity::Critical);
    CHECK(atm::classifyLoadSeverity(-1.0) == atm::LoadSeverity::Normal);
  }

  run("load severity display names");
  {
    CHECK(std::string(atm::loadSeverityName(atm::LoadSeverity::Normal)) ==
          "NORMAL");
    CHECK(std::string(atm::loadSeverityName(atm::LoadSeverity::Elevated)) ==
          "ELEVATED");
    CHECK(std::string(atm::loadSeverityName(atm::LoadSeverity::High)) ==
          "HIGH");
    CHECK(std::string(atm::loadSeverityName(atm::LoadSeverity::Critical)) ==
          "CRITICAL");
  }

  // --- Monitor reads --------------------------------------------------------
  run("monitor reads load average and uptime from synthetic root");
  {
    TestRoot root;
    root.writeLoadAvg("3.36 2.98 2.51 7/1023 19041\n");
    root.writeUptime("1000.5 8000.25\n");
    atm::SystemLoadMonitor monitor(root.root);
    const atm::SystemLoadSnapshot snap = monitor.read();

    CHECK(snap.load.readable);
    CHECK_NEAR(snap.load.load1, 3.36, 1e-9);
    CHECK_NEAR(snap.load.load15, 2.51, 1e-9);
    CHECK(snap.load.running_total_available);
    CHECK(snap.load.last_pid_available);
    CHECK(snap.load.last_pid == 19041ULL);

    CHECK(snap.uptime.readable);
    CHECK_NEAR(snap.uptime.uptime_seconds, 1000.5, 1e-6);
    CHECK_NEAR(snap.uptime.idle_seconds, 8000.25, 1e-6);
  }

  run("boot time derived from uptime and current clock");
  {
    TestRoot root;
    root.writeLoadAvg("0.10 0.10 0.10 1/10\n");
    root.writeUptime("3600.0 7200.0\n");
    atm::SystemLoadMonitor monitor(root.root);
    const auto before = std::chrono::system_clock::now();
    const atm::SystemLoadSnapshot snap = monitor.read();
    const auto after = std::chrono::system_clock::now();
    CHECK(snap.boot_time.has_value());
    // boot time == now - uptime, within the time the read itself took.
    const auto expected_low = before -
        std::chrono::duration<double>(3600.0) -
        std::chrono::seconds(2);
    const auto expected_high = after -
        std::chrono::duration<double>(3600.0) +
        std::chrono::seconds(2);
    CHECK(*snap.boot_time >= expected_low);
    CHECK(*snap.boot_time <= expected_high);
  }

  run("missing loadavg leaves load unavailable but uptime intact");
  {
    TestRoot root;
    root.writeUptime("100.0 200.0\n");
    atm::SystemLoadMonitor monitor(root.root);
    const atm::SystemLoadSnapshot snap = monitor.read();
    CHECK(!snap.load.readable);
    CHECK(snap.uptime.readable);
    CHECK(snap.boot_time.has_value());
  }

  run("missing uptime leaves uptime and boot time unavailable");
  {
    TestRoot root;
    root.writeLoadAvg("0.10 0.10 0.10 1/10\n");
    atm::SystemLoadMonitor monitor(root.root);
    const atm::SystemLoadSnapshot snap = monitor.read();
    CHECK(snap.load.readable);
    CHECK(!snap.uptime.readable);
    CHECK(!snap.boot_time.has_value());
  }

  run("neither file present yields a harmless empty snapshot");
  {
    TestRoot root;
    atm::SystemLoadMonitor monitor(root.root);
    const atm::SystemLoadSnapshot snap = monitor.read();
    CHECK(!snap.load.readable);
    CHECK(!snap.uptime.readable);
    CHECK(!snap.boot_time.has_value());
  }

  // --- History integration -------------------------------------------------
  run("history load records raw and normalized samples");
  {
    atm::HistoryManager history;
    atm::LoadMetrics metrics;
    metrics.load1 = 3.36;
    metrics.load5 = 2.98;
    metrics.load15 = 2.51;
    metrics.normalized_load1 = 0.42;
    metrics.normalized_load5 = 0.37;
    metrics.normalized_load15 = 0.31;
    history.updateLoad(metrics);
    CHECK(history.load1History().size() == 1);
    CHECK(history.load5History().size() == 1);
    CHECK(history.load15History().size() == 1);
    CHECK(history.normalizedLoad1History().size() == 1);
    CHECK(history.normalizedLoad5History().size() == 1);
    CHECK(history.normalizedLoad15History().size() == 1);
    CHECK_NEAR(history.load1History().samples().back().value, 3.36, 1e-9);
    CHECK_NEAR(history.normalizedLoad1History().samples().back().value, 0.42,
               1e-9);
  }

  run("history load skips unavailable metrics");
  {
    atm::HistoryManager history;
    atm::LoadMetrics partial;
    partial.load1 = 1.0;  // only this metric available
    partial.normalized_load15 = 4.0;
    history.updateLoad(partial);
    CHECK(history.load1History().size() == 1);
    CHECK(history.load5History().empty());
    CHECK(history.load15History().empty());
    CHECK(history.normalizedLoad1History().empty());
    CHECK(history.normalizedLoad15History().size() == 1);
  }

  run("history load empty update contributes no samples");
  {
    atm::HistoryManager history;
    atm::LoadMetrics none;
    history.updateLoad(none);
    CHECK(history.load1History().empty());
    CHECK(history.normalizedLoad5History().empty());
  }

  run("history load respects pause");
  {
    atm::HistoryManager history;
    history.setPaused(true);
    atm::LoadMetrics metrics;
    metrics.load1 = 2.0;
    metrics.normalized_load1 = 1.0;
    history.updateLoad(metrics);
    CHECK(history.load1History().empty());

    history.setPaused(false);
    history.updateLoad(metrics);
    CHECK(history.load1History().size() == 1);
  }

  run("history load is bounded by max samples");
  {
    atm::HistoryManager history(4);
    atm::LoadMetrics metrics;
    metrics.load1 = 1.0;
    metrics.normalized_load1 = 2.0;
    for (int i = 0; i < 10; ++i) {
      history.updateLoad(metrics);
    }
    CHECK(history.load1History().size() == 4);
    CHECK(history.normalizedLoad1History().size() == 4);
  }

  run("history load timestamps are consistent within a refresh");
  {
    atm::HistoryManager history;
    atm::LoadMetrics metrics;
    metrics.load1 = 1.0;
    metrics.load5 = 2.0;
    metrics.load15 = 3.0;
    metrics.normalized_load1 = 0.1;
    metrics.normalized_load5 = 0.2;
    metrics.normalized_load15 = 0.3;
    history.updateLoad(metrics);
    const auto init = std::chrono::steady_clock::now();
    atm::LoadMetrics next;
    next.load1 = 2.0;
    next.load5 = 2.5;
    next.load15 = 3.5;
    next.normalized_load5 = 0.4;
    history.updateLoad(next);

    const auto &load1 = history.load1History().samples();
    const auto &load5 = history.load5History().samples();
    const auto &load15 = history.load15History().samples();
    const auto &n1 = history.normalizedLoad1History().samples();
    CHECK(load1.size() == 2 && load5.size() == 2 && load15.size() == 2);
    CHECK(n1.size() == 1);
    // All samples of one refresh are timestamped with a single clock read.
    CHECK(load1.front().timestamp == load15.front().timestamp);
    CHECK(load1.back().timestamp == load15.back().timestamp);
    CHECK(load5.back().timestamp == load15.back().timestamp);
    CHECK(load1.back().timestamp >= init);
    CHECK(load1.back().timestamp >= load1.front().timestamp);
  }

  run("history load survives clearAll");
  {
    atm::HistoryManager history;
    atm::LoadMetrics metrics;
    metrics.load1 = 5.0;
    metrics.normalized_load1 = 1.0;
    history.updateLoad(metrics);
    history.clearAll();
    CHECK(history.load1History().empty());
    CHECK(history.load15History().empty());
    CHECK(history.normalizedLoad1History().empty());
  }

  std::fprintf(stderr, "\n%s (%d checks)\n",
               g_failures == 0 ? "PASS" : "FAIL", g_checks);
  return g_failures == 0 ? 0 : 1;
}