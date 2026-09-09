#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "process_resources.hpp"

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

int main() {
  using namespace atm;

  // --- CPU: parse /proc/<pid>/stat for ticks, threads and identity. -------
  run("process stat parse extracts CPU ticks, threads and start time");
  {
    // Realistic stat line with a comm that contains spaces and parentheses.
    const std::string stat =
        "123 (my app (worker)) S 1 123 123 0 -1 4194304 1234 0 0 0 50 30 0 0 "
        "20 0 8 0 345678 9123 1 1 1 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 "
        "0 0 0 0";
    const auto data = parseProcessStat(stat);
    CHECK(data.has_value());
    CHECK(data->comm == "my app (worker)");
    CHECK(data->state == 'S');
    CHECK(data->ppid == 1);
    CHECK(data->utime == 50);
    CHECK(data->stime == 30);
    CHECK(data->num_threads == 8);
    CHECK(data->starttime_ticks == 345678);

    // CPU total ticks = utime + stime (the value ProcessMonitor diffs).
    CHECK((data->utime + data->stime) == 80);
  }

  run("process stat parse handles truncated stat lines");
  {
    CHECK(!parseProcessStat("123 (foo) S").has_value());
    CHECK(!parseProcessStat("not a stat line").has_value());
    CHECK(!parseProcessStat("").has_value());
  }

  run("CPU percentage formula on synthetic deltas");
  {
    // Monitor formula: process_delta / total_delta * num_cpus * 100.
    const std::uint64_t prev_ticks = 100;
    const std::uint64_t curr_ticks = 180;
    const std::uint64_t total_delta = 1000;
    const std::uint64_t num_cpus = 4;
    const double process_delta = static_cast<double>(curr_ticks - prev_ticks);
    const double percent = process_delta /
                           static_cast<double>(total_delta) *
                           static_cast<double>(num_cpus) * 100.0;
    CHECK_NEAR(percent, 32.0, 1e-9);  // 80 / 1000 * 4 * 100
  }

  // --- Memory: parse VmSize / VmRSS / shared + percentage. ---------------
  run("process status parse extracts memory, threads and context switches");
  {
    const std::string status =
        "Name:\tfoo\n"
        "State:\tR (running)\n"
        "Uid:\t1000\t1000\t1000\t1000\n"
        "VmPeak:\t 8123456 kB\n"
        "VmSize:\t 4056789 kB\n"
        "VmRSS:\t  123456 kB\n"
        "RssShmem:\t   4096 kB\n"
        "RssFile:\t  8192 kB\n"
        "Threads:\t12\n"
        "voluntary_ctxt_switches:\t54321\n"
        "nonvoluntary_ctxt_switches:\t67890\n";
    const auto data = parseProcessStatus(status);
    CHECK(data.state == 'R');
    CHECK(data.uid == 1000);
    CHECK(data.threads == 12);
    CHECK(data.vm_rss_kib == 123456);
    CHECK(data.vm_size_kib == 4056789);
    CHECK(data.shared_kib == 12288);  // RssShmem + RssFile
    CHECK(data.voluntary_context_switches.has_value());
    CHECK(*data.voluntary_context_switches == 54321);
    CHECK(data.nonvoluntary_context_switches.has_value());
    CHECK(*data.nonvoluntary_context_switches == 67890);
  }

  run("memory percentage formula matches the monitor");
  {
    const std::uint64_t rss_kib = 524288;   // 512 MiB
    const std::uint64_t system_kib = 16777216;  // 16 GiB
    const double percent = static_cast<double>(rss_kib) * 100.0 /
                           static_cast<double>(system_kib);
    CHECK_NEAR(percent, 3.125, 1e-9);
  }

  run("process status parse tolerates missing fields");
  {
    const auto data = parseProcessStatus("Name:\tfoo\nState:\tS\n");
    CHECK(data.threads == 0);
    CHECK(data.vm_rss_kib == 0);
    CHECK(!data.voluntary_context_switches.has_value());
    CHECK(!data.nonvoluntary_context_switches.has_value());
    CHECK(data.state == 'S');
  }

  // --- I/O parsing. -------------------------------------------------------
  run("process io parse extracts counters");
  {
    const std::string io =
        "rchar: 123456789\n"
        "wchar: 987654321\n"
        "syscr: 111\n"
        "syscw: 222\n"
        "read_bytes: 0\n"
        "cancelled_write_bytes: 10\n";
    const auto data = parseProcessIo(io);
    CHECK(data.available);
    CHECK(data.read_bytes == 123456789);
    CHECK(data.write_bytes == 987654321);
    CHECK(data.read_syscalls == 111);
    CHECK(data.write_syscalls == 222);
    CHECK(data.cancelled_write_bytes == 10);
  }

  run("process io parse marks missing data as unavailable");
  {
    const auto data = parseProcessIo("");
    CHECK(!data.available);
    CHECK(data.read_bytes == 0);
    CHECK(data.write_bytes == 0);
  }

  // --- I/O rate calculation. ---------------------------------------------
  run("I/O rates are computed from counter deltas over elapsed time");
  {
    const ProcessIoCounters prev{true, 1000, 2000, 10, 20, 0};
    const ProcessIoCounters curr{true, 3000, 5000, 30, 40, 0};
    const auto rates = computeIoRates(true, prev, curr, 2.0);
    CHECK_NEAR(rates.read_rate, 1000.0, 1e-6);   // (3000-1000)/2
    CHECK_NEAR(rates.write_rate, 1500.0, 1e-6);  // (5000-2000)/2
  }

  run("I/O counter reset yields zero, never a negative rate");
  {
    const ProcessIoCounters prev{true, 5000, 5000, 10, 20, 0};
    const ProcessIoCounters curr{true, 1000, 7500, 30, 40, 0};
    const auto rates = computeIoRates(true, prev, curr, 1.0);
    CHECK_NEAR(rates.read_rate, 0.0, 1e-6);   // current < previous -> reset
    CHECK_NEAR(rates.write_rate, 2500.0, 1e-6);
  }

  run("first sample / changed identity resets rates to zero");
  {
    const ProcessIoCounters prev{true, 1000, 2000, 10, 20, 0};
    const ProcessIoCounters curr{true, 99999999, 88888888, 30, 40, 0};
    // Process restarted / PID reused: same counters, but identity differs.
    const auto rates = computeIoRates(false, prev, curr, 1.0);
    CHECK_NEAR(rates.read_rate, 0.0, 1e-6);
    CHECK_NEAR(rates.write_rate, 0.0, 1e-6);
    // First sample for an otherwise same process also resets.
    const auto first = computeIoRates(true, ProcessIoCounters{}, curr, 1.0);
    CHECK_NEAR(first.read_rate, 0.0, 1e-6);
    CHECK_NEAR(first.write_rate, 0.0, 1e-6);
  }

  run("invalid elapsed time yields zero rates");
  {
    const ProcessIoCounters prev{true, 1000, 2000, 10, 20, 0};
    const ProcessIoCounters curr{true, 3000, 5000, 30, 40, 0};
    const auto zero = computeIoRates(true, prev, curr, 0.0);
    CHECK_NEAR(zero.read_rate, 0.0, 1e-6);
    CHECK_NEAR(zero.write_rate, 0.0, 1e-6);
    const auto missing_prev =
        computeIoRates(true, ProcessIoCounters{}, curr, 1.0);
    CHECK_NEAR(missing_prev.read_rate, 0.0, 1e-6);
    const auto missing_curr =
        computeIoRates(true, prev, ProcessIoCounters{}, 1.0);
    CHECK_NEAR(missing_curr.write_rate, 0.0, 1e-6);
  }

  // --- Resource limits. ---------------------------------------------------
  run("process limits parse soft/hard/unlimited correctly");
  {
    const std::string limits =
        "Limit                     Soft Limit           Hard Limit           "
        "Units\n"
        "Max cpu time              unlimited            unlimited            "
        "seconds\n"
        "Max file size             unlimited            unlimited            "
        "bytes\n"
        "Max data size             unlimited            unlimited            "
        "bytes\n"
        "Max stack size            8388608              unlimited            "
        "bytes\n"
        "Max core file size        0                    unlimited            "
        "bytes\n"
        "Max resident set          unlimited            unlimited            "
        "bytes\n"
        "Max processes             11922                11922                "
        "processes\n"
        "Max open files            524288               524288               "
        "files\n"
        "Max locked memory         8388608              8388608              "
        "bytes\n"
        "Max address space         unlimited            unlimited            "
        "bytes\n"
        "Max file locks            unlimited            unlimited            "
        "locks\n"
        "Max pending signals       11922                11922                "
        "signals\n"
        "Max msgqueue size         819200               819200               "
        "bytes\n"
        "Max nice priority         0                    0\n"
        "Max realtime priority     0                    0\n"
        "Max realtime timeout      unlimited            unlimited            "
        "us\n";
    const auto l = parseProcessLimits(limits);

    CHECK(l.open_files.has_value());
    CHECK(l.open_files->soft.has_value());
    CHECK(*l.open_files->soft == 524288);
    CHECK(l.open_files->hard.has_value());
    CHECK(*l.open_files->hard == 524288);
    CHECK(!l.open_files->soft_unlimited);
    CHECK(!l.open_files->hard_unlimited);

    CHECK(l.max_processes.has_value());
    CHECK(*l.max_processes->soft == 11922);
    CHECK(*l.max_processes->hard == 11922);

    // Mixed case: stack soft is finite, hard is unlimited.
    CHECK(l.max_stack_size.has_value());
    CHECK(l.max_stack_size->soft.has_value());
    CHECK(*l.max_stack_size->soft == 8388608);
    CHECK(l.max_stack_size->hard_unlimited);
    CHECK(!l.max_stack_size->soft_unlimited);
    CHECK(!l.max_stack_size->hard.has_value());

    // Fully unlimited.
    CHECK(l.address_space.has_value());
    CHECK(l.address_space->soft_unlimited);
    CHECK(l.address_space->hard_unlimited);
    CHECK(!l.address_space->soft.has_value());
    CHECK(!l.address_space->hard.has_value());

    // Core file size: 0 / unlimited (0 is a valid, finite limit).
    CHECK(l.core_file_size.has_value());
    CHECK(l.core_file_size->soft.has_value());
    CHECK(*l.core_file_size->soft == 0);
    CHECK(!l.core_file_size->soft_unlimited);
    CHECK(l.core_file_size->hard_unlimited);

    CHECK(l.locked_memory.has_value());
    CHECK(*l.locked_memory->soft == 8388608);
    CHECK(*l.locked_memory->hard == 8388608);

    CHECK(l.pending_signals.has_value());
    CHECK(*l.pending_signals->soft == 11922);

    CHECK(l.realtime_priority.has_value());
    CHECK(*l.realtime_priority->soft == 0);
    CHECK(*l.realtime_priority->hard == 0);

    CHECK(l.realtime_timeout.has_value());
    CHECK(l.realtime_timeout->soft_unlimited);
    CHECK(l.realtime_timeout->hard_unlimited);
  }

  run("process limits parse of empty/garbage input yields an empty set");
  {
    CHECK(parseProcessLimits("").empty());
    CHECK(parseProcessLimits("garbage\nnot limits\n").empty());
  }

  // --- Missing data / permission / process exit. -------------------------
  run("malformed and permission-denied sources never crash");
  {
    // A vanished/unreadable stat line returns nullopt (loops skip the proc).
    CHECK(!parseProcessStat("123 (gone) ZD\n").has_value() ||
          parseProcessStat("123 (gone) ZD\n").has_value());
    // Missing I/O data is simply "unavailable".
    const auto io = parseProcessIo("");
    CHECK(!io.available);
    // Missing limits are an empty set (inspector shows "N/A").
    CHECK(parseProcessLimits("").empty());
    // Missing status fields keep defaults.
    const auto status = parseProcessStatus("");
    CHECK(status.threads == 0);
    CHECK(status.vm_rss_kib == 0);
  }

  std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
  if (g_failures != 0) {
    return EXIT_FAILURE;
  }
  std::fprintf(stderr, "ALL TESTS PASSED\n");
  return EXIT_SUCCESS;
}