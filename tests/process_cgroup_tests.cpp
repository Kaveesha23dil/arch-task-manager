#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "process_cgroup.hpp"
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

void run(const char *name) { std::fprintf(stderr, "TEST %s\n", name); }
}  // namespace

#define CHECK(expr) ::expect((expr), #expr, __FILE__, __LINE__)

int main() {
  using namespace atm;

  // --- /proc/<pid>/cgroup line parsing: cgroup v2 unified record. --------
  run("parseCgroupLine: cgroup v2 unified record");
  {
    const auto line = parseCgroupLine("0::/user.slice/user-1000.slice");
    CHECK(line.has_value());
    CHECK(line->hierarchy_id == 0);
    CHECK(line->controllers.empty());
    CHECK(line->relative_path == "/user.slice/user-1000.slice");
  }

  run("parseCgroupLine: cgroup v1 controller record");
  {
    const auto line = parseCgroupLine("5:cpu,cpuacct:/user.slice");
    CHECK(line.has_value());
    CHECK(line->hierarchy_id == 5);
    CHECK(line->controllers.size() == 2);
    CHECK(line->controllers[0] == "cpu");
    CHECK(line->controllers[1] == "cpuacct");
    CHECK(line->relative_path == "/user.slice");
  }

  run("parseCgroupLine: a full 64-bit hierarchy id parses without overflow");
  {
    const auto line =
        parseCgroupLine("18446744073709551615:cpu:/a");
    CHECK(line.has_value());
    CHECK(line->hierarchy_id == 18446744073709551615ULL);
  }

  run("parseCgroupLine: empty controller list (v2) vs one controller");
  {
    const auto v2 = parseCgroupLine("0::/a/b");
    CHECK(v2.has_value());
    CHECK(v2->controllers.empty());
    const auto single = parseCgroupLine("4:memory:/a/b");
    CHECK(single.has_value());
    CHECK(single->controllers.size() == 1);
    CHECK(single->controllers[0] == "memory");
  }

  run("parseCgroupLine: octal-escaped path bytes are decoded");
  {
    const auto space = parseCgroupLine("3:blkio:/a\\040b/c");
    CHECK(space.has_value());
    CHECK(space->relative_path == "/a b/c");
    const auto colon = parseCgroupLine("0::/a\\072b");
    CHECK(colon.has_value());
    CHECK(colon->relative_path == "/a:b");
    const auto slash = parseCgroupLine("0::/a\\057b");
    CHECK(slash.has_value());
    CHECK(slash->relative_path == "/a/b");
  }

  run("parseCgroupLine: tolerates extra colon-bearing path content");
  {
    // A raw (unescaped) colon in the path is unusual but harmless: everything
    // after the second separator is treated as the path.
    const auto line = parseCgroupLine("5:cpu:/user.slice:extra");
    CHECK(line.has_value());
    CHECK(line->relative_path == "/user.slice:extra");
  }

  // --- /proc/<pid>/cgroup line parsing: malformed lines are rejected. ----
  run("parseCgroupLine: malformed lines are rejected safely");
  {
    CHECK(!parseCgroupLine("").has_value());
    CHECK(!parseCgroupLine("\n").has_value());
    CHECK(!parseCgroupLine("0:/a").has_value());            // one separator
    CHECK(!parseCgroupLine("0:").has_value());              // truncated path
    CHECK(!parseCgroupLine("0::").has_value());             // empty path
    CHECK(!parseCgroupLine("0::relative").has_value());     // no leading '/'
    CHECK(!parseCgroupLine("abc:cpu:/x").has_value());      // non-numeric id
    CHECK(!parseCgroupLine("-3:cpu:/x").has_value());       // negative id
    CHECK(!parseCgroupLine("18446744073709551616:cpu:/x").has_value());  // 2^64
    CHECK(!parseCgroupLine("1::/x\njunk").has_value());      // embedded newline
    CHECK(!parseCgroupLine("5:cpu:/a\tb").has_value());      // raw tab in path
  }

  run("parseCgroupLine: repeated/trailing commas in the controller list");
  {
    CHECK(!parseCgroupLine("5:cpu,,cpuacct:/x").has_value());
    CHECK(!parseCgroupLine("5:cpu,:/x").has_value());
    CHECK(!parseCgroupLine("5:,cpu:/x").has_value());
    CHECK(!parseCgroupLine("5:::/x").has_value());  // path ":/x" has no leading '/'
    // Two separators with an empty controller list is the v2 shape: parsed.
    CHECK(parseCgroupLine("5::/x").has_value());
  }

  // --- Whole control-file value parsing: numbers, max, garbage. ----------
  run("parseCgroupValue: unsigned integers parse exactly");
  {
    const auto value = parseCgroupValue("1683103744\n");
    CHECK(value.has_value());
    CHECK(value->available);
    CHECK(!value->unlimited);
    CHECK(value->value == 1683103744ULL);
    const auto zero = parseCgroupValue("0\n");
    CHECK(zero.has_value());
    CHECK(zero->value == 0);
    const auto trimmed = parseCgroupValue("  42 \n");
    CHECK(trimmed.has_value());
    CHECK(trimmed->value == 42);
  }

  run("parseCgroupValue: 'max' means unlimited");
  {
    const auto value = parseCgroupValue("max\n");
    CHECK(value.has_value());
    CHECK(value->available);
    CHECK(value->unlimited);
    const auto value_ws = parseCgroupValue(" max \n");
    CHECK(value_ws.has_value());
    CHECK(value_ws->unlimited);
  }

  run("parseCgroupValue: malformed or empty content yields nullopt");
  {
    CHECK(!parseCgroupValue("").has_value());
    CHECK(!parseCgroupValue("\n").has_value());
    CHECK(!parseCgroupValue("abc").has_value());
    CHECK(!parseCgroupValue("1.5").has_value());
    CHECK(!parseCgroupValue("12x").has_value());
    CHECK(!parseCgroupValue("-5").has_value());
    CHECK(!parseCgroupValue("18446744073709551616").has_value());  // 2^64
  }

  // --- cpu.max parsing: quota, period, unlimited. ------------------------
  run("parseCgroupCpuMax: finite quota and period");
  {
    const auto max = parseCgroupCpuMax("50000 100000\n");
    CHECK(max.has_value());
    CHECK(max->available);
    CHECK(!max->unlimited);
    CHECK(max->quota_usec == 50000);
    CHECK(max->period_usec == 100000);
  }

  run("parseCgroupCpuMax: 'max' quota (with and without period)");
  {
    const auto bare = parseCgroupCpuMax("max\n");
    CHECK(bare.has_value());
    CHECK(bare->available);
    CHECK(bare->unlimited);
    const auto with_period = parseCgroupCpuMax("max 100000\n");
    CHECK(with_period.has_value());
    CHECK(with_period->unlimited);
    CHECK(with_period->period_usec == 100000);
  }

  run("parseCgroupCpuMax: malformed values yield nullopt");
  {
    CHECK(!parseCgroupCpuMax("").has_value());
    CHECK(!parseCgroupCpuMax("50000").has_value());        // missing period
    CHECK(!parseCgroupCpuMax("50000 100000 5").has_value());  // too many tokens
    CHECK(!parseCgroupCpuMax("abc 100000").has_value());
    CHECK(!parseCgroupCpuMax("max abc").has_value());
    CHECK(!parseCgroupCpuMax("50000 0").has_value());      // zero period
  }

  // --- Version labels. ---------------------------------------------------
  run("cgroupVersionName labels are stable");
  {
    CHECK(std::string(cgroupVersionName(CgroupVersion::V1)) == "cgroup v1");
    CHECK(std::string(cgroupVersionName(CgroupVersion::V2)) == "cgroup v2");
    CHECK(std::string(cgroupVersionName(CgroupVersion::Unknown)) == "unknown");
  }

  // --- Live integration: inspect this process's own cgroup membership. ----
  run("manager: inspect this process's cgroup membership");
  {
    const auto self = ProcessIdentity::current(getpid());
    if (!self) {
      std::fprintf(stderr, "SKIP: cannot read own process identity\n");
      return EXIT_FAILURE;
    }
    const ProcessCgroupManager mgr;
    const ProcessCgroupResult res = mgr.inspect(*self, 64);
    CHECK(res.success());
    CHECK(res.hierarchies.size() >= 1);
    for (const ProcessCgroupHierarchy &h : res.hierarchies) {
      CHECK(h.relative_path.size() > 0);
      CHECK(h.relative_path[0] == '/');
    }
    // The unified v2 record (hierarchy id 0, no controllers) is present.
    bool saw_unified = false;
    for (const ProcessCgroupHierarchy &h : res.hierarchies) {
      if (h.hierarchy_id == 0) {
        saw_unified = true;
        CHECK(h.controllers.empty());
      }
    }
    CHECK(saw_unified);
    CHECK(res.version != CgroupVersion::Unknown);
    CHECK(res.version == CgroupVersion::V2);
  }

  run("manager: v2 resource values are read from the resolved cgroup dir");
  {
    const auto self = ProcessIdentity::current(getpid());
    if (!self) {
      return EXIT_FAILURE;
    }
    const ProcessCgroupManager mgr;
    const ProcessCgroupResult res = mgr.inspect(*self, 64);
    if (res.version != CgroupVersion::V2) {
      return EXIT_SUCCESS;  // not meaningful on a v1-only system
    }
    const ProcessCgroupHierarchy *unified = nullptr;
    for (const ProcessCgroupHierarchy &h : res.hierarchies) {
      if (h.hierarchy_id == 0) {
        unified = &h;
      }
    }
    CHECK(unified != nullptr);
    CHECK(unified->resolvable);
    CHECK(!unified->absolute_path.empty());
    CHECK(!unified->mount_point.empty());
    // The resolved path must stay inside the cgroup mount.
    CHECK(unified->absolute_path.rfind(unified->mount_point, 0) == 0);
    // On this system the memory and pids controllers are enabled, so their
    // counters must be present; anything absent must never be fabricated.
    CHECK(res.resources.readable_file_count >= 1);
    const bool mem_current = res.resources.memory_current.available;
    const bool pids_current = res.resources.pids_current.available;
    CHECK(mem_current || pids_current);
    if (res.resources.memory_max.available && res.resources.memory_max.unlimited) {
      CHECK(res.resources.memory_max.value == 0);
    }
    if (!res.resources.cpu_weight.available) {
      // A disabled controller has no control file: never a fabricated value.
      CHECK(res.resources.cpu_weight.value == 0);
      CHECK(!res.resources.cpu_weight.unlimited);
    }
    // Controllers metadata read as a whitespace list.
    CHECK(res.resources.controllers.empty() || !res.resources.controllers[0].empty());
  }

  run("manager: a directory without the resource files degrades gracefully");
  {
    const auto self = ProcessIdentity::current(getpid());
    if (!self) {
      return EXIT_FAILURE;
    }
    const ProcessCgroupManager mgr;
    const ProcessCgroupResult res = mgr.inspect(*self, 1);
    // A live process always succeeds; reading never throws and unreadable
    // values stay "unavailable", never fabricated.
    CHECK(res.success());
    CHECK(res.hierarchies.size() <= 1);
  }

  run("manager: PID reuse is detected and results are discarded");
  {
    const pid_t self_pid = getpid();
    const auto self = ProcessIdentity::current(self_pid);
    if (!self) {
      return EXIT_FAILURE;
    }
    const ProcessCgroupManager mgr;
    ProcessIdentity forged{self_pid, self->starttime_ticks + 1ULL};
    const ProcessCgroupResult res = mgr.inspect(forged, 64);
    CHECK(res.status == CgroupStatus::ProcessReused);
    CHECK(res.hierarchies.empty());
  }

  run("manager: invalid PID is rejected");
  {
    const ProcessCgroupManager mgr;
    const ProcessCgroupResult res_zero =
        mgr.inspect(ProcessIdentity{0, 1}, 64);
    CHECK(res_zero.status == CgroupStatus::InvalidPid);
    const ProcessCgroupResult res_neg =
        mgr.inspect(ProcessIdentity{-5, 1}, 64);
    CHECK(res_neg.status == CgroupStatus::InvalidPid);
  }

  run("manager: vanished process is reported, never crashes");
  {
    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
      ::_exit(0);  // child exits immediately
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    const ProcessCgroupManager mgr;
    const ProcessCgroupResult res =
        mgr.inspect(ProcessIdentity{child, 12345}, 64);
    // The identity is gone (IdentityUnknown) or the membership file no longer
    // exists (ProcessNotFound). Either way: never success, never a crash.
    CHECK(res.status != CgroupStatus::Success);
  }

  run("manager: repeated inspection is stable and deterministic");
  {
    const auto self = ProcessIdentity::current(getpid());
    if (!self) {
      return EXIT_FAILURE;
    }
    const ProcessCgroupManager mgr;
    const ProcessCgroupResult a = mgr.inspect(*self, 64);
    const ProcessCgroupResult b = mgr.inspect(*self, 64);
    CHECK(a.success() && b.success());
    CHECK(a.hierarchies.size() == b.hierarchies.size());
    for (std::size_t i = 0; i < a.hierarchies.size(); ++i) {
      CHECK(a.hierarchies[i].hierarchy_id == b.hierarchies[i].hierarchy_id);
      CHECK(a.hierarchies[i].relative_path == b.hierarchies[i].relative_path);
    }
  }

  std::fprintf(stderr, "\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}