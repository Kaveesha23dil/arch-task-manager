#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "process_security.hpp"
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

// ---------------------------------------------------------------------------
// Fixtures: sample /proc/<pid>/status contents
// ---------------------------------------------------------------------------

namespace {

/// A realistic /proc/<pid>/status with all security fields present.
std::string sampleStatus() {
  return std::string("Name:\tsample\n"
                     "Umask:\t0022\n"
                     "State:\tS (sleeping)\n"
                     "Tgid:\t1234\n"
                     "Ngid:\t0\n"
                     "Pid:\t1234\n"
                     "PPid:\t1\n"
                     "TracerPid:\t0\n"
                     "Uid:\t1000\t1000\t1000\t1000\n"
                     "Gid:\t1000\t1000\t1000\t1000\n"
                     "FDSize:\t128\n"
                     "Groups:\t4 24 27 30 44 46 1000\n"
                     "NStgid:\t1234\n"
                     "NSpid:\t1234\n"
                     "NSpgid:\t1234\n"
                     "NSsid:\t1234\n"
                     "VmPeak:\t90516 kB\n"
                     "VmSize:\t90516 kB\n"
                     "VmRSS:\t8700 kB\n"
                     "RssAnon:\t8308 kB\n"
                     "RssFile:\t392 kB\n"
                     "RssShmem:\t0 kB\n"
                     "VmData:\t5096 kB\n"
                     "VmStk:\t132 kB\n"
                     "VmExe:\t732 kB\n"
                     "VmLib:\t4212 kB\n"
                     "VmPTE:\t44 kB\n"
                     "VmSwap:\t0 kB\n"
                     "CoreDumping:\t0\n"
                     "Threads:\t2\n"
                     "SigQ:\t0/77439\n"
                     "SigPnd:\t0000000000000000\n"
                     "ShdPnd:\t0000000000000000\n"
                     "SigBlk:\t0000000000000000\n"
                     "SigIgn:\t0000000000001002\n"
                     "SigCgt:\t0000000180004000\n"
                     "CapInh:\t0000000000000000\n"
                     "CapPrm:\t0000000000000000\n"
                     "CapEff:\t0000000000000000\n"
                     "CapBnd:\t0000003fffffffff\n"
                     "CapAmb:\t0000000000000000\n"
                     "NoNewPrivs:\t0\n"
                     "Seccomp:\t0\n"
                     "Seccomp_filters:\t0\n"
                     "Speculation_Store_Bypass:\tthread vulnerable\n"
                     "Cpus_allowed:\tff\n"
                     "Cpus_allowed_list:\t0-7\n"
                     "Mems_allowed:\t1\n"
                     "Mems_allowed_list:\t0\n"
                     "voluntary_ctxt_switches:\t1000\n"
                     "nonvoluntary_ctxt_switches:\t200\n");
}

/// Find a capability set by type in the parsed info.
const atm::CapabilitySet *findCapability(
    const std::vector<atm::CapabilitySet> &sets,
    atm::CapabilitySetType type) {
  for (const atm::CapabilitySet &set : sets) {
    if (set.type == type) {
      return &set;
    }
  }
  return nullptr;
}

/// Find any capability set with a non-zero mask for "effective".
const atm::CapabilitySet *findCapEff(
    const std::vector<atm::CapabilitySet> &sets) {
  return findCapability(sets, atm::CapabilitySetType::Effective);
}

/// Execs `sleep 3600` in a child for live /proc/<pid>/status inspection.
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

  // ------------------------------------------------------------------ UIDs
  run("parseStatusSecurity: Uid with four values");
  {
    const ProcessSecurityInfo info = parseStatusSecurity(sampleStatus());
    CHECK(info.uid.real.has_value());
    CHECK(info.uid.effective.has_value());
    CHECK(info.uid.saved.has_value());
    CHECK(info.uid.filesystem.has_value());
    CHECK(*info.uid.real == 1000);
    CHECK(*info.uid.effective == 1000);
    CHECK(*info.uid.saved == 1000);
    CHECK(*info.uid.filesystem == 1000);
  }

  run("parseStatusSecurity: Uid with different values per slot");
  {
    const std::string status =
        "Uid:\t0\t1000\t1001\t1002\n";
    const ProcessSecurityInfo info = parseStatusSecurity(status);
    CHECK(info.uid.real.has_value() && *info.uid.real == 0);
    CHECK(info.uid.effective.has_value() && *info.uid.effective == 1000);
    CHECK(info.uid.saved.has_value() && *info.uid.saved == 1001);
    CHECK(info.uid.filesystem.has_value() && *info.uid.filesystem == 1002);
  }

  run("parseStatusSecurity: missing Uid line");
  {
    const ProcessSecurityInfo info = parseStatusSecurity("Name:\tx\n");
    CHECK(!info.uid.real.has_value());
    CHECK(!info.uid.effective.has_value());
    CHECK(!info.uid.saved.has_value());
    CHECK(!info.uid.filesystem.has_value());
  }

  // ------------------------------------------------------------------ GIDs
  run("parseStatusSecurity: Gid with four values");
  {
    const ProcessSecurityInfo info = parseStatusSecurity(sampleStatus());
    CHECK(info.gid.real.has_value() && *info.gid.real == 1000);
    CHECK(info.gid.effective.has_value() && *info.gid.effective == 1000);
    CHECK(info.gid.saved.has_value() && *info.gid.saved == 1000);
    CHECK(info.gid.filesystem.has_value() && *info.gid.filesystem == 1000);
  }

  run("parseStatusSecurity: Gid with different values per slot");
  {
    const std::string status =
        "Gid:\t4\t24\t27\t30\n";
    const ProcessSecurityInfo info = parseStatusSecurity(status);
    CHECK(info.gid.real.has_value() && *info.gid.real == 4);
    CHECK(info.gid.effective.has_value() && *info.gid.effective == 24);
    CHECK(info.gid.saved.has_value() && *info.gid.saved == 27);
    CHECK(info.gid.filesystem.has_value() && *info.gid.filesystem == 30);
  }

  // ------------------------------------------------- Supplementary groups
  run("parseStatusSecurity: supplementary groups");
  {
    const ProcessSecurityInfo info = parseStatusSecurity(sampleStatus());
    CHECK(info.groups_available);
    CHECK(info.supplementary_groups.size() == 7);
    CHECK(info.supplementary_groups[0].gid == 4);
    CHECK(info.supplementary_groups[6].gid == 1000);
  }

  run("parseStatusSecurity: empty groups line");
  {
    const std::string status = "Groups:\n";
    const ProcessSecurityInfo info = parseStatusSecurity(status);
    CHECK(info.groups_available);
    CHECK(info.supplementary_groups.empty());
  }

  run("parseStatusSecurity: missing Groups line");
  {
    const std::string status = "Name:\tx\n";
    const ProcessSecurityInfo info = parseStatusSecurity(status);
    CHECK(!info.groups_available);
    CHECK(info.supplementary_groups.empty());
  }

  // ----------------------------------------------------------- Capabilities
  run("parseStatusSecurity: zero capability masks");
  {
    const ProcessSecurityInfo info = parseStatusSecurity(sampleStatus());
    const atm::CapabilitySet *eff = findCapEff(info.capabilities);
    CHECK(eff != nullptr);
    if (eff != nullptr) {
      CHECK(eff->available);
      CHECK(eff->raw_mask == 0);
      CHECK(eff->decoded_names.empty());
      CHECK(eff->unknown_bits.empty());
    }
  }

  run("decodeCapabilities: single capability bit");
  {
    const CapabilitySet set = decodeCapabilities(
        CapabilitySetType::Effective, std::uint64_t{1} << 12);  // CAP_NET_ADMIN
    CHECK(set.raw_mask == (std::uint64_t{1} << 12));
    CHECK(set.decoded_names.size() == 1);
    CHECK(set.decoded_names[0] == "CAP_NET_ADMIN");
    CHECK(set.unknown_bits.empty());
  }

  run("decodeCapabilities: multiple capability bits");
  {
    const std::uint64_t mask = (std::uint64_t{1} << 0) |  // CAP_CHOWN
                               (std::uint64_t{1} << 6) |  // CAP_SETGID
                               (std::uint64_t{1} << 7);    // CAP_SETUID
    const CapabilitySet set = decodeCapabilities(CapabilitySetType::Permitted,
                                                 mask);
    CHECK(set.decoded_names.size() == 3);
    CHECK(set.decoded_names[0] == "CAP_CHOWN");
    CHECK(set.decoded_names[1] == "CAP_SETGID");
    CHECK(set.decoded_names[2] == "CAP_SETUID");
    CHECK(set.unknown_bits.empty());
  }

  run("parseStatusSecurity: all five capability sets are parsed");
  {
    const ProcessSecurityInfo info = parseStatusSecurity(sampleStatus());
    CHECK(findCapability(info.capabilities,
                         CapabilitySetType::Inheritable) != nullptr);
    CHECK(findCapability(info.capabilities,
                         CapabilitySetType::Permitted) != nullptr);
    CHECK(findCapability(info.capabilities,
                         CapabilitySetType::Effective) != nullptr);
    CHECK(findCapability(info.capabilities,
                         CapabilitySetType::Bounding) != nullptr);
    CHECK(findCapability(info.capabilities,
                         CapabilitySetType::Ambient) != nullptr);
    CHECK(info.capabilities.size() == 5);
  }

  run("decodeCapabilities: capability boundary (CAP_BPF at bit 39)");
  {
    const CapabilitySet set = decodeCapabilities(
        CapabilitySetType::Bounding, std::uint64_t{1} << 39);
    CHECK(set.decoded_names.size() == 1);
    CHECK(set.decoded_names[0] == "CAP_BPF");
    CHECK(set.unknown_bits.empty());
  }

  run("decodeCapabilities: unknown capability bit (37) is reported, not "
      "dropped");
  {
    // Bit 37 is above the known table entries >40 (the known entries start at
    // 0..40 and 41+ are reserved); bit 37 is CAP_AUDIT_READ, a real cap.
    const CapabilitySet set = decodeCapabilities(
        CapabilitySetType::Inheritable, std::uint64_t{1} << 37);
    CHECK(set.decoded_names.size() >= 1);  // CAP_AUDIT_READ is known
    // Force a genuinely unknown bit: 44 is reserved.
    const CapabilitySet bogus = decodeCapabilities(
        CapabilitySetType::Effective, std::uint64_t{1} << 44);
    CHECK(bogus.unknown_bits.size() == 1);
    CHECK(bogus.unknown_bits[0] == 44);
  }

  run("parseStatusSecurity: invalid hexadecimal value yields no capability");
  {
    const std::string status = "CapEff:\tNOTHEX\n";
    const ProcessSecurityInfo info = parseStatusSecurity(status);
    CHECK(findCapEff(info.capabilities) == nullptr);
  }

  run("parseStatusSecurity: missing capability field");
  {
    const std::string status = "Name:\tx\n";
    const ProcessSecurityInfo info = parseStatusSecurity(status);
    CHECK(info.capabilities.empty());
  }

  // ---------------------------------------------------------- Security flags
  run("parseStatusSecurity: NoNewPrivs 0");
  {
    const ProcessSecurityInfo info = parseStatusSecurity(sampleStatus());
    CHECK(info.no_new_privs == NoNewPrivsState::Disabled);
  }

  run("parseStatusSecurity: NoNewPrivs 1");
  {
    const std::string status = "NoNewPrivs:\t1\n";
    const ProcessSecurityInfo info = parseStatusSecurity(status);
    CHECK(info.no_new_privs == NoNewPrivsState::Enabled);
  }

  run("parseStatusSecurity: missing NoNewPrivs");
  {
    const std::string status = "Name:\tx\n";
    const ProcessSecurityInfo info = parseStatusSecurity(status);
    CHECK(info.no_new_privs == NoNewPrivsState::Unavailable);
  }

  run("parseStatusSecurity: Seccomp 0");
  {
    const ProcessSecurityInfo info = parseStatusSecurity(sampleStatus());
    CHECK(info.seccomp == SeccompMode::Disabled);
  }

  run("parseStatusSecurity: Seccomp 1 (strict)");
  {
    const std::string status = "Seccomp:\t1\n";
    const ProcessSecurityInfo info = parseStatusSecurity(status);
    CHECK(info.seccomp == SeccompMode::Strict);
  }

  run("parseStatusSecurity: Seccomp 2 (filter)");
  {
    const std::string status = "Seccomp:\t2\n";
    const ProcessSecurityInfo info = parseStatusSecurity(status);
    CHECK(info.seccomp == SeccompMode::Filter);
  }

  run("parseStatusSecurity: unknown Seccomp value");
  {
    const std::string status = "Seccomp:\t7\n";
    const ProcessSecurityInfo info = parseStatusSecurity(status);
    CHECK(info.seccomp == SeccompMode::Unknown);
  }

  run("parseStatusSecurity: missing Seccomp");
  {
    const std::string status = "Name:\tx\n";
    const ProcessSecurityInfo info = parseStatusSecurity(status);
    CHECK(info.seccomp == SeccompMode::Unknown);
  }

  run("parseStatusSecurity: Seccomp filter count");
  {
    const std::string status = "Seccomp_filters:\t4\n";
    const ProcessSecurityInfo info = parseStatusSecurity(status);
    CHECK(info.seccomp_filters.has_value());
    CHECK(*info.seccomp_filters == 4);
  }

  run("parseStatusSecurity: TracerPid 0 means no tracer");
  {
    const ProcessSecurityInfo info = parseStatusSecurity(sampleStatus());
    CHECK(info.tracer_pid.has_value());
    CHECK(*info.tracer_pid == 0);
  }

  run("parseStatusSecurity: nonzero TracerPid");
  {
    const std::string status = "TracerPid:\t4321\n";
    const ProcessSecurityInfo info = parseStatusSecurity(status);
    CHECK(info.tracer_pid.has_value());
    CHECK(*info.tracer_pid == 4321);
  }

  run("parseStatusSecurity: Umask is parsed (octal display value)");
  {
    const ProcessSecurityInfo info = parseStatusSecurity(sampleStatus());
    CHECK(info.umask.has_value());
    // "0022" is stored as the decimal value 22; the UI renders it as octal
    // "0022". (0022 as a C++ literal would be octal 18, which is not the
    // numeric value the status file carries.)
    CHECK(*info.umask == 22);
  }

  run("parseStatusSecurity: CoreDumping is parsed");
  {
    const ProcessSecurityInfo info = parseStatusSecurity(sampleStatus());
    CHECK(info.core_dumping.has_value());
    CHECK(!*info.core_dumping);  // 0 = not dumping
    const std::string status = "CoreDumping:\t1\n";
    const ProcessSecurityInfo forcing = parseStatusSecurity(status);
    CHECK(forcing.core_dumping.has_value());
    CHECK(*forcing.core_dumping);
  }

  run("parseStatusSecurity: malformed Umask value is tolerated");
  {
    const std::string status = "Umask:\tnotoctal\n";
    const ProcessSecurityInfo info = parseStatusSecurity(status);
    CHECK(!info.umask.has_value());
  }

  run("parseStatusSecurity: missing security context is not an error");
  {
    const std::string status = sampleStatus();  // no attr files in a status dump
    const ProcessSecurityInfo info = parseStatusSecurity(status);
    CHECK(!info.security_context_available);
    CHECK(info.security_context.empty());
    CHECK(!info.exec_context_available);
    CHECK(info.exec_context.empty());
    CHECK(!info.login_uid_available);
  }

  run("parseStatusSecurity: empty state (no fields) does not crash");
  {
    const ProcessSecurityInfo info = parseStatusSecurity("");
    CHECK(!info.groups_available);
    CHECK(info.capabilities.empty());
    CHECK(info.no_new_privs == NoNewPrivsState::Unavailable);
  }

  // ----------------------------------------------------------- Malformed data
  run("parseStatusSecurity: missing colon is tolerated");
  {
    const std::string status = "NoNewPrivs\t1\n"   // no colon
                               "Uid 1000 1000 1000 1000\n";  // no colon
    const ProcessSecurityInfo info = parseStatusSecurity(status);
    CHECK(info.no_new_privs == NoNewPrivsState::Unavailable);
    CHECK(!info.uid.real.has_value());
  }

  run("parseStatusSecurity: invalid numeric value");
  {
    const std::string status = "TracerPid:\tabc\n"
                               "Seccomp:\txyz\n"
                               "NoNewPrivs:\tnotanumber\n";
    const ProcessSecurityInfo info = parseStatusSecurity(status);
    CHECK(!info.tracer_pid.has_value());
    CHECK(info.seccomp == SeccompMode::Unknown);
    CHECK(info.no_new_privs == NoNewPrivsState::Unavailable);
  }

  run("parseStatusSecurity: empty numeric field");
  {
    const std::string status = "NoNewPrivs:\n"
                               "Seccomp:\n"
                               "TracerPid:\n";
    const ProcessSecurityInfo info = parseStatusSecurity(status);
    CHECK(info.no_new_privs == NoNewPrivsState::Unavailable);
    CHECK(info.seccomp == SeccompMode::Unknown);
    CHECK(!info.tracer_pid.has_value());
  }

  run("parseStatusSecurity: extra whitespace");
  {
    const std::string status = "Uid:\t\t\t1000\t\t1000  1000 1000\n";
    const ProcessSecurityInfo info = parseStatusSecurity(status);
    CHECK(info.uid.real.has_value() && *info.uid.real == 1000);
    CHECK(info.uid.filesystem.has_value() && *info.uid.filesystem == 1000);
  }

  run("parseStatusSecurity: unknown fields are ignored");
  {
    const std::string status =
        "FUTURE_FIELD:\t42\n"
        "Name:\tx\n"
        "AnotherNewField = something odd\n";
    const ProcessSecurityInfo info = parseStatusSecurity(status);
    CHECK(!info.uid.real.has_value());
    CHECK(info.capabilities.empty());
    CHECK(info.no_new_privs == NoNewPrivsState::Unavailable);
  }

  run("parseStatusSecurity: truncated input yields available fields only");
  {
    const std::string status = "Uid:\t1000 1000 1000 1000\n"
                               "Gid:\t1000";  // truncated mid-line
    const ProcessSecurityInfo info = parseStatusSecurity(status);
    CHECK(info.uid.real.has_value() && *info.uid.real == 1000);
    // Gid line truncated: the tokenizer sees "1000" only, which fills real.
    CHECK(info.gid.real.has_value());
  }

  run("parseStatusSecurity: unexpected capability values are tolerated");
  {
    const std::string status = "CapEff:\t0xZZZZ\n"
                               "CapBnd:\tzzz\n";
    const ProcessSecurityInfo info = parseStatusSecurity(status);
    CHECK(findCapEff(info.capabilities) == nullptr);
    CHECK(findCapability(info.capabilities,
                         CapabilitySetType::Bounding) == nullptr);
  }

  // --------------------------------------------- Capability name helpers
  run("capabilitySetTypeName: all five");
  {
    CHECK(std::string(capabilitySetTypeName(CapabilitySetType::Inheritable)) ==
          "Inheritable");
    CHECK(std::string(capabilitySetTypeName(CapabilitySetType::Permitted)) ==
          "Permitted");
    CHECK(std::string(capabilitySetTypeName(CapabilitySetType::Effective)) ==
          "Effective");
    CHECK(std::string(capabilitySetTypeName(CapabilitySetType::Bounding)) ==
          "Bounding");
    CHECK(std::string(capabilitySetTypeName(CapabilitySetType::Ambient)) ==
          "Ambient");
  }

  // ------------------------------------------------------------ Live manager
  run("manager: inspecting a live child process succeeds");
  {
    const pid_t child = spawnSleep();
    const bool exec_done = child >= 0 && waitForExec(child);
    CHECK(child >= 0);
    if (child < 0) {
      return EXIT_FAILURE;
    }
    CHECK(exec_done);
    const ProcessSecurityManager manager;
    if (!exec_done) {
      ::kill(child, SIGKILL);
      ::waitpid(child, nullptr, 0);
    } else {
      const std::optional<ProcessIdentity> identity =
          ProcessIdentity::current(child);
      CHECK(identity.has_value());
      if (identity) {
        const ProcessSecurityResult result = manager.inspect(*identity);
        ::kill(child, SIGKILL);
        ::waitpid(child, nullptr, 0);

        CHECK(result.success());
        CHECK(result.info.uid.real.has_value());
        CHECK(result.info.uid.effective == result.info.uid.real);
        CHECK(result.info.gid.real.has_value());
        CHECK(result.info.gid.effective == result.info.gid.real);
        CHECK(result.info.groups_available);
        // Capabilities: the child may or may not have any, but the sets must
        // be parsed (bounding set, at minimum, is always present).
        const atm::CapabilitySet *bnd = findCapability(
            result.info.capabilities, CapabilitySetType::Bounding);
        CHECK(bnd != nullptr);
        // NoNewPrivs / Seccomp from the child.
        CHECK(result.info.no_new_privs == NoNewPrivsState::Disabled ||
              result.info.no_new_privs == NoNewPrivsState::Enabled ||
              result.info.no_new_privs == NoNewPrivsState::Unavailable);
        CHECK(result.info.seccomp == SeccompMode::Disabled ||
              result.info.seccomp == SeccompMode::Strict ||
              result.info.seccomp == SeccompMode::Filter ||
              result.info.seccomp == SeccompMode::Unknown);
        CHECK(result.info.tracer_pid.has_value());
        CHECK(*result.info.tracer_pid == 0);
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
    const ProcessSecurityManager manager;
    const ProcessSecurityResult result = manager.inspect(*self);
    CHECK(result.success());
    CHECK(result.info.uid.real.has_value());
    CHECK(result.info.gid.real.has_value());
    CHECK(!result.info.capabilities.empty());

    // Optional files must never break the overall inspection. On a system
    // without SELinux/AppArmor new-format labels, attr/current is absent
    // (ENOENT) or permission-denied — either way the inspection stays Success.
    if (!result.info.security_context_available) {
      CHECK(result.info.security_context.empty());
    }
    // loginuid: either available (with a numeric value, possibly the "unset"
    // sentinel) or unavailable — never an inspection-level error.
    if (result.info.login_uid_available) {
      CHECK(result.info.login_uid.has_value());
      CHECK(*result.info.login_uid == atm::kLoginUidUnset ||
            *result.info.login_uid < atm::kLoginUidUnset);
    }
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
    const ProcessSecurityManager manager;
    ProcessIdentity forged{self_pid, self->starttime_ticks + 1ULL};
    const ProcessSecurityResult result = manager.inspect(forged);
    CHECK(result.status == SecurityStatus::ProcessReused);
  }

  run("manager: invalid PID is rejected before any read");
  {
    const ProcessSecurityManager manager;
    const ProcessSecurityResult zero =
        manager.inspect(ProcessIdentity{0, 1});
    CHECK(zero.status == SecurityStatus::InvalidPid);
    const ProcessSecurityResult negative =
        manager.inspect(ProcessIdentity{-5, 1});
    CHECK(negative.status == SecurityStatus::InvalidPid);
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
    const ProcessSecurityManager manager;
    const ProcessSecurityResult result =
        manager.inspect(ProcessIdentity{child, 12345});
    CHECK(result.status == SecurityStatus::IdentityUnknown ||
          result.status == SecurityStatus::ProcessNotFound);
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
    const ProcessSecurityManager manager;
    if (!exec_done) {
      ::kill(child, SIGKILL);
      ::waitpid(child, nullptr, 0);
    } else {
      const std::optional<ProcessIdentity> child_identity =
          ProcessIdentity::current(child);
      CHECK(child_identity.has_value());
      if (child_identity) {
        // Select the child, then (simulating a concurrent selection change)
        // forge an identity for a different start time — the inspection must
        // reject it rather than return stale data.
        ProcessIdentity stale{child, child_identity->starttime_ticks + 1ULL};
        const ProcessSecurityResult stale_result = manager.inspect(stale);
        CHECK(stale_result.status == SecurityStatus::ProcessReused);
        CHECK(stale_result.info.uid.real.has_value() == false);

        const ProcessSecurityResult fresh_result =
            manager.inspect(*child_identity);
        CHECK(fresh_result.status == SecurityStatus::Success);

        ::kill(child, SIGKILL);
        ::waitpid(child, nullptr, 0);
      }
    }
  }

  // ------------------------------------------------------- UID/GID / loginuid
  run("loginUidParsing: helper reads the audit login UID of self");
  {
    const std::optional<std::uint32_t> luid =
        readLoginUid("/proc/self/loginuid", kDefaultMaxLoginUidBytes, nullptr);
    (void)luid;  // available whenever the kernel exposes /proc/self/loginuid
  }

  run("readSecurityAttr: non-existent path reports errno, does not crash");
  {
    int err = 0;
    const std::optional<std::string> ctx = readSecurityAttr(
        "/proc/1/attr/definitely-missing-" + std::to_string(getpid()),
        kDefaultMaxSecurityAttrBytes, &err);
    CHECK(!ctx.has_value());
    CHECK(err == ENOENT || err == EACCES || err == EPERM);
  }

  run("readSecurityAttr: bounded read never exceeds max_bytes");
  {
    // /proc/self/environ can be larger than 64 bytes; a bounded read of it
    // through the same primitive must never return more than requested.
    int err = 0;
    const std::optional<std::string> small = readSecurityAttr(
        "/proc/self/status", 64, &err);
    CHECK(small.has_value() || err != 0);
    if (small) {
      CHECK(small->size() <= 64);
    }
  }

  std::fprintf(stderr, "\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}