#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "history_manager.hpp"
#include "process_statistics.hpp"

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
  if (!(a > b - eps && a < b + eps)) {
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

namespace {

using atm::Process;
using atm::ProcessSnapshot;
using atm::ProcessState;
using atm::SystemProcessStatistics;

/// Builds a snapshot from a list of (pid, state, starttime) triples. Threads,
/// CPU ticks and memory are set when non-zero so aggregation paths that skip
/// zero values are exercised separately by other tests.
ProcessSnapshot makeSnapshot(
    std::vector<std::tuple<int, ProcessState, std::uint64_t>> entries) {
  ProcessSnapshot snapshot;
  snapshot.processes.reserve(entries.size());
  for (const auto &[pid, state, starttime] : entries) {
    Process p;
    p.pid = pid;
    p.state = state;
    p.starttime_ticks = starttime;
    snapshot.processes.push_back(std::move(p));
  }
  snapshot.stats.total = snapshot.processes.size();
  return snapshot;
}

}  // namespace

int main() {
  using atm::aggregateSystemProcessStatistics;
  using atm::ProcessStatisticsAggregator;

  // --- aggregateSystemProcessStatistics -----------------------------------
  run("aggregate of an empty snapshot is all zeros");
  {
    const SystemProcessStatistics s = aggregateSystemProcessStatistics({});
    CHECK(s.total == 0);
    CHECK(s.running == 0);
    CHECK(s.sleeping == 0);
    CHECK(s.disk_sleep == 0);
    CHECK(s.stopped == 0);
    CHECK(s.zombie == 0);
    CHECK(s.idle == 0);
    CHECK(s.unknown == 0);
    CHECK(s.total_threads == 0);
    CHECK(s.processes_with_thread_data == 0);
    CHECK(s.aggregate_cpu_percent == 0.0);
    CHECK(s.total_user_cpu_ticks == 0);
    CHECK(s.total_system_cpu_ticks == 0);
    CHECK(s.total_cpu_ticks == 0);
    CHECK(s.total_rss_kib == 0);
    CHECK(s.total_shared_kib == 0);
    CHECK(s.aggregate_memory_percent == 0.0);
    CHECK(s.total_read_rate == 0.0);
    CHECK(s.total_write_rate == 0.0);
  }

  run("aggregate groups counts by process state");
  {
    ProcessSnapshot snapshot = makeSnapshot({
        {101, ProcessState::Running, 1},
        {102, ProcessState::Sleeping, 2},
        {103, ProcessState::Sleeping, 3},
        {104, ProcessState::DiskSleep, 4},
        {105, ProcessState::Stopped, 5},
        {106, ProcessState::Zombie, 6},
        {107, ProcessState::Idle, 7},
        {108, ProcessState::Unknown, 8},
    });
    const SystemProcessStatistics s = aggregateSystemProcessStatistics(snapshot);
    CHECK(s.total == 8);
    CHECK(s.running == 1);
    CHECK(s.sleeping == 2);
    CHECK(s.disk_sleep == 1);
    CHECK(s.stopped == 1);
    CHECK(s.zombie == 1);
    CHECK(s.idle == 1);
    CHECK(s.unknown == 1);
  }

  run("aggregate sums thread counts");
  {
    ProcessSnapshot snapshot;
    Process a;
    a.pid = 1;
    a.thread_count = 12;
    snapshot.processes.push_back(a);
    Process b;
    b.pid = 2;
    b.thread_count = 8;
    snapshot.processes.push_back(b);
    Process c;
    c.pid = 3;
    c.thread_count = 0;  // no data
    snapshot.processes.push_back(c);

    const SystemProcessStatistics s = aggregateSystemProcessStatistics(snapshot);
    CHECK(s.total_threads == 20);
    CHECK(s.processes_with_thread_data == 2);
  }

  run("aggregate sums CPU usage percentages and user/system ticks");
  {
    ProcessSnapshot snapshot;
    Process a;
    a.pid = 1;
    a.cpu_percent = 12.5;
    a.user_cpu_ticks = 100;
    a.system_cpu_ticks = 40;
    snapshot.processes.push_back(a);
    Process b;
    b.pid = 2;
    b.cpu_percent = 7.0;
    b.user_cpu_ticks = 50;
    b.system_cpu_ticks = 10;
    snapshot.processes.push_back(b);
    Process c;
    c.pid = 3;
    c.cpu_percent = 0.0;
    c.user_cpu_ticks = 0;
    c.system_cpu_ticks = 0;
    snapshot.processes.push_back(c);

    const SystemProcessStatistics s = aggregateSystemProcessStatistics(snapshot);
    CHECK_NEAR(s.aggregate_cpu_percent, 19.5, 1e-9);
    CHECK(s.processes_with_cpu_data == 3);
    CHECK(s.total_user_cpu_ticks == 150);
    CHECK(s.total_system_cpu_ticks == 50);
    CHECK(s.total_cpu_ticks == 200);
  }

  run("aggregate sums memory, shared memory and memory percent");
  {
    ProcessSnapshot snapshot;
    Process a;
    a.pid = 1;
    a.memory_kib = 2048;
    a.shared_memory_kib = 512;
    a.memory_percent = 1.5;
    snapshot.processes.push_back(a);
    Process b;
    b.pid = 2;
    b.memory_kib = 1024;
    b.shared_memory_kib = 128;
    b.memory_percent = 0.5;
    snapshot.processes.push_back(b);
    Process c;
    c.pid = 3;
    c.memory_kib = 0;  // no VmRSS data
    c.memory_percent = 0.0;
    snapshot.processes.push_back(c);

    const SystemProcessStatistics s = aggregateSystemProcessStatistics(snapshot);
    CHECK(s.total_rss_kib == 3072);
    CHECK(s.total_shared_kib == 640);
    CHECK_NEAR(s.aggregate_memory_percent, 2.0, 1e-9);
    CHECK(s.processes_with_memory_data == 2);
  }

  run("aggregate sums I/O rates from processes with I/O data");
  {
    ProcessSnapshot snapshot;
    Process a;
    a.pid = 1;
    a.io_available = true;
    a.read_rate = 1000.0;
    a.write_rate = 250.0;
    snapshot.processes.push_back(a);
    Process b;
    b.pid = 2;
    b.io_available = true;
    b.read_rate = 500.0;
    b.write_rate = 750.0;
    snapshot.processes.push_back(b);
    Process c;
    c.pid = 3;
    c.io_available = false;
    c.read_rate = 99999.0;  // must be ignored
    snapshot.processes.push_back(c);

    const SystemProcessStatistics s = aggregateSystemProcessStatistics(snapshot);
    CHECK_NEAR(s.total_read_rate, 1500.0, 1e-9);
    CHECK_NEAR(s.total_write_rate, 1000.0, 1e-9);
    CHECK(s.processes_with_io_data == 2);
  }

  // --- ProcessStatisticsAggregator ----------------------------------------
  run("first update establishes a baseline with zero activity");
  {
    ProcessStatisticsAggregator aggregator;
    ProcessSnapshot snapshot = makeSnapshot({
        {101, ProcessState::Running, 1},
        {102, ProcessState::Sleeping, 2},
    });
    const SystemProcessStatistics s = aggregator.update(snapshot);
    CHECK(s.total == 2);
    CHECK(s.process_creations == 0);
    CHECK(s.process_exits == 0);
    CHECK(s.creation_rate_per_second == 0.0);
    CHECK(s.exit_rate_per_second == 0.0);
  }

  run("unchanged population between updates reports no activity");
  {
    ProcessStatisticsAggregator aggregator;
    const ProcessSnapshot first = makeSnapshot({
        {101, ProcessState::Running, 1},
        {102, ProcessState::Sleeping, 2},
    });
    const SystemProcessStatistics s1 = aggregator.update(first);
    CHECK(s1.process_creations == 0);

    const ProcessSnapshot second = makeSnapshot({
        {101, ProcessState::Running, 1},
        {102, ProcessState::Sleeping, 2},
    });
    const SystemProcessStatistics s2 = aggregator.update(second);
    CHECK(s2.total == 2);
    CHECK(s2.process_creations == 0);
    CHECK(s2.process_exits == 0);
  }

  run("a new process counts as one creation");
  {
    ProcessStatisticsAggregator aggregator;
    const ProcessSnapshot first = makeSnapshot({
        {101, ProcessState::Running, 1},
    });
    static_cast<void>(aggregator.update(first));

    const ProcessSnapshot second = makeSnapshot({
        {101, ProcessState::Running, 1},
        {102, ProcessState::Zombie, 9},
    });
    const SystemProcessStatistics s = aggregator.update(second);
    CHECK(s.process_creations == 1);
    CHECK(s.process_exits == 0);
    CHECK(s.creation_rate_per_second > 0.0);
    CHECK(s.exit_rate_per_second == 0.0);
  }

  run("a vanished process counts as one exit");
  {
    ProcessStatisticsAggregator aggregator;
    const ProcessSnapshot first = makeSnapshot({
        {101, ProcessState::Running, 1},
        {102, ProcessState::Sleeping, 2},
    });
    static_cast<void>(aggregator.update(first));

    const ProcessSnapshot second = makeSnapshot({
        {101, ProcessState::Running, 1},
    });
    const SystemProcessStatistics s = aggregator.update(second);
    CHECK(s.process_creations == 0);
    CHECK(s.process_exits == 1);
    CHECK(s.exit_rate_per_second > 0.0);
  }

  run("a reused PID with a new start time is one exit plus one creation");
  {
    ProcessStatisticsAggregator aggregator;
    const ProcessSnapshot first = makeSnapshot({
        {101, ProcessState::Running, 100},
    });
    static_cast<void>(aggregator.update(first));

    // Same PID but a different /proc/<pid>/stat start-time: the old
    // incarnation exited and a new one took over the PID.
    const ProcessSnapshot second = makeSnapshot({
        {101, ProcessState::Running, 200},
    });
    const SystemProcessStatistics s = aggregator.update(second);
    CHECK(s.process_creations == 1);
    CHECK(s.process_exits == 1);
  }

  run("multiple creations and exits are counted together");
  {
    ProcessStatisticsAggregator aggregator;
    const ProcessSnapshot first = makeSnapshot({
        {101, ProcessState::Running, 1},
        {102, ProcessState::Sleeping, 2},
        {103, ProcessState::Sleeping, 3},
    });
    static_cast<void>(aggregator.update(first));

    const ProcessSnapshot second = makeSnapshot({
        {101, ProcessState::Running, 1},
        {104, ProcessState::Zombie, 4},
        {105, ProcessState::Zombie, 5},
    });
    const SystemProcessStatistics s = aggregator.update(second);
    CHECK(s.process_creations == 2);
    CHECK(s.process_exits == 2);
  }

  run("reset clears the baseline so the next update reports no activity");
  {
    ProcessStatisticsAggregator aggregator;
    const ProcessSnapshot first = makeSnapshot({
        {101, ProcessState::Running, 1},
    });
    static_cast<void>(aggregator.update(first));
    aggregator.reset();

    // Even a completely different population must not count as activity
    // because the baseline was discarded.
    const ProcessSnapshot second = makeSnapshot({
        {102, ProcessState::Sleeping, 2},
    });
    const SystemProcessStatistics s = aggregator.update(second);
    CHECK(s.total == 1);
    CHECK(s.process_creations == 0);
    CHECK(s.process_exits == 0);
  }

  // --- HistoryManager integration -----------------------------------------
  run("updateProcessStats records all process history samples");
  {
    atm::HistoryManager history(120);
    SystemProcessStatistics s;
    s.total = 42;
    s.running = 3;
    s.zombie = 1;
    s.total_threads = 300;
    s.aggregate_cpu_percent = 25.5;
    s.total_rss_kib = 1048576;
    s.creation_rate_per_second = 2.0;
    s.exit_rate_per_second = 1.0;
    history.updateProcessStats(s);

    CHECK(history.processCountHistory().size() == 1);
    CHECK(history.processCountHistory().samples().back().value == 42.0);
    CHECK(history.runningCountHistory().size() == 1);
    CHECK(history.runningCountHistory().samples().back().value == 3.0);
    CHECK(history.zombieCountHistory().samples().back().value == 1.0);
    CHECK(history.threadCountHistory().samples().back().value == 300.0);
    CHECK_NEAR(history.aggregateCpuHistory().samples().back().value, 25.5, 1e-9);
    CHECK(history.aggregateRssHistory().samples().back().value == 1048576.0);
    CHECK_NEAR(history.creationRateHistory().samples().back().value, 2.0, 1e-9);
    CHECK_NEAR(history.exitRateHistory().samples().back().value, 1.0, 1e-9);
  }

  run("paused history does not record process samples");
  {
    atm::HistoryManager history(8);
    history.setPaused(true);
    SystemProcessStatistics s;
    s.total = 7;
    history.updateProcessStats(s);
    CHECK(history.processCountHistory().empty());
  }

  run("process history is bounded by max samples");
  {
    atm::HistoryManager history(3);
    for (std::size_t i = 0; i < 5; ++i) {
      SystemProcessStatistics s;
      s.total = i;
      history.updateProcessStats(s);
    }
    CHECK(history.processCountHistory().size() == 3);
    // Ring buffer: the first two samples were evicted.
    CHECK(history.processCountHistory().samples().front().value == 2.0);
    CHECK(history.processCountHistory().samples().back().value == 4.0);
  }

  run("clearAll also clears the process histories");
  {
    atm::HistoryManager history(8);
    SystemProcessStatistics s;
    s.total = 3;
    history.updateProcessStats(s);
    history.clearAll();
    CHECK(history.processCountHistory().empty());
  }

  if (g_failures == 0) {
    std::fprintf(stderr, "OK: %d checks passed\n", g_checks);
    return 0;
  }
  std::fprintf(stderr, "FAILED: %d of %d checks\n", g_failures, g_checks);
  return 1;
}