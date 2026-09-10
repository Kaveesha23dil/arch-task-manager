#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "process_io_details.hpp"
#include "process_resources.hpp"
#include "process_scheduling.hpp"

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
  if (std::abs(a - b) > eps) {
    ++g_failures;
    std::fprintf(stderr, "FAIL %s:%d: %s (%g != %g, eps %g)\n", file, line,
                 expr, a, b, eps);
  }
}

void run(const char *name) { std::fprintf(stderr, "TEST %s\n", name); }
}  // namespace

#define CHECK(expr) ::expect((expr), #expr, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, eps) \
  ::expectNear(double(a), double(b), (eps), #a " == " #b, __FILE__, __LINE__)

// ---------------------------------------------------------------------------
// Fixtures: sample /proc/<pid>/io contents
// ---------------------------------------------------------------------------

namespace {

/// A realistic /proc/<pid>/io with all seven standard fields.
std::string sampleIo() {
  return std::string("rchar: 12345678\n"
                     "wchar: 87654321\n"
                     "syscr: 500\n"
                     "syscw: 300\n"
                     "read_bytes: 4096000\n"
                     "write_bytes: 8192000\n"
                     "cancelled_write_bytes: 512\n");
}

/// A /proc/<pid>/io with only some fields present.
std::string partialIo() {
  return std::string("rchar: 100\n"
                     "wchar: 200\n");
}

/// A /proc/<pid>/io with zero values.
std::string zeroIo() {
  return std::string("rchar: 0\n"
                     "wchar: 0\n"
                     "syscr: 0\n"
                     "syscw: 0\n"
                     "read_bytes: 0\n"
                     "write_bytes: 0\n"
                     "cancelled_write_bytes: 0\n");
}

/// Execs `sleep 3600` in a child for live /proc/<pid>/io inspection.
pid_t spawnSleep() {
  const pid_t child = ::fork();
  if (child < 0) {
    return -1;
  }
  if (child == 0) {
    char *const argv[] = {
        const_cast<char *>("sleep"), const_cast<char *>("3600"), nullptr};
    char *const envp[] = {nullptr};
    ::execve("/bin/sleep", argv, envp);
    ::_exit(127);
  }
  return child;
}

/// Waits (bounded) until the child has completed execve by polling its comm.
bool waitForExec(const pid_t child) {
  for (int attempt = 0; attempt < 2000; ++attempt) {
    const std::string comm_path = "/proc/" + std::to_string(child) + "/comm";
    std::FILE *file = std::fopen(comm_path.c_str(), "r");
    if (file != nullptr) {
      char comm[64] = {0};
      const bool read_ok = std::fgets(comm, sizeof(comm), file) != nullptr;
      std::fclose(file);
      if (read_ok && std::strncmp(comm, "sleep", 5) == 0) {
        return true;
      }
    }
    ::usleep(1000);
  }
  return false;
}

}  // namespace

int main() {
  using namespace atm;

  // ---------------------------------------------------------------- Normal
  run("parseIoDetails: all seven fields");
  {
    const ProcessIoDetailsInfo info = parseIoDetails(sampleIo());
    CHECK(info.chars_read.has_value() && *info.chars_read == 12345678);
    CHECK(info.chars_written.has_value() && *info.chars_written == 87654321);
    CHECK(info.read_syscalls.has_value() && *info.read_syscalls == 500);
    CHECK(info.write_syscalls.has_value() && *info.write_syscalls == 300);
    CHECK(info.bytes_read.has_value() && *info.bytes_read == 4096000);
    CHECK(info.bytes_written.has_value() && *info.bytes_written == 8192000);
    CHECK(info.cancelled_write_bytes.has_value() &&
          *info.cancelled_write_bytes == 512);
  }

  run("parseIoDetails: zero values");
  {
    const ProcessIoDetailsInfo info = parseIoDetails(zeroIo());
    CHECK(info.chars_read.has_value() && *info.chars_read == 0);
    CHECK(info.chars_written.has_value() && *info.chars_written == 0);
    CHECK(info.read_syscalls.has_value() && *info.read_syscalls == 0);
    CHECK(info.write_syscalls.has_value() && *info.write_syscalls == 0);
    CHECK(info.bytes_read.has_value() && *info.bytes_read == 0);
    CHECK(info.bytes_written.has_value() && *info.bytes_written == 0);
    CHECK(info.cancelled_write_bytes.has_value() &&
          *info.cancelled_write_bytes == 0);
  }

  run("parseIoDetails: large values");
  {
    const std::string io =
        "rchar: 18446744073709551615\n"
        "wchar: 9999999999999999\n";
    const ProcessIoDetailsInfo info = parseIoDetails(io);
    CHECK(info.chars_read.has_value() &&
          *info.chars_read == 18446744073709551615ULL);
    CHECK(info.chars_written.has_value() && *info.chars_written == 9999999999999999ULL);
  }

  run("parseIoDetails: partial fields");
  {
    const ProcessIoDetailsInfo info = parseIoDetails(partialIo());
    CHECK(info.chars_read.has_value() && *info.chars_read == 100);
    CHECK(info.chars_written.has_value() && *info.chars_written == 200);
    CHECK(!info.read_syscalls.has_value());
    CHECK(!info.write_syscalls.has_value());
    CHECK(!info.bytes_read.has_value());
    CHECK(!info.bytes_written.has_value());
    CHECK(!info.cancelled_write_bytes.has_value());
  }

  // ----------------------------------------------------------- Parsing edge
  run("parseIoDetails: extra whitespace");
  {
    const std::string io = "rchar: \t  42\nwchar:    99\n";
    const ProcessIoDetailsInfo info = parseIoDetails(io);
    CHECK(info.chars_read.has_value() && *info.chars_read == 42);
    CHECK(info.chars_written.has_value() && *info.chars_written == 99);
  }

  run("parseIoDetails: empty lines are tolerated");
  {
    const std::string io = "\nrchar: 10\n\nwchar: 20\n\n";
    const ProcessIoDetailsInfo info = parseIoDetails(io);
    CHECK(info.chars_read.has_value() && *info.chars_read == 10);
    CHECK(info.chars_written.has_value() && *info.chars_written == 20);
  }

  run("parseIoDetails: unknown fields are ignored");
  {
    const std::string io = "FUTURE_FIELD: 42\nrchar: 7\nanother_future: x\n";
    const ProcessIoDetailsInfo info = parseIoDetails(io);
    CHECK(info.chars_read.has_value() && *info.chars_read == 7);
    CHECK(!info.chars_written.has_value());
  }

  run("parseIoDetails: missing fields");
  {
    const ProcessIoDetailsInfo info = parseIoDetails("Name: test\n");
    CHECK(!info.chars_read.has_value());
    CHECK(!info.chars_written.has_value());
    CHECK(!info.read_syscalls.has_value());
    CHECK(!info.write_syscalls.has_value());
    CHECK(!info.bytes_read.has_value());
    CHECK(!info.bytes_written.has_value());
    CHECK(!info.cancelled_write_bytes.has_value());
  }

  run("parseIoDetails: field order changes are handled");
  {
    const std::string io =
        "write_bytes: 999\n"
        "cancelled_write_bytes: 7\n"
        "rchar: 111\n"
        "syscw: 22\n"
        "read_bytes: 888\n"
        "wchar: 222\n"
        "syscr: 11\n";
    const ProcessIoDetailsInfo info = parseIoDetails(io);
    CHECK(info.chars_read.has_value() && *info.chars_read == 111);
    CHECK(info.chars_written.has_value() && *info.chars_written == 222);
    CHECK(info.read_syscalls.has_value() && *info.read_syscalls == 11);
    CHECK(info.write_syscalls.has_value() && *info.write_syscalls == 22);
    CHECK(info.bytes_read.has_value() && *info.bytes_read == 888);
    CHECK(info.bytes_written.has_value() && *info.bytes_written == 999);
    CHECK(info.cancelled_write_bytes.has_value() &&
          *info.cancelled_write_bytes == 7);
  }

  // ----------------------------------------------------------- Invalid data
  run("parseIoDetails: missing colon is tolerated");
  {
    const std::string io = "rchar 42\nwchar 99\n";
    const ProcessIoDetailsInfo info = parseIoDetails(io);
    CHECK(!info.chars_read.has_value());
    CHECK(!info.chars_written.has_value());
  }

  run("parseIoDetails: empty numeric value");
  {
    const std::string io = "rchar:\nwchar:\n";
    const ProcessIoDetailsInfo info = parseIoDetails(io);
    CHECK(!info.chars_read.has_value());
    CHECK(!info.chars_written.has_value());
  }

  run("parseIoDetails: non-numeric value");
  {
    const std::string io = "rchar: abc\nwchar: xyz\nsyscr: notanumber\n";
    const ProcessIoDetailsInfo info = parseIoDetails(io);
    CHECK(!info.chars_read.has_value());
    CHECK(!info.chars_written.has_value());
    CHECK(!info.read_syscalls.has_value());
  }

  run("parseIoDetails: negative value is rejected");
  {
    const std::string io = "rchar: -1\n";
    const ProcessIoDetailsInfo info = parseIoDetails(io);
    CHECK(!info.chars_read.has_value());
  }

  run("parseIoDetails: truncated input");
  {
    const std::string io = "rchar: 100\nwchar: 200\nread_byte";
    const ProcessIoDetailsInfo info = parseIoDetails(io);
    CHECK(info.chars_read.has_value() && *info.chars_read == 100);
    CHECK(info.chars_written.has_value() && *info.chars_written == 200);
  }

  run("parseIoDetails: empty input");
  {
    const ProcessIoDetailsInfo info = parseIoDetails("");
    CHECK(!info.chars_read.has_value());
    CHECK(!info.chars_written.has_value());
    CHECK(!info.read_syscalls.has_value());
    CHECK(!info.write_syscalls.has_value());
    CHECK(!info.bytes_read.has_value());
    CHECK(!info.bytes_written.has_value());
    CHECK(!info.cancelled_write_bytes.has_value());
  }

  run("parseIoDetails: duplicate fields keep last value");
  {
    const std::string io = "rchar: 10\nrchar: 20\n";
    const ProcessIoDetailsInfo info = parseIoDetails(io);
    CHECK(info.chars_read.has_value() && *info.chars_read == 20);
  }

  // ------------------------------------------------------ ioDetailsStatusMessage
  run("ioDetailsStatusMessage: covers all statuses");
  {
    CHECK(std::string(ioDetailsStatusMessage(IoDetailsStatus::Success))
              .find("loaded") != std::string::npos);
    CHECK(std::string(ioDetailsStatusMessage(IoDetailsStatus::InvalidPid))
              .find("Invalid") != std::string::npos);
    CHECK(std::string(ioDetailsStatusMessage(IoDetailsStatus::ProcessNotFound))
              .find("no longer") != std::string::npos);
    CHECK(
        std::string(ioDetailsStatusMessage(IoDetailsStatus::IdentityUnknown))
            .find("could not") != std::string::npos);
    CHECK(std::string(ioDetailsStatusMessage(IoDetailsStatus::ProcessReused))
              .find("reused") != std::string::npos);
    CHECK(std::string(ioDetailsStatusMessage(IoDetailsStatus::PermissionDenied))
              .find("Permission denied") != std::string::npos);
    CHECK(std::string(ioDetailsStatusMessage(IoDetailsStatus::ReadError))
              .find("Failed") != std::string::npos);
  }

  // --------------------------------------------------------- Storage I/O fields
  run("parseIoDetails: storage I/O fields (read_bytes / write_bytes)");
  {
    const ProcessIoDetailsInfo info = parseIoDetails(sampleIo());
    CHECK(info.bytes_read.has_value() && *info.bytes_read == 4096000);
    CHECK(info.bytes_written.has_value() && *info.bytes_written == 8192000);
  }

  // ---- Integration with existing parseProcessIo (storage fields added) ---
  run("parseProcessIo: storage_read_bytes and storage_write_bytes parsed");
  {
    const ProcessIoCounters counters = parseProcessIo(sampleIo());
    CHECK(counters.available);
    CHECK(counters.read_bytes == 12345678);
    CHECK(counters.write_bytes == 87654321);
    CHECK(counters.read_syscalls == 500);
    CHECK(counters.write_syscalls == 300);
    CHECK(counters.cancelled_write_bytes == 512);
    CHECK(counters.storage_read_bytes == 4096000);
    CHECK(counters.storage_write_bytes == 8192000);
  }

  // ---- Storage rates in IoRates / computeIoRates -------------------------
  run("computeIoRates: storage rates computed from deltas");
  {
    const ProcessIoCounters prev{true, 1000, 2000, 10, 20, 0, 500, 800};
    const ProcessIoCounters curr{true, 3000, 5000, 30, 40, 0, 1500, 2400};
    const IoRates rates = computeIoRates(true, prev, curr, 2.0);
    CHECK_NEAR(rates.read_rate, 1000.0, 1e-6);
    CHECK_NEAR(rates.write_rate, 1500.0, 1e-6);
    CHECK_NEAR(rates.storage_read_rate, 500.0, 1e-6);
    CHECK_NEAR(rates.storage_write_rate, 800.0, 1e-6);
  }

  run("computeIoRates: storage counter reset yields zero");
  {
    const ProcessIoCounters prev{true, 1000, 2000, 10, 20, 0, 5000, 800};
    const ProcessIoCounters curr{true, 3000, 5000, 30, 40, 0, 1000, 900};
    const IoRates rates = computeIoRates(true, prev, curr, 1.0);
    CHECK_NEAR(rates.storage_read_rate, 0.0, 1e-6);   // reset
    CHECK_NEAR(rates.storage_write_rate, 100.0, 1e-6);
  }

  run("computeIoRates: identity change zeroes all rates including storage");
  {
    const ProcessIoCounters prev{true, 1000, 2000, 10, 20, 0, 500, 800};
    const ProcessIoCounters curr{true, 3000, 5000, 30, 40, 0, 1500, 2400};
    const IoRates rates = computeIoRates(false, prev, curr, 1.0);
    CHECK_NEAR(rates.read_rate, 0.0, 1e-6);
    CHECK_NEAR(rates.write_rate, 0.0, 1e-6);
    CHECK_NEAR(rates.storage_read_rate, 0.0, 1e-6);
    CHECK_NEAR(rates.storage_write_rate, 0.0, 1e-6);
  }

  // ----------------------------------------------------------- Live manager
  run("manager: inspecting a live child process succeeds");
  {
    const pid_t child = spawnSleep();
    const bool exec_done = child >= 0 && waitForExec(child);
    CHECK(child >= 0);
    if (child < 0) {
      return EXIT_FAILURE;
    }
    CHECK(exec_done);
    const ProcessIoDetailsManager manager;
    if (!exec_done) {
      ::kill(child, SIGKILL);
      ::waitpid(child, nullptr, 0);
    } else {
      const std::optional<ProcessIdentity> identity =
          ProcessIdentity::current(child);
      CHECK(identity.has_value());
      if (identity) {
        const ProcessIoDetailsResult result = manager.inspect(*identity);
        ::kill(child, SIGKILL);
        ::waitpid(child, nullptr, 0);

        CHECK(result.success());
        CHECK(result.info.chars_read.has_value());
        CHECK(result.info.chars_written.has_value());
        CHECK(result.info.read_syscalls.has_value());
        CHECK(result.info.write_syscalls.has_value());
        CHECK(result.info.bytes_read.has_value());
        CHECK(result.info.bytes_written.has_value());
      }
    }
  }

  run("manager: inspecting the application itself works");
  {
    const std::optional<ProcessIdentity> self =
        ProcessIdentity::current(getpid());
    CHECK(self.has_value());
    if (!self) {
      return EXIT_FAILURE;
    }
    const ProcessIoDetailsManager manager;
    const ProcessIoDetailsResult result = manager.inspect(*self);
    CHECK(result.success());
    CHECK(result.info.chars_read.has_value());
    CHECK(result.info.chars_written.has_value());
  }

  // --------------------------------------------------- Identity gate / reuse
  run("manager: PID reuse / forged identity aborts the inspection");
  {
    const pid_t self_pid = getpid();
    const std::optional<ProcessIdentity> self =
        ProcessIdentity::current(self_pid);
    CHECK(self.has_value());
    if (!self) {
      return EXIT_FAILURE;
    }
    const ProcessIoDetailsManager manager;
    ProcessIdentity forged{self_pid, self->starttime_ticks + 1ULL};
    const ProcessIoDetailsResult result = manager.inspect(forged);
    CHECK(result.status == IoDetailsStatus::ProcessReused);
  }

  run("manager: invalid PID is rejected before any read");
  {
    const ProcessIoDetailsManager manager;
    const ProcessIoDetailsResult zero =
        manager.inspect(ProcessIdentity{0, 1});
    CHECK(zero.status == IoDetailsStatus::InvalidPid);
    const ProcessIoDetailsResult negative =
        manager.inspect(ProcessIdentity{-5, 1});
    CHECK(negative.status == IoDetailsStatus::InvalidPid);
  }

  // ------------------------------------------------------ Process lifetime
  run("manager: a disappeared process is reported, never crashes");
  {
    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
      ::_exit(0);
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    const ProcessIoDetailsManager manager;
    const ProcessIoDetailsResult result =
        manager.inspect(ProcessIdentity{child, 12345});
    CHECK(result.status == IoDetailsStatus::IdentityUnknown ||
          result.status == IoDetailsStatus::ProcessNotFound);
    CHECK(!result.success());
  }

  // ----------------------------------------------- Async stale-result safety
  run("manager: results are bound to the selected process only");
  {
    const pid_t child = spawnSleep();
    const bool exec_done = child >= 0 && waitForExec(child);
    CHECK(child >= 0);
    if (child < 0) {
      return EXIT_FAILURE;
    }
    CHECK(exec_done);
    const ProcessIoDetailsManager manager;
    if (!exec_done) {
      ::kill(child, SIGKILL);
      ::waitpid(child, nullptr, 0);
    } else {
      const std::optional<ProcessIdentity> child_identity =
          ProcessIdentity::current(child);
      CHECK(child_identity.has_value());
      if (child_identity) {
        ProcessIdentity stale{child, child_identity->starttime_ticks + 1ULL};
        const ProcessIoDetailsResult stale_result = manager.inspect(stale);
        CHECK(stale_result.status == IoDetailsStatus::ProcessReused);

        const ProcessIoDetailsResult fresh_result =
            manager.inspect(*child_identity);
        CHECK(fresh_result.status == IoDetailsStatus::Success);

        ::kill(child, SIGKILL);
        ::waitpid(child, nullptr, 0);
      }
    }
  }

  // ------------------------------------------------------ Rate defaults
  run("ProcessIoDetailsInfo: rates default to zero");
  {
    const ProcessIoDetailsInfo info = parseIoDetails("");
    CHECK_NEAR(info.chars_read_rate, 0.0, 1e-6);
    CHECK_NEAR(info.chars_written_rate, 0.0, 1e-6);
    CHECK_NEAR(info.bytes_read_rate, 0.0, 1e-6);
    CHECK_NEAR(info.bytes_written_rate, 0.0, 1e-6);
  }

  run("ProcessIoDetailsResult: success() returns true only for Success");
  {
    ProcessIoDetailsResult r;
    r.status = IoDetailsStatus::Success;
    CHECK(r.success());
    r.status = IoDetailsStatus::ReadError;
    CHECK(!r.success());
  }

  std::fprintf(stderr, "\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
