#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "advanced_cpu_monitor.hpp"
#include "history_manager.hpp"
#include "resource_history.hpp"

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

/// Per-test temp filesystem root mimicking / with a proc/stat + sysfs tree.
struct TestRoot {
  fs::path root =
      fs::temp_directory_path() /
      ("arch-task-manager-cpu-" + std::to_string(::getpid()) + "-" +
       std::to_string(reinterpret_cast<std::uintptr_t>(&root)));

  TestRoot() { fs::create_directories(root); }
  ~TestRoot() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }

  fs::path procStat() const { return root / "proc" / "stat"; }
  fs::path sysCpu() const { return root / "sys" / "devices" / "system" / "cpu"; }

  void write(const fs::path &path, const std::string &content) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path);
    file << content;
  }

  void writeProcStat(const std::string &content) { write(procStat(), content); }

  void writeTopology(const std::string &online, const std::string &possible,
                     const std::string &present) {
    const fs::path dir = sysCpu();
    if (!fs::exists(dir)) {
      fs::create_directories(dir);
    }
    write(dir / "online", online + "\n");
    write(dir / "possible", possible + "\n");
    write(dir / "present", present + "\n");
  }

  void writeCpufreq(int cpu_id, const std::string &current,
                    const std::string &min, const std::string &max,
                    const std::string &governor) {
    const fs::path dir = sysCpu() / ("cpu" + std::to_string(cpu_id)) / "cpufreq";
    write(dir / "scaling_cur_freq", current + "\n");
    write(dir / "scaling_min_freq", min + "\n");
    write(dir / "scaling_max_freq", max + "\n");
    write(dir / "scaling_governor", governor + "\n");
  }
};

atm::CpuCounterSnapshot makeCounter(std::uint64_t user, std::uint64_t nice,
                                    std::uint64_t system, std::uint64_t idle,
                                    std::uint64_t iowait = 0,
                                    std::uint64_t irq = 0,
                                    std::uint64_t softirq = 0,
                                    std::uint64_t steal = 0,
                                    std::uint64_t guest = 0,
                                    std::uint64_t guest_nice = 0) {
  atm::CpuCounterSnapshot c;
  c.user = user;
  c.nice = nice;
  c.system = system;
  c.idle = idle;
  c.iowait = iowait;
  c.irq = irq;
  c.softirq = softirq;
  c.steal = steal;
  c.guest = guest;
  c.guest_nice = guest_nice;
  return c;
}

std::string cpuLine(int cpu_id, const atm::CpuCounterSnapshot &c) {
  std::ostringstream out;
  out << (cpu_id < 0 ? "cpu " : "cpu" + std::to_string(cpu_id)) << ' ' << c.user
      << ' ' << c.nice << ' ' << c.system << ' ' << c.idle << ' ' << c.iowait
      << ' ' << c.irq << ' ' << c.softirq << ' ' << c.steal << ' ' << c.guest
      << ' ' << c.guest_nice << '\n';
  return out.str();
}

int main() {
  // --- /proc/stat parsing ---------------------------------------------------

  run("parse aggregate cpu line");
  {
    atm::CpuCounterSnapshot c;
    CHECK(atm::parseCpuCounterLine(
        "cpu  100 2 300 400 50 6 7 8 9 10", c));
    CHECK(c.cpu_id == -1);
    CHECK(c.user == 100);
    CHECK(c.nice == 2);
    CHECK(c.system == 300);
    CHECK(c.idle == 400);
    CHECK(c.iowait == 50);
    CHECK(c.irq == 6);
    CHECK(c.softirq == 7);
    CHECK(c.steal == 8);
    CHECK(c.guest == 9);
    CHECK(c.guest_nice == 10);
  }

  run("parse per-cpu line");
  {
    atm::CpuCounterSnapshot c;
    CHECK(atm::parseCpuCounterLine(
        "cpu7 10 20 30 40 50 60 70 80 90 100", c));
    CHECK(c.cpu_id == 7);
    CHECK(c.user == 10 && c.steal == 80 && c.guest_nice == 100);
  }

  run("cpu id with multiple digits");
  {
    atm::CpuCounterSnapshot c;
    CHECK(atm::parseCpuCounterLine(
        "cpu128 1 2 3 4 5 6 7 8 9 10", c));
    CHECK(c.cpu_id == 128);
  }

  run("missing optional fields");
  {
    // Only the four mandatory fields: the optional ones default to 0.
    atm::CpuCounterSnapshot c;
    CHECK(atm::parseCpuCounterLine("cpu0 1 2 3 4", c));
    CHECK(c.cpu_id == 0);
    CHECK(c.user == 1 && c.nice == 2 && c.system == 3 && c.idle == 4);
    CHECK(c.iowait == 0 && c.irq == 0 && c.softirq == 0 && c.steal == 0);
    CHECK(c.guest == 0 && c.guest_nice == 0);
  }

  run("extra fields beyond guest_nice are ignored");
  {
    atm::CpuCounterSnapshot c;
    CHECK(atm::parseCpuCounterLine(
        "cpu 1 2 3 4 5 6 7 8 9 10 999 888 777", c));
    CHECK(c.cpu_id == -1);
    CHECK(c.user == 1 && c.guest_nice == 10);
  }

  run("unrelated and malformed lines are rejected");
  {
    atm::CpuCounterSnapshot c;
    CHECK(!atm::parseCpuCounterLine("intr 123 4", c));
    CHECK(!atm::parseCpuCounterLine("", c));
    CHECK(!atm::parseCpuCounterLine("cpu", c));          // bare "cpu"
    CHECK(!atm::parseCpuCounterLine("cpuq 1 2 3 4", c));  // non-numeric id
    CHECK(!atm::parseCpuCounterLine("cpu0 1 abc 3 4", c));  // non-numeric field
    CHECK(!atm::parseCpuCounterLine("cpu0 1 2 3", c));   // fewer than 4 fields
    CHECK(!atm::parseCpuCounterLine("cpu0 -1 2 3 4", c));  // negative value
    CHECK(!atm::parseCpuCounterLine(
        "cpu0 18446744073709551616 0 0 0", c));  // overflows uint64
  }

  run("parseProcStat handles aggregate, per-cpu and unrelated lines");
  {
    std::istringstream in(
        "cpu  100 0 0 900 0 0 0 0 0 0\n"
        "cpu0 10 0 0 90 0 0 0 0 0 0\n"
        "cpu12 20 0 0 80 0 0 0 0 0 0\n"
        "intr 123 456\n"
        "ctxt 42\n"
        "btime 1600000000\n"
        "processes 7\n"
        "procs_running 2\n"
        "softirq 1 2 3\n");
    atm::ProcStatSnapshot snap;
    CHECK(atm::parseProcStat(in, snap));
    CHECK(snap.has_aggregate);
    CHECK(snap.counters.size() == 3);  // -1, 0, 12
    CHECK(snap.counters.count(-1) == 1);
    CHECK(snap.counters.count(0) == 1);
    CHECK(snap.counters.count(12) == 1);
    CHECK(snap.counters.at(-1).user == 100);
    CHECK(snap.counters.at(12).nice == 0);
  }

  run("empty proc/stat input yields no counters");
  {
    std::istringstream in("");
    atm::ProcStatSnapshot snap;
    CHECK(atm::parseProcStat(in, snap));
    CHECK(!snap.has_aggregate);
    CHECK(snap.counters.empty());
  }

  // --- Utilization calculations ---------------------------------------------

  run("normal delta breakdown");
  {
    const atm::CpuCounterSnapshot prev =
        makeCounter(100, 0, 50, 800, 0, 10, 20, 0);   // total 980
    const atm::CpuCounterSnapshot cur =
        makeCounter(200, 0, 100, 850, 0, 20, 40, 0);  // total 1210
    atm::CpuDeltaPercentages d;
    atm::computeCpuDelta(prev, cur, d);
    CHECK(d.valid);
    CHECK_NEAR(d.busy_percent, 180.0 / 230.0 * 100.0, 1e-9);
    CHECK_NEAR(d.user_percent, 100.0 / 230.0 * 100.0, 1e-9);
    CHECK_NEAR(d.system_percent, 50.0 / 230.0 * 100.0, 1e-9);
    CHECK_NEAR(d.idle_percent, 50.0 / 230.0 * 100.0, 1e-9);
    CHECK_NEAR(d.irq_percent, 10.0 / 230.0 * 100.0, 1e-9);
    CHECK_NEAR(d.softirq_percent, 20.0 / 230.0 * 100.0, 1e-9);
    CHECK_NEAR(d.nice_percent, 0.0, 1e-9);
    CHECK_NEAR(d.iowait_percent, 0.0, 1e-9);
    CHECK_NEAR(d.steal_percent, 0.0, 1e-9);
    // Categories must sum to 100%.
    const double sum = d.user_percent + d.nice_percent + d.system_percent +
                       d.idle_percent + d.iowait_percent + d.irq_percent +
                       d.softirq_percent + d.steal_percent;
    CHECK_NEAR(sum, 100.0, 1e-6);
  }

  run("iowait counts as idle time for busy");
  {
    const atm::CpuCounterSnapshot prev = makeCounter(0, 0, 0, 1000, 0);
    const atm::CpuCounterSnapshot cur = makeCounter(0, 0, 0, 1000, 30);
    atm::CpuDeltaPercentages d;
    atm::computeCpuDelta(prev, cur, d);
    CHECK(d.valid);
    CHECK_NEAR(d.busy_percent, 0.0, 1e-9);
    CHECK_NEAR(d.iowait_percent, 100.0, 1e-9);
    CHECK_NEAR(d.idle_percent, 0.0, 1e-9);
  }

  run("irq and softirq are busy time");
  {
    const atm::CpuCounterSnapshot prev = makeCounter(0, 0, 0, 800, 0, 0, 0, 0);
    const atm::CpuCounterSnapshot cur = makeCounter(0, 0, 0, 800, 0, 100, 100, 0);
    atm::CpuDeltaPercentages d;
    atm::computeCpuDelta(prev, cur, d);
    CHECK(d.valid);
    CHECK_NEAR(d.busy_percent, 100.0, 1e-9);
    CHECK_NEAR(d.irq_percent, 50.0, 1e-9);
    CHECK_NEAR(d.softirq_percent, 50.0, 1e-9);
  }

  run("steal is busy time and reported separately");
  {
    const atm::CpuCounterSnapshot prev = makeCounter(0, 0, 0, 900, 0, 0, 0, 0);
    const atm::CpuCounterSnapshot cur = makeCounter(0, 0, 0, 900, 0, 0, 0, 100);
    atm::CpuDeltaPercentages d;
    atm::computeCpuDelta(prev, cur, d);
    CHECK(d.valid);
    CHECK_NEAR(d.steal_percent, 100.0, 1e-9);
    CHECK_NEAR(d.busy_percent, 100.0, 1e-9);
  }

  run("guest time is not double-counted");
  {
    // guest/guest_nice live inside user; the total() must not count them again.
    atm::CpuCounterSnapshot prev;
    prev.user = 100;
    prev.guest = 50;
    atm::CpuCounterSnapshot cur;
    cur.user = 200;
    cur.guest = 150;
    atm::CpuDeltaPercentages d;
    atm::computeCpuDelta(prev, cur, d);
    CHECK(d.valid);
    CHECK_NEAR(d.user_percent, 100.0, 1e-9);  // 100 user of 100 elapsed ticks
    CHECK_NEAR(d.busy_percent, 100.0, 1e-9);
  }

  run("zero total delta is invalid");
  {
    const atm::CpuCounterSnapshot prev = makeCounter(10, 0, 0, 90);
    const atm::CpuCounterSnapshot cur = makeCounter(10, 0, 0, 90);
    atm::CpuDeltaPercentages d;
    atm::computeCpuDelta(prev, cur, d);
    CHECK(!d.valid);
    CHECK(d.busy_percent == 0.0);
  }

  run("counter decrease is invalid");
  {
    atm::CpuCounterSnapshot prev = makeCounter(200, 0, 0, 800);
    atm::CpuCounterSnapshot cur = makeCounter(100, 0, 0, 900);
    atm::CpuDeltaPercentages d;
    atm::computeCpuDelta(prev, cur, d);
    CHECK(!d.valid);
  }

  run("counter reset yields no sample and values stay bounded");
  {
    atm::CpuCounterSnapshot prev = makeCounter(999999, 0, 0, 999999);
    atm::CpuCounterSnapshot cur = makeCounter(10, 0, 0, 10);  // reset lower
    atm::CpuDeltaPercentages d;
    atm::computeCpuDelta(prev, cur, d);
    CHECK(!d.valid);
  }

  run("suspend-resume large elapsed interval still bounded");
  {
    // Counters advance a lot (e.g. 3600 s of ticks) but percentages stay sane.
    const std::uint64_t big = 1ULL << 40;
    atm::CpuCounterSnapshot prev = makeCounter(big, 0, 0, 3 * big);
    atm::CpuCounterSnapshot cur =
        makeCounter(big + (1ULL << 38), 0, 0, 3 * big + (3ULL << 38));
    atm::CpuDeltaPercentages d;
    atm::computeCpuDelta(prev, cur, d);
    CHECK(d.valid);
    CHECK(d.busy_percent >= 0.0 && d.busy_percent <= 100.0);
    CHECK(d.idle_percent >= 0.0 && d.idle_percent <= 100.0);
    CHECK(std::isfinite(d.busy_percent));
    CHECK(std::isfinite(d.idle_percent));
  }

  run("percentages bounded between 0 and 100 and never NaN/inf");
  {
    atm::CpuDeltaPercentages d;
    // A wild current where the single idle counter skips forward.
    const atm::CpuCounterSnapshot prev = makeCounter(100, 0, 100, 100);
    const atm::CpuCounterSnapshot cur = makeCounter(100, 0, 100, 1000000);
    atm::computeCpuDelta(prev, cur, d);
    CHECK(d.valid);
    for (double value : {d.user_percent, d.nice_percent, d.system_percent,
                         d.idle_percent, d.iowait_percent, d.irq_percent,
                         d.softirq_percent, d.steal_percent, d.busy_percent}) {
      CHECK(std::isfinite(value));
      CHECK(value >= 0.0 && value <= 100.0);
    }
  }

  // --- CPU topology masks ----------------------------------------------------

  run("parseCpuList normal + ranges + dedupe");
  {
    std::vector<int> ids;
    CHECK(atm::parseCpuList("0-3,8-11", ids));
    const std::vector<int> expected = {0, 1, 2, 3, 8, 9, 10, 11};
    CHECK(ids == expected);
  }

  run("parseCpuList single ids and trailing comma form");
  {
    std::vector<int> ids;
    CHECK(atm::parseCpuList("0,2,4", ids));
    CHECK(ids == (std::vector<int>{0, 2, 4}));
    CHECK(atm::parseCpuList("1,1,2", ids));  // duplicates collapsed
    CHECK(ids == (std::vector<int>{1, 2}));
  }

  run("parseCpuList rejects malformed text");
  {
    std::vector<int> ids;
    CHECK(!atm::parseCpuList("", ids));
    CHECK(!atm::parseCpuList("abc", ids));
    CHECK(!atm::parseCpuList("5-2", ids));
    CHECK(!atm::parseCpuList("0,,2", ids));
  }

  // --- Frequency reading and formatting --------------------------------------

  run("formatCpuFrequency kHz to MHz/GHz");
  {
    CHECK(atm::formatCpuFrequency(std::optional<std::uint64_t>(800000)) ==
          "800 MHz");
    CHECK(atm::formatCpuFrequency(std::optional<std::uint64_t>(1000000)) ==
          "1.00 GHz");
    CHECK(atm::formatCpuFrequency(std::optional<std::uint64_t>(2394000)) ==
          "2.39 GHz");
    CHECK(atm::formatCpuFrequency(std::optional<std::uint64_t>(0)) == "0 MHz");
    CHECK(atm::formatCpuFrequency(std::nullopt) == "N/A");
  }

  run("formatCpuGovernor");
  {
    CHECK(atm::formatCpuGovernor(std::optional<std::string>("schedutil")) ==
          "schedutil");
    CHECK(atm::formatCpuGovernor(std::nullopt) == "N/A");
  }

  run("readCpuFrequency reading and fallbacks");
  {
    TestRoot root;
    root.writeCpufreq(0, "2200000", "800000", "4700000", "schedutil");
    // cpu1 only exposes cpuinfo_cur_freq (fallback source).
    root.write(root.sysCpu() / "cpu1" / "cpufreq" / "cpuinfo_cur_freq",
               "1000000\n");

    const atm::CpuFrequencyInfo f0 = atm::readCpuFrequency(0, root.root);
    CHECK(f0.has_policy);
    CHECK(f0.current_khz.has_value() && *f0.current_khz == 2200000);
    CHECK(f0.min_khz.has_value() && *f0.min_khz == 800000);
    CHECK(f0.max_khz.has_value() && *f0.max_khz == 4700000);
    CHECK(f0.governor.has_value() && *f0.governor == "schedutil");

    const atm::CpuFrequencyInfo f1 = atm::readCpuFrequency(1, root.root);
    CHECK(f1.has_policy);
    CHECK(f1.current_khz.has_value() && *f1.current_khz == 1000000);
    CHECK(!f1.min_khz.has_value());

    const atm::CpuFrequencyInfo f2 = atm::readCpuFrequency(2, root.root);
    CHECK(!f2.has_policy);
    CHECK(!f2.current_khz.has_value());
    CHECK(!f2.governor.has_value());
  }

  run("invalid and zero frequency values");
  {
    TestRoot root;
    root.write(root.sysCpu() / "cpu0" / "cpufreq" / "scaling_cur_freq", "abc\n");
    root.write(root.sysCpu() / "cpu0" / "cpufreq" / "scaling_min_freq", "0\n");
    const atm::CpuFrequencyInfo f = atm::readCpuFrequency(0, root.root);
    CHECK(f.has_policy);
    CHECK(!f.current_khz.has_value());  // invalid value -> unavailable
    CHECK(f.min_khz.has_value() && *f.min_khz == 0);  // zero is a valid value
  }

  run("frequency is never mandatory in the monitor");
  {
    TestRoot root;
    root.writeProcStat(cpuLine(-1, makeCounter(100, 0, 0, 900)) +
                       cpuLine(0, makeCounter(10, 0, 0, 90)));
    root.writeTopology("0", "0", "0");
    atm::AdvancedCpuMonitor monitor(root.root);
    (void)monitor.read();  // baseline
    root.writeProcStat(cpuLine(-1, makeCounter(130, 0, 0, 980)) +
                       cpuLine(0, makeCounter(20, 0, 0, 190)));
    const atm::AdvancedCpuSnapshot snap = monitor.read();
    CHECK(snap.cpus.size() == 1);
    CHECK(snap.cpus[0].has_sample);
    CHECK(!snap.cpus[0].frequency.has_policy);  // no cpufreq tree at all
    CHECK(!snap.cpus[0].frequency.current_khz.has_value());
  }

  // --- Monitor-level identity and hotplug ------------------------------------

  run("cpu ids are stable and not matched by vector index");
  {
    TestRoot root;
    root.writeProcStat(
        cpuLine(-1, makeCounter(200, 0, 0, 800)) +
        cpuLine(0, makeCounter(100, 0, 0, 900)) +
        cpuLine(2, makeCounter(20, 0, 0, 180)));
    root.writeTopology("0,2", "0-2", "0-2");
    atm::AdvancedCpuMonitor monitor(root.root);
    (void)monitor.read();

    // cpu0: +50 busy of +100 -> 50% ; cpu2: +20 busy of +20 -> 100%.
    root.writeProcStat(
        cpuLine(-1, makeCounter(250, 0, 0, 850)) +
        cpuLine(0, makeCounter(150, 0, 0, 950)) +
        cpuLine(2, makeCounter(40, 0, 0, 180)));
    const atm::AdvancedCpuSnapshot snap = monitor.read();

    CHECK(snap.cpus.size() == 3);  // cpu0 online, cpu1 offline, cpu2 online
    const atm::CpuStatistics *cpu0 = nullptr;
    const atm::CpuStatistics *cpu1 = nullptr;
    const atm::CpuStatistics *cpu2 = nullptr;
    for (const atm::CpuStatistics &stat : snap.cpus) {
      if (stat.cpu_id == 0) cpu0 = &stat;
      if (stat.cpu_id == 1) cpu1 = &stat;
      if (stat.cpu_id == 2) cpu2 = &stat;
    }
    CHECK(cpu0 != nullptr && cpu1 != nullptr && cpu2 != nullptr);
    CHECK(cpu0->online && cpu0->has_sample);
    CHECK_NEAR(cpu0->delta.busy_percent, 50.0, 0.5);
    CHECK(!cpu1->online && !cpu1->has_sample);  // offline, identity preserved
    CHECK(cpu2->online && cpu2->has_sample);
    // vec[2] here is cpu2 with 100%, NOT cpu0's 50%: proves ID-based matching.
    CHECK_NEAR(cpu2->delta.busy_percent, 100.0, 0.5);
  }

  run("cpu appears, disappears and returns online with fresh baselines");
  {
    TestRoot root;
    root.writeProcStat(cpuLine(-1, makeCounter(200, 0, 0, 800)) +
                       cpuLine(0, makeCounter(100, 0, 0, 900)));
    root.writeTopology("0", "0-1", "0-1");
    atm::AdvancedCpuMonitor monitor(root.root);
    (void)monitor.read();

    // cpu0 still online; cpu1 (present but offline) is reported offline.
    {
      const atm::AdvancedCpuSnapshot snap = monitor.read();
      const atm::CpuStatistics *cpu1 = nullptr;
      for (const atm::CpuStatistics &stat : snap.cpus) {
        if (stat.cpu_id == 1) cpu1 = &stat;
      }
      CHECK(cpu1 != nullptr);
      CHECK(!cpu1->online && !cpu1->has_sample);
    }

    // cpu1 appears online with counters -> baseline, no sample yet.
    root.writeProcStat(
        cpuLine(-1, makeCounter(240, 0, 0, 860)) +
        cpuLine(0, makeCounter(140, 0, 0, 960)) +
        cpuLine(1, makeCounter(10, 0, 0, 90)));
    root.writeTopology("0,1", "0-1", "0-1");
    atm::AdvancedCpuSnapshot snap = monitor.read();
    const atm::CpuStatistics *cpu1 = nullptr;
    for (const atm::CpuStatistics &stat : snap.cpus) {
      if (stat.cpu_id == 1) cpu1 = &stat;
    }
    CHECK(snap.cpus.size() == 2);
    CHECK(cpu1 != nullptr && cpu1->online && !cpu1->has_sample);

    // cpu1 had one more tick of idle -> now a valid sample.
    root.writeProcStat(
        cpuLine(-1, makeCounter(280, 0, 0, 920)) +
        cpuLine(0, makeCounter(180, 0, 0, 1020)) +
        cpuLine(1, makeCounter(10, 0, 0, 95)));
    snap = monitor.read();
    for (const atm::CpuStatistics &stat : snap.cpus) {
      if (stat.cpu_id == 1) cpu1 = &stat;
    }
    CHECK(cpu1->online && cpu1->has_sample);
    CHECK_NEAR(cpu1->delta.busy_percent, 0.0, 0.5);
    CHECK_NEAR(cpu1->delta.idle_percent, 100.0, 0.5);

    // cpu1 disappears (offline) -> still listed, offline, no sample, no crash.
    root.writeProcStat(cpuLine(-1, makeCounter(320, 0, 0, 960)) +
                       cpuLine(0, makeCounter(220, 0, 0, 1060)));
    root.writeTopology("0", "0-1", "0-1");
    snap = monitor.read();
    cpu1 = nullptr;
    for (const atm::CpuStatistics &stat : snap.cpus) {
      if (stat.cpu_id == 1) cpu1 = &stat;
    }
    CHECK(cpu1 != nullptr);
    CHECK(!cpu1->online && !cpu1->has_sample);

    // cpu1 returns online -> fresh baseline again, then a valid sample.
    root.writeProcStat(
        cpuLine(-1, makeCounter(360, 0, 0, 1000)) +
        cpuLine(0, makeCounter(260, 0, 0, 1100)) +
        cpuLine(1, makeCounter(50, 0, 0, 50)));
    root.writeTopology("0,1", "0-1", "0-1");
    snap = monitor.read();
    for (const atm::CpuStatistics &stat : snap.cpus) {
      if (stat.cpu_id == 1) cpu1 = &stat;
    }
    CHECK(cpu1->online && !cpu1->has_sample);  // returned: baseline first

    root.writeProcStat(
        cpuLine(-1, makeCounter(400, 0, 0, 1000)) +
        cpuLine(0, makeCounter(300, 0, 0, 1100)) +
        cpuLine(1, makeCounter(150, 0, 0, 50)));
    snap = monitor.read();
    for (const atm::CpuStatistics &stat : snap.cpus) {
      if (stat.cpu_id == 1) cpu1 = &stat;
    }
    CHECK(cpu1->online && cpu1->has_sample);
    CHECK_NEAR(cpu1->delta.busy_percent, 100.0, 0.5);
  }

  run("online set changes update reported state");
  {
    TestRoot root;
    root.writeProcStat(cpuLine(-1, makeCounter(200, 0, 0, 800)) +
                       cpuLine(0, makeCounter(100, 0, 0, 900)) +
                       cpuLine(2, makeCounter(20, 0, 0, 180)));
    root.writeTopology("0,2", "0-2", "0-2");
    atm::AdvancedCpuMonitor monitor(root.root);
    (void)monitor.read();

    // cpu1 comes online; cpu2 goes offline => both transitions reflected.
    root.writeProcStat(
        cpuLine(-1, makeCounter(240, 0, 0, 860)) +
        cpuLine(0, makeCounter(140, 0, 0, 960)) +
        cpuLine(1, makeCounter(5, 0, 0, 45)));
    root.writeTopology("0,1", "0-2", "0-2");
    const atm::AdvancedCpuSnapshot snap = monitor.read();
    const atm::CpuStatistics *cpu1 = nullptr;
    const atm::CpuStatistics *cpu2 = nullptr;
    for (const atm::CpuStatistics &stat : snap.cpus) {
      if (stat.cpu_id == 1) cpu1 = &stat;
      if (stat.cpu_id == 2) cpu2 = &stat;
    }
    CHECK(cpu1 != nullptr && cpu1->online);
    CHECK(cpu2 != nullptr && !cpu2->online);  // removed from online set
  }

  run("missing CPU counter for an online-declared CPU is not a process error");
  {
    TestRoot root;
    root.writeProcStat(cpuLine(-1, makeCounter(100, 0, 0, 900)));
    root.writeTopology("0-1", "0-1", "0-1");
    atm::AdvancedCpuMonitor monitor(root.root);
    (void)monitor.read();
    const atm::AdvancedCpuSnapshot snap = monitor.read();
    const atm::CpuStatistics *cpu1 = nullptr;
    for (const atm::CpuStatistics &stat : snap.cpus) {
      if (stat.cpu_id == 1) cpu1 = &stat;
    }
    // cpu1 is reported online (topology) but has no counters -> no sample.
    CHECK(cpu1 != nullptr && cpu1->online);
    CHECK(!cpu1->has_sample);
  }

  run("counter reset discards the invalid delta and re-baselines");
  {
    TestRoot root;
    root.writeProcStat(cpuLine(-1, makeCounter(300, 0, 0, 700)) +
                       cpuLine(0, makeCounter(100, 0, 0, 900)));
    root.writeTopology("0", "0", "0");
    atm::AdvancedCpuMonitor monitor(root.root);
    (void)monitor.read();  // baseline

    root.writeProcStat(cpuLine(-1, makeCounter(330, 0, 0, 770)) +
                       cpuLine(0, makeCounter(120, 0, 0, 990)));
    const atm::AdvancedCpuSnapshot snap1 = monitor.read();
    CHECK(snap1.cpus[0].has_sample);

    // counters drop (reset / VM migration)
    root.writeProcStat(cpuLine(-1, makeCounter(50, 0, 0, 50)) +
                       cpuLine(0, makeCounter(10, 0, 0, 90)));
    const atm::AdvancedCpuSnapshot snap2 = monitor.read();
    CHECK(!snap2.aggregate.has_sample);
    CHECK(!snap2.cpus[0].has_sample);

    // The very next read must not mix the pre-reset baseline with the new one.
    root.writeProcStat(cpuLine(-1, makeCounter(60, 0, 0, 140)) +
                       cpuLine(0, makeCounter(20, 0, 0, 180)));
    const atm::AdvancedCpuSnapshot snap3 = monitor.read();
    CHECK(snap3.aggregate.has_sample);
    CHECK(snap3.cpus[0].has_sample);
    CHECK_NEAR(snap3.aggregate.delta.busy_percent, 10.0, 1e-9);
    CHECK_NEAR(snap3.cpus[0].delta.busy_percent, 10.0, 1e-9);
  }

  run("aggregate utilization tracks the per-cpu sum");
  {
    TestRoot root;
    // Aggregate = sum of the two logical CPUs for consistent input.
    root.writeProcStat(
        cpuLine(-1, makeCounter(200, 0, 0, 1800)) +
        cpuLine(0, makeCounter(100, 0, 0, 900)) +
        cpuLine(1, makeCounter(100, 0, 0, 900)));
    root.writeTopology("0,1", "0-1", "0-1");
    atm::AdvancedCpuMonitor monitor(root.root);
    (void)monitor.read();

    root.writeProcStat(
        cpuLine(-1, makeCounter(250, 0, 0, 1900)) +
        cpuLine(0, makeCounter(140, 0, 0, 960)) +
        cpuLine(1, makeCounter(110, 0, 0, 940)));
    const atm::AdvancedCpuSnapshot snap = monitor.read();
    CHECK(snap.aggregate.has_sample);
    // 50 busy of 150 elapsed ticks on the aggregate.
    CHECK_NEAR(snap.aggregate.delta.busy_percent, 50.0 / 150.0 * 100.0, 1e-9);
    CHECK_NEAR(snap.aggregate.delta.user_percent, 50.0 / 150.0 * 100.0, 1e-9);
    CHECK_NEAR(snap.aggregate.delta.idle_percent, 100.0 / 150.0 * 100.0,
               1e-9);
  }

  // --- History integration ---------------------------------------------------

  run("history stores one sample per CPU per update");
  {
    atm::HistoryManager history(10);
    history.updateCpuHistories({{0, 50.0}, {1, 20.0}});
    CHECK(history.cpuHistories().size() == 2);
    CHECK(history.cpuHistories()[0].cpu_id == 0);
    CHECK(history.cpuHistories()[0].utilization.size() == 1);
    CHECK(history.cpuHistories()[1].cpu_id == 1);
    CHECK(history.cpuHistories()[1].utilization.size() == 1);
    CHECK_NEAR(history.cpuHistories()[0].utilization.samples().front().value,
               50.0, 1e-9);
  }

  run("history is bounded by max samples");
  {
    atm::HistoryManager history(5);
    for (int i = 0; i < 20; ++i) {
      history.updateCpuHistories({{0, 10.0 + i}});
    }
    CHECK(history.cpuHistories()[0].utilization.size() == 5);
    CHECK(history.cpuHistories()[0].utilization.maxSamples() == 5);
  }

  run("history respects pause / one sample per refresh");
  {
    atm::HistoryManager history(10);
    history.setPaused(true);
    history.updateCpuHistories({{0, 1.0}});
    CHECK(history.cpuHistories().empty());  // paused: nothing collected
    history.setPaused(false);
    history.updateCpuHistories({{0, 1.0}});
    history.updateCpuHistories({{0, 2.0}});
    history.updateCpuHistories({{0, 3.0}});
    CHECK(history.cpuHistories()[0].utilization.size() == 3);
  }

  run("per-cpu history tracks identity across CPU appearance/removal");
  {
    atm::HistoryManager history(10);
    history.updateCpuHistories({{0, 10.0}, {2, 30.0}});
    CHECK(history.cpuHistories().size() == 2);
    CHECK(history.cpuHistories()[0].cpu_id == 0);
    CHECK(history.cpuHistories()[1].cpu_id == 2);

    // cpu1 appears: the set changes, histories are rebuilt for stable IDs.
    history.updateCpuHistories({{0, 11.0}, {1, 40.0}, {2, 31.0}});
    CHECK(history.cpuHistories().size() == 3);
    CHECK(history.cpuHistories()[0].cpu_id == 0);
    CHECK(history.cpuHistories()[1].cpu_id == 1);
    CHECK(history.cpuHistories()[2].cpu_id == 2);
    CHECK(history.cpuHistories()[0].utilization.size() == 1);
    CHECK(history.cpuHistories()[1].utilization.size() == 1);
    CHECK(history.cpuHistories()[2].utilization.size() == 1);
  }

  run("history timestamps are consistent within one update");
  {
    atm::HistoryManager history(10);
    history.updateCpuHistories({{0, 10.0}, {1, 20.0}});
    const auto t0 = history.cpuHistories()[0].utilization.samples().front().timestamp;
    const auto t1 = history.cpuHistories()[1].utilization.samples().front().timestamp;
    CHECK(t0 == t1);
  }

  if (g_failures == 0) {
    std::fprintf(stderr, "PASS (%d checks)\n", g_checks);
    return EXIT_SUCCESS;
  }
  std::fprintf(stderr, "FAIL (%d of %d checks failed)\n", g_failures, g_checks);
  return EXIT_FAILURE;
}