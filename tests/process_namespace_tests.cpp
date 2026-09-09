#include <dirent.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "process_namespace.hpp"
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

  // --- Target parsing: valid /proc/<pid>/ns symlink targets. ------------
  run("parseNamespaceTarget: valid IPv6-width IDs");
  {
    const char *target = "net:[4026531992]";
    const auto parsed = parseNamespaceTarget(target);
    CHECK(parsed.has_value());
    CHECK(parsed->type_name == "net");
    CHECK(parsed->id == 4026531992ULL);
  }

  run("parseNamespaceTarget: other known types");
  {
    const auto cg = parseNamespaceTarget("cgroup:[4026531835]");
    CHECK(cg.has_value());
    CHECK(cg->id == 4026531835ULL);
    const auto pid = parseNamespaceTarget("pid:[4026531836]");
    CHECK(pid.has_value());
    CHECK(pid->type_name == "pid");
    const auto mnt = parseNamespaceTarget("mnt:[4026531832]");
    CHECK(mnt.value().id == 4026531832ULL);
  }

  run("parseNamespaceTarget: identifier larger than signed 32-bit");
  {
    // 3e9 exceeds INT32_MAX; must parse without overflow into uint64.
    const auto parsed = parseNamespaceTarget("time:[3000000000]");
    CHECK(parsed.has_value());
    CHECK(parsed->id == 3000000000ULL);
  }

  run("parseNamespaceTarget: maximum uint64 identifier does not overflow");
  {
    const auto parsed =
        parseNamespaceTarget("user:[18446744073709551615]");
    CHECK(parsed.has_value());
    CHECK(parsed->id == 18446744073709551615ULL);
  }

  // --- Target parsing: malformed targets are rejected safely. -----------
  run("parseNamespaceTarget: reject empty / malformed identifiers");
  {
    CHECK(!parseNamespaceTarget("net:[]").has_value());
    CHECK(!parseNamespaceTarget("net:abc").has_value());
    CHECK(!parseNamespaceTarget("net:[abc]").has_value());
    CHECK(!parseNamespaceTarget("net:[-1]").has_value());
    CHECK(!parseNamespaceTarget("").has_value());
    CHECK(!parseNamespaceTarget("net").has_value());
    CHECK(!parseNamespaceTarget("net:[1").has_value());
    CHECK(!parseNamespaceTarget(":[5]").has_value());
    CHECK(!parseNamespaceTarget("net:[0]").has_value());       // zero id
    CHECK(!parseNamespaceTarget("net:[1]x").has_value());     // trailing junk
    CHECK(!parseNamespaceTarget("net:[1]net:[2]").has_value());
    CHECK(!parseNamespaceTarget("net:[]]").has_value());
  }

  // --- Type name mapping. -----------------------------------------------
  run("namespaceTypeFromName: all known names");
  {
    CHECK(namespaceTypeFromName("cgroup") == NamespaceType::Cgroup);
    CHECK(namespaceTypeFromName("ipc") == NamespaceType::Ipc);
    CHECK(namespaceTypeFromName("mnt") == NamespaceType::Mount);
    CHECK(namespaceTypeFromName("net") == NamespaceType::Network);
    CHECK(namespaceTypeFromName("pid") == NamespaceType::Pid);
    CHECK(namespaceTypeFromName("pid_for_children") ==
          NamespaceType::PidForChildren);
    CHECK(namespaceTypeFromName("time") == NamespaceType::Time);
    CHECK(namespaceTypeFromName("time_for_children") ==
          NamespaceType::TimeForChildren);
    CHECK(namespaceTypeFromName("user") == NamespaceType::User);
    CHECK(namespaceTypeFromName("uts") == NamespaceType::Uts);
  }

  run("namespaceTypeFromName: unexpected entries map to Unknown");
  {
    CHECK(namespaceTypeFromName("vpid") == NamespaceType::Unknown);
    CHECK(namespaceTypeFromName("NET") == NamespaceType::Unknown);
    CHECK(namespaceTypeFromName("") == NamespaceType::Unknown);
  }

  run("namespaceTypeName / short name labels are stable");
  {
    CHECK(std::string(namespaceTypeName(NamespaceType::Network)) == "Network");
    CHECK(std::string(namespaceTypeShortName(NamespaceType::Mount)) == "mnt");
    CHECK(std::string(namespaceTypeName(NamespaceType::PidForChildren)) ==
          "PID for Children");
    CHECK(std::string(namespaceTypeShortName(NamespaceType::PidForChildren)) ==
          "pid_for_children");
    CHECK(std::string(namespaceTypeName(NamespaceType::TimeForChildren)) ==
          "Time for Children");
    CHECK(std::string(namespaceTypeName(NamespaceType::Unknown)) == "Unknown");
    CHECK(std::string(namespaceTypeShortName(NamespaceType::Unknown)) == "");
  }

  // --- Live integration: enumerate the test process's own namespaces. ----
  run("manager: enumerate this process's namespaces");
  {
    const auto self = ProcessIdentity::current(getpid());
    if (!self) {
      std::fprintf(stderr, "SKIP: cannot read own process identity\n");
      return EXIT_FAILURE;
    }
    const ProcessNamespaceManager mgr;
    const ProcessNamespaceResult res = mgr.inspect(*self, 64);
    CHECK(res.success());
    // A live process always has namespaces; this kernel exposes at least the
    // handful of standard types.
    CHECK(res.detected_count >= 1);
    CHECK(res.namespaces.size() == res.detected_count);
    CHECK(res.detected_count <= 64);
    // Every record keeps its directory-entry name, and none are unavailable
    // when inspecting our own process.
    bool saw_known = false;
    for (const ProcessNamespace &ns : res.namespaces) {
      CHECK(!ns.name.empty());
      if (ns.type != NamespaceType::Unknown) {
        saw_known = true;
      }
      if (!ns.unavailable) {
        CHECK(!ns.target.empty());
        // The raw target round-trips through the parser: id present means the
        // target parsed to the same numeric identifier.
        if (ns.id.has_value()) {
          const auto parsed = parseNamespaceTarget(ns.target);
          CHECK(parsed.has_value());
          CHECK(parsed->id == *ns.id);
        }
      }
    }
    CHECK(saw_known);
    CHECK(res.unique_id_count >= 1);
    CHECK(res.unique_id_count <= res.detected_count);
    CHECK(res.unavailable_count == 0);
  }

  run("manager: pid vs pid_for_children share the same namespace ID");
  {
    const auto self = ProcessIdentity::current(getpid());
    if (!self) {
      return EXIT_FAILURE;
    }
    const ProcessNamespaceManager mgr;
    const ProcessNamespaceResult res = mgr.inspect(*self, 64);
    std::optional<std::uint64_t> pid_id;
    std::optional<std::uint64_t> pfc_id;
    for (const ProcessNamespace &ns : res.namespaces) {
      if (ns.type == NamespaceType::Pid && ns.id.has_value()) {
        pid_id = ns.id;
      }
      if (ns.type == NamespaceType::PidForChildren && ns.id.has_value()) {
        pfc_id = ns.id;
      }
    }
    // On any kernel that exposes both, pid_for_children is the same PID
    // namespace as pid, so the identifiers must match.
    if (pid_id.has_value() && pfc_id.has_value()) {
      CHECK(*pid_id == *pfc_id);
    }
  }

  run("manager: deterministic ordering and stable between refreshes");
  {
    const auto self = ProcessIdentity::current(getpid());
    if (!self) {
      return EXIT_FAILURE;
    }
    const ProcessNamespaceManager mgr;
    const ProcessNamespaceResult a = mgr.inspect(*self, 64);
    const ProcessNamespaceResult b = mgr.inspect(*self, 64);
    CHECK(a.success() && b.success());
    CHECK(a.namespaces.size() == b.namespaces.size());
    for (std::size_t i = 0; i < a.namespaces.size(); ++i) {
      CHECK(a.namespaces[i].name == b.namespaces[i].name);
    }
  }

  run("manager: a missing namespace type is not fatal");
  {
    const auto self = ProcessIdentity::current(getpid());
    if (!self) {
      return EXIT_FAILURE;
    }
    const ProcessNamespaceManager mgr;
    const ProcessNamespaceResult res = mgr.inspect(*self, 64);
    CHECK(res.success());
    // Some kernels do not expose every known type (e.g. no `time` namespace
    // before Linux 5.6). Whatever is absent must simply not appear; the result
    // is still a success and never invents IDs. Cross-check against a raw
    // /proc/<pid>/ns readdir: every entry must be represented.
    std::size_t raw_count = 0;
    const std::string ns_dir = "/proc/" + std::to_string(getpid()) + "/ns";
    DIR *dir = ::opendir(ns_dir.c_str());
    CHECK(dir != nullptr);
    if (dir != nullptr) {
      struct ::dirent *entry = nullptr;
      while ((entry = ::readdir(dir)) != nullptr) {
        const char *name = entry->d_name;
        if (name[0] == '.' &&
            (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
          continue;
        }
        ++raw_count;
      }
      ::closedir(dir);
    }
    CHECK(res.detected_count == raw_count);
    // No entry is ever fabricated: absent types contribute zero records.
    std::set<std::string> present;
    for (const ProcessNamespace &ns : res.namespaces) {
      present.insert(ns.name);
    }
    CHECK(present.size() == res.detected_count);
  }

  run("manager: collection limit is enforced and marked truncated");
  {
    const auto self = ProcessIdentity::current(getpid());
    if (!self) {
      return EXIT_FAILURE;
    }
    const ProcessNamespaceManager mgr;
    // Every process has several namespaces, so a limit of 1 is guaranteed to
    // truncate while still counting every entry present.
    const ProcessNamespaceResult res = mgr.inspect(*self, 1);
    CHECK(res.success());
    CHECK(res.namespaces.size() == 1);
    CHECK(res.truncated);
    CHECK(res.detected_count >= res.namespaces.size());
    CHECK(res.detected_count >= 2);
  }

  run("manager: PID reuse is detected");
  {
    const pid_t self_pid = getpid();
    const auto self = ProcessIdentity::current(self_pid);
    if (!self) {
      return EXIT_FAILURE;
    }
    const ProcessNamespaceManager mgr;
    // A fabricated starttime tick that must differ from the real one, so the
    // manager must report ProcessReused instead of real namespaces.
    ProcessIdentity forged{self_pid, self->starttime_ticks + 1ULL};
    const ProcessNamespaceResult res = mgr.inspect(forged, 64);
    CHECK(res.status == NamespaceStatus::ProcessReused);
  }

  run("manager: invalid PID is rejected");
  {
    const ProcessNamespaceManager mgr;
    const ProcessNamespaceResult res =
        mgr.inspect(ProcessIdentity{0, 1}, 64);
    CHECK(res.status == NamespaceStatus::InvalidPid);
    const ProcessNamespaceResult res_neg =
        mgr.inspect(ProcessIdentity{-5, 1}, 64);
    CHECK(res_neg.status == NamespaceStatus::InvalidPid);
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
    // The process has exited; its identity is gone.
    const ProcessNamespaceManager mgr;
    const ProcessNamespaceResult res =
        mgr.inspect(ProcessIdentity{child, 12345}, 64);
    // Either the identity could not be re-read (IdentityUnknown / gone) or the
    // ns directory no longer exists (ProcessNotFound). Either way: never a
    // success with namespaces, and never a crash.
    CHECK(res.status != NamespaceStatus::Success);
  }

  std::fprintf(stderr, "\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}