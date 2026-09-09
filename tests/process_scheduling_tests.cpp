#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

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

void run(const char *name) {
  std::fprintf(stderr, "TEST %s\n", name);
}
}  // namespace

#define CHECK(expr) ::expect((expr), #expr, __FILE__, __LINE__)

int main() {
  using namespace atm;

  atm::ProcessSchedulingManager scheduling;
  const ::pid_t self_pid = ::getpid();
  const std::optional<ProcessIdentity> self =
      ProcessIdentity::current(self_pid);
  if (!self) {
    std::fprintf(stderr, "SKIP: cannot read own /proc identity\n");
    return EXIT_FAILURE;
  }

  // --- Online CPU count. --------------------------------------------------
  run("system cpu count is positive");
  CHECK(ProcessSchedulingManager::systemCpuCount() >= 1);

  // --- formatCpuList / parseCpuSelection. --------------------------------
  run("formatCpuList joins CPU ids with commas");
  CHECK(formatCpuList({0, 2, 3}) == "0,2,3");
  CHECK(formatCpuList({2}) == "2");

  run("parseCpuSelection: single CPU");
  {
    const auto cpus = parseCpuSelection("2", 8);
    CHECK(cpus.has_value());
    CHECK(*cpus == std::vector<int>{2});
  }

  run("parseCpuSelection: multiple and ranged CPUs");
  {
    const auto cpus = parseCpuSelection("0,1,3", 8);
    CHECK(cpus.has_value());
    CHECK(*cpus == std::vector<int>({0, 1, 3}));

    const auto ranged = parseCpuSelection("0-3", 8);
    CHECK(ranged.has_value());
    CHECK(*ranged == std::vector<int>({0, 1, 2, 3}));

    const auto spaced = parseCpuSelection("0 2 3", 8);
    CHECK(spaced.has_value());
    CHECK(*spaced == std::vector<int>({0, 2, 3}));

    const auto mixed = parseCpuSelection("0-1, 3-4", 8);
    CHECK(mixed.has_value());
    CHECK(*mixed == std::vector<int>({0, 1, 3, 4}));
  }

  run("parseCpuSelection: deduplicates and sorts");
  {
    const auto cpus = parseCpuSelection("3,0,0,1", 8);
    CHECK(cpus.has_value());
    CHECK(*cpus == std::vector<int>({0, 1, 3}));
  }

  run("parseCpuSelection: empty or all-invalid input is rejected");
  {
    CHECK(!parseCpuSelection("", 8).has_value());
    CHECK(!parseCpuSelection(", ,", 8).has_value());
    CHECK(!parseCpuSelection("abc", 8).has_value());
    CHECK(!parseCpuSelection("0-", 8).has_value());
    CHECK(!parseCpuSelection("-3", 8).has_value());
  }

  run("parseCpuSelection: out-of-range and malformed IDs are rejected");
  {
    const int online = ProcessSchedulingManager::systemCpuCount();
    CHECK(!parseCpuSelection(std::to_string(online), online).has_value());
    CHECK(!parseCpuSelection("-1", online).has_value());
    CHECK(!parseCpuSelection(std::to_string(online * 100), online).has_value());
    CHECK(!parseCpuSelection("2-1", 8).has_value());
    CHECK(!parseCpuSelection("0-3,9", 8).has_value());  // 9 is invalid here
    CHECK(parseCpuSelection("0-3,", 5).has_value());    // trailing comma: ok
    CHECK(!parseCpuSelection("3,50", 5).has_value());
  }

  // --- Nice priority. -----------------------------------------------------
  run("getNice reads the current value");
  {
    const std::optional<int> nice = scheduling.getNice(self_pid);
    CHECK(nice.has_value());
    CHECK(nice.value() >= kMinimumNice);
    CHECK(nice.value() <= kMaximumNice);
  }

  run("getNice of a nonexistent process is nullopt");
  {
    // Fork a child that exits immediately and reap it: its PID is gone.
    const ::pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
      _exit(0);
    }
    int status = 0;
    static_cast<void>(::waitpid(child, &status, 0));
    CHECK(!scheduling.getNice(child).has_value());
  }

  run("setNice rejects invalid values before any syscall");
  {
    CHECK(scheduling.setNice(*self, kMaximumNice + 1).status ==
          SchedulingStatus::InvalidNice);
    CHECK(scheduling.setNice(*self, kMinimumNice - 1).status ==
          SchedulingStatus::InvalidNice);
    CHECK(scheduling.setNice(ProcessIdentity{}, 0).status ==
          SchedulingStatus::InvalidPid);
    CHECK(scheduling.setNice(*self, 0).status !=
          SchedulingStatus::InvalidPid);
  }

  run("setNice to the current value is a no-change success");
  {
    const std::optional<int> current = scheduling.getNice(self_pid);
    CHECK(current.has_value());
    const SchedulingResult result = scheduling.setNice(*self, *current);
    CHECK(result.success());
    const std::optional<int> after = scheduling.getNice(self_pid);
    CHECK(after.has_value());
    CHECK(*after == *current);
  }

  run("setNice refuses to modify a reused PID (identity mismatch)");
  {
    // Same PID but a different start time: exactly the PID-reuse hazard.
    const ProcessIdentity reused{self->pid, self->starttime_ticks + 1};
    CHECK(scheduling.setNice(reused, 0).status ==
          SchedulingStatus::ProcessReused);
  }

  run("setNice reports IdentityUnknown for a vanished process");
  {
    const ::pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
      _exit(0);
    }
    int status = 0;
    static_cast<void>(::waitpid(child, &status, 0));
    const ProcessIdentity gone{child, 0};
    CHECK(scheduling.setNice(gone, 0).status ==
              SchedulingStatus::IdentityUnknown ||
          scheduling.setNice(gone, 0).status ==
              SchedulingStatus::ProcessReused);
  }

  run("setNice round-trips a real change reflected on read");
  {
    const std::optional<int> current = scheduling.getNice(self_pid);
    CHECK(current.has_value());
    if (*current < kMaximumNice) {
      const SchedulingResult result = scheduling.setNice(*self, *current + 1);
      CHECK(result.success());
      const std::optional<int> after = scheduling.getNice(self_pid);
      CHECK(after.has_value());
      CHECK(*after == *current + 1);
    }
  }

  run("setNice EPERM is mapped for requests beyond the caller's limit");
  {
    // A normal user may raise this process's nice but not lower it below the
    // current value; a root user may. Gate the expectation on privileges.
    const std::optional<int> current = scheduling.getNice(self_pid);
    CHECK(current.has_value());
    if (*current > kMinimumNice) {
      const SchedulingResult result = scheduling.setNice(*self, *current - 1);
      if (::geteuid() == 0) {
        CHECK(result.status == SchedulingStatus::Success);
      } else {
        CHECK(result.status == SchedulingStatus::PermissionDenied);
      }
    }
  }

  // --- CPU affinity. ------------------------------------------------------
  run("getCpuAffinity reads a non-empty, sorted list");
  {
    const std::optional<std::vector<int>> affinity =
        scheduling.getCpuAffinity(self_pid);
    CHECK(affinity.has_value());
    CHECK(!affinity->empty());
    for (std::size_t i = 1; i < affinity->size(); ++i) {
      CHECK((*affinity)[i - 1] < (*affinity)[i]);
    }
    const int online = ProcessSchedulingManager::systemCpuCount();
    for (const int cpu : *affinity) {
      CHECK(cpu >= 0 && cpu < online);
    }
  }

  run("setCpuAffinity rejects empty masks");
  {
    CHECK(scheduling.setCpuAffinity(*self, {}).status ==
          SchedulingStatus::EmptyAffinity);
  }

  run("setCpuAffinity rejects invalid CPU ids before the syscall");
  {
    const int online = ProcessSchedulingManager::systemCpuCount();
    CHECK(scheduling.setCpuAffinity(*self, {online}).status ==
          SchedulingStatus::InvalidCpu);
    CHECK(scheduling.setCpuAffinity(*self, {-1}).status ==
          SchedulingStatus::InvalidCpu);
    CHECK(scheduling.setCpuAffinity(ProcessIdentity{}, {0}).status ==
          SchedulingStatus::InvalidPid);
  }

  run("setCpuAffinity to the current mask is a no-change success");
  {
    const std::optional<std::vector<int>> current =
        scheduling.getCpuAffinity(self_pid);
    CHECK(current.has_value());
    CHECK(scheduling.setCpuAffinity(*self, *current).success());
  }

  run("setCpuAffinity refuses to modify a reused PID (identity mismatch)");
  {
    const std::optional<std::vector<int>> current =
        scheduling.getCpuAffinity(self_pid);
    CHECK(current.has_value());
    const ProcessIdentity reused{self->pid, self->starttime_ticks + 1};
    CHECK(scheduling.setCpuAffinity(reused, *current).status ==
          SchedulingStatus::ProcessReused);
  }

  run("setCpuAffinity single-CPU change is reflected on read");
  {
    const std::optional<std::vector<int>> original =
        scheduling.getCpuAffinity(self_pid);
    CHECK(original.has_value());
    if (original->size() > 1) {
      const std::vector<int> single{original->front()};
      const SchedulingResult result =
          scheduling.setCpuAffinity(*self, single);
      CHECK(result.success());
      const std::optional<std::vector<int>> after =
          scheduling.getCpuAffinity(self_pid);
      CHECK(after.has_value());
      CHECK(*after == single);
      // Restore the original mask so later tests keep full scheduling freedom.
      const SchedulingResult restored =
          scheduling.setCpuAffinity(*self, *original);
      CHECK(restored.success());
      const std::optional<std::vector<int>> back =
          scheduling.getCpuAffinity(self_pid);
      CHECK(back.has_value());
      CHECK(*back == *original);
    }
  }

  // --- ProcessIdentity. ---------------------------------------------------
  run("ProcessIdentity::current rejects invalid pids");
  {
    CHECK(!ProcessIdentity::current(0).has_value());
    CHECK(!ProcessIdentity::current(-5).has_value());
  }

  run("ProcessIdentity::current matches on the same process");
  {
    const std::optional<ProcessIdentity> again =
        ProcessIdentity::current(self_pid);
    CHECK(again.has_value());
    CHECK(again->pid == self_pid);
    CHECK(again->starttime_ticks == self->starttime_ticks);
  }

  std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
  if (g_failures != 0) {
    return EXIT_FAILURE;
  }
  std::fprintf(stderr, "ALL TESTS PASSED\n");
  return EXIT_SUCCESS;
}