#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "process_environment.hpp"
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

/// In-memory environment bytes from a string of "NAME=value\0..." records.
std::string envBytes(const std::vector<std::string> &records) {
  std::string out;
  for (const std::string &record : records) {
    out += record;
    out.push_back('\0');
  }
  return out;
}

// Looks up a parsed entry by name; nullptr when absent.
const atm::ProcessEnvironmentEntry *findEntry(
    const std::vector<atm::ProcessEnvironmentEntry> &entries,
    const std::string &name) {
  for (const atm::ProcessEnvironmentEntry &entry : entries) {
    if (entry.name == name) {
      return &entry;
    }
  }
  return nullptr;
}

/// Execs `sleep 3600` in a child with a controlled environment so the parent
/// can read /proc/<child>/environ. Returns the child PID, or -1 on failure.
pid_t spawnEnvProvider(char *const *envp) {
  const pid_t child = ::fork();
  if (child < 0) {
    return -1;
  }
  if (child == 0) {
    char *const argv[] = {
        const_cast<char *>("sleep"), const_cast<char *>("3600"), nullptr};
    ::execve("/bin/sleep", argv, envp);
    ::_exit(127);
  }
  return child;
}

/// Waits (bounded) until the child has completed its execve by polling
/// /proc/<child>/comm — its environment is only defined once exec lands.
bool waitForExec(const pid_t child) {
  for (int attempt = 0; attempt < 2000; ++attempt) {  // ~2 s worst case
    const std::string comm_path =
        "/proc/" + std::to_string(child) + "/comm";
    std::FILE *file = std::fopen(comm_path.c_str(), "r");
    if (file != nullptr) {
      char comm[64] = {0};
      const bool read_ok =
          std::fgets(comm, sizeof(comm), file) != nullptr;
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

#define CHECK(expr) ::expect((expr), #expr, __FILE__, __LINE__)

int main() {
  using namespace atm;

  // --- Parsing: NUL-separated records, sorted by name. ------------------
  run("parseEnvironment: basic records are parsed and sorted");
  {
    const std::string data = envBytes({"PATH=/usr/bin", "HOME=/home/user"});
    const EnvironmentParseResult result = parseEnvironment(data);
    CHECK(result.entries.size() == 2);
    CHECK(result.record_count == 2);
    CHECK(result.malformed_count == 0);
    CHECK(result.duplicate_count == 0);
    // Sorted ascending by variable name regardless of input order.
    CHECK(result.entries[0].name == "HOME");
    CHECK(result.entries[0].value == "/home/user");
    CHECK(result.entries[1].name == "PATH");
    CHECK(result.entries[1].value == "/usr/bin");
  }

  // --- Values containing '=' are split at the FIRST '=' only. ------------
  run("parseEnvironment: '=' inside a value is preserved");
  {
    const std::string data = envBytes({"URL=https://example.com?a=b=c"});
    const EnvironmentParseResult result = parseEnvironment(data);
    CHECK(result.entries.size() == 1);
    CHECK(result.entries[0].name == "URL");
    CHECK(result.entries[0].value == "https://example.com?a=b=c");
  }

  // --- Empty values. -----------------------------------------------------
  run("parseEnvironment: empty values are kept");
  {
    const std::string data = envBytes({"EMPTY="});
    const EnvironmentParseResult result = parseEnvironment(data);
    CHECK(result.entries.size() == 1);
    CHECK(result.entries[0].name == "EMPTY");
    CHECK(result.entries[0].value.empty());
  }

  // --- Malformed records are counted and skipped, never crash. ----------
  run("parseEnvironment: malformed records are counted and skipped");
  {
    const std::string data = envBytes({"GOOD=1", "NOEQUALS", "=EMPTYNAME"});
    const EnvironmentParseResult result = parseEnvironment(data);
    CHECK(result.record_count == 3);
    CHECK(result.malformed_count == 2);
    CHECK(result.entries.size() == 1);
    CHECK(result.entries[0].name == "GOOD");
    CHECK(result.entries[0].value == "1");
  }

  // --- Missing trailing NUL. ---------------------------------------------
  run("parseEnvironment: missing trailing NUL still parses the last record");
  {
    const std::string data = std::string("A=1\0B=2", 7);  // no trailing NUL
    const EnvironmentParseResult result = parseEnvironment(data);
    CHECK(result.entries.size() == 2);
    CHECK(findEntry(result.entries, "B") != nullptr);
    CHECK(findEntry(result.entries, "B")->value == "2");
  }

  // --- Duplicate names: first wins, deterministically counted. ----------
  run("parseEnvironment: duplicate names keep the first value");
  {
    const std::string data = envBytes({"DUP=1", "DUP=2", "DUP=3"});
    const EnvironmentParseResult result = parseEnvironment(data);
    CHECK(result.entries.size() == 1);
    CHECK(result.entries[0].name == "DUP");
    CHECK(result.entries[0].value == "1");
    CHECK(result.duplicate_count == 2);
  }

  // --- Empty / all-NUL input. --------------------------------------------
  run("parseEnvironment: empty and all-NUL input produce no entries");
  {
    const EnvironmentParseResult empty = parseEnvironment("");
    CHECK(empty.record_count == 0);
    CHECK(empty.entries.empty());
    const EnvironmentParseResult nuls =
        parseEnvironment(std::string("\0\0\0", 3));
    CHECK(nuls.record_count == 0);
    CHECK(nuls.entries.empty());
  }

  // --- Variable-count limit. ---------------------------------------------
  run("parseEnvironment: variable limit truncates deterministically");
  {
    const std::string data = envBytes({"A=1", "B=2", "C=3", "D=4"});
    const EnvironmentParseResult result = parseEnvironment(data, 2);
    CHECK(result.entries.size() == 2);
    CHECK(result.variable_truncated);
    CHECK(result.record_count == 4);
  }

  // --- Sensitivity classification. ---------------------------------------
  run("isSensitiveVariableName: documented secret-bearing patterns");
  {
    CHECK(isSensitiveVariableName("PASSWORD"));
    CHECK(isSensitiveVariableName("DATABASE_PASSWORD"));
    CHECK(isSensitiveVariableName("API_TOKEN"));
    CHECK(isSensitiveVariableName("GITHUB_TOKEN"));
    CHECK(isSensitiveVariableName("AWS_SECRET_ACCESS_KEY"));
    CHECK(isSensitiveVariableName("PRIVATE_KEY"));
    CHECK(isSensitiveVariableName("AUTH_TOKEN"));
    CHECK(isSensitiveVariableName("CREDENTIALS"));
    CHECK(isSensitiveVariableName("MY_COOKIE_SECRET"));
    CHECK(isSensitiveVariableName("BEARER_AUTH"));
    CHECK(!isSensitiveVariableName("NORMAL_VALUE"));
    CHECK(!isSensitiveVariableName("PATH"));
    CHECK(!isSensitiveVariableName("HOME"));
    CHECK(!isSensitiveVariableName(""));
  }

  run("isSensitiveVariableName: case-insensitive");
  {
    CHECK(isSensitiveVariableName("password"));
    CHECK(isSensitiveVariableName("Password"));
    CHECK(isSensitiveVariableName("PASSWORD"));
    CHECK(isSensitiveVariableName("db_token"));
    CHECK(isSensitiveVariableName("DB-Token"));
    CHECK(!isSensitiveVariableName("PATH_IS_FINE"));
  }

  // --- Masking: sensitive values never enter the display model. ---------
  run("parseEnvironment: sensitive values are masked immediately");
  {
    const std::string data =
        envBytes({"DATABASE_PASSWORD=hunter2", "NORMAL_VALUE=visible"});
    const EnvironmentParseResult result = parseEnvironment(data);
    const ProcessEnvironmentEntry *secret = findEntry(result.entries, "DATABASE_PASSWORD");
    CHECK(secret != nullptr);
    CHECK(secret->sensitive);
    CHECK(secret->value == kMaskedSecretPlaceholder);
    // The plaintext secret must not appear anywhere in the model.
    bool leaked = false;
    for (const ProcessEnvironmentEntry &entry : result.entries) {
      if (entry.value.find("hunter2") != std::string::npos) {
        leaked = true;
      }
    }
    CHECK(!leaked);
    const ProcessEnvironmentEntry *normal = findEntry(result.entries, "NORMAL_VALUE");
    CHECK(normal != nullptr);
    CHECK(!normal->sensitive);
    CHECK(normal->value == "visible");
  }

  // --- Live: a child with a controlled environment. ---------------------
  run("manager: reads a child's controlled environment correctly");
  {
    char *const envp[] = {
        const_cast<char *>("A=1"),
        const_cast<char *>("SECRET_API_TOKEN=xyz-super-secret"),
        const_cast<char *>("DUP=first"),
        const_cast<char *>("DUP=second"),
        const_cast<char *>("URL=https://example.com?a=b=c"),
        const_cast<char *>("EMPTY="),
        nullptr};
    const pid_t child = spawnEnvProvider(envp);
    const bool exec_done = child >= 0 && waitForExec(child);
    CHECK(child >= 0);
    if (child < 0) {
      return EXIT_FAILURE;
    }
    CHECK(exec_done);
    const ProcessEnvironmentManager manager;
    if (!exec_done) {
      ::kill(child, SIGKILL);
      ::waitpid(child, nullptr, 0);
    } else {
      const std::optional<ProcessIdentity> child_identity =
          ProcessIdentity::current(child);
      CHECK(child_identity.has_value());
      if (child_identity) {
        const ProcessEnvironmentResult result = manager.inspect(*child_identity);
        ::kill(child, SIGKILL);
        ::waitpid(child, nullptr, 0);

        CHECK(result.success());
        CHECK(result.status == EnvironmentStatus::Success);
        const ProcessEnvironmentEntry *a = findEntry(result.entries, "A");
        CHECK(a != nullptr && a->value == "1");
        const ProcessEnvironmentEntry *secret = findEntry(result.entries, "SECRET_API_TOKEN");
        CHECK(secret != nullptr);
        CHECK(secret->sensitive);
        CHECK(secret->value == kMaskedSecretPlaceholder);
        CHECK(result.sensitive_count >= 1);
        const ProcessEnvironmentEntry *dup = findEntry(result.entries, "DUP");
        CHECK(dup != nullptr && dup->value == "first");
        CHECK(result.duplicate_count == 1);
        const ProcessEnvironmentEntry *url = findEntry(result.entries, "URL");
        CHECK(url != nullptr && url->value == "https://example.com?a=b=c");
        const ProcessEnvironmentEntry *empty = findEntry(result.entries, "EMPTY");
        CHECK(empty != nullptr && empty->value.empty());
        // The secret must not leak into any entry's displayed value.
        for (const ProcessEnvironmentEntry &entry : result.entries) {
          CHECK(entry.value.find("xyz-super-secret") == std::string::npos);
        }
      }
    }
  }

  // --- Empty environment (exec with a truly empty env list). ------------
  run("manager: a genuinely empty environment is distinguished");
  {
    char *const envp[] = {nullptr};
    const pid_t child = spawnEnvProvider(envp);
    const bool exec_done = child >= 0 && waitForExec(child);
    CHECK(child >= 0);
    if (child < 0) {
      return EXIT_FAILURE;
    }
    CHECK(exec_done);
    const ProcessEnvironmentManager manager;
    if (!exec_done) {
      ::kill(child, SIGKILL);
      ::waitpid(child, nullptr, 0);
    } else {
      const std::optional<ProcessIdentity> child_identity =
          ProcessIdentity::current(child);
      CHECK(child_identity.has_value());
      if (child_identity) {
        const ProcessEnvironmentResult result = manager.inspect(*child_identity);
        ::kill(child, SIGKILL);
        ::waitpid(child, nullptr, 0);
        CHECK(result.status == EnvironmentStatus::EmptyEnvironment);
        CHECK(result.success());
        CHECK(result.entries.empty());
      }
    }
  }

  // --- Live: own process is readable and masked consistently. -----------
  run("manager: inspecting the application itself works");
  {
    const std::optional<ProcessIdentity> self =
        ProcessIdentity::current(getpid());
    CHECK(self.has_value());
    if (!self) {
      return EXIT_FAILURE;
    }
    const ProcessEnvironmentManager manager;
    const ProcessEnvironmentResult result = manager.inspect(*self);
    CHECK(result.status == EnvironmentStatus::Success);
    CHECK(result.entries.size() >= 1);
    // Every sensitive entry must already be masked.
    for (const ProcessEnvironmentEntry &entry : result.entries) {
      if (entry.sensitive) {
        CHECK(entry.value == kMaskedSecretPlaceholder);
      }
    }
    CHECK(result.sensitive_count <= result.entries.size());
  }

  // --- Selection safety: A's environment never leaks into B's result. ---
  run("manager: results are bound to the selected process only");
  {
    // Self carries a distinctive variable; the crafted child carries "A=1".
    const std::optional<ProcessIdentity> self =
        ProcessIdentity::current(getpid());
    char *const envp[] = {const_cast<char *>("CHILD_ONLY_MARKER=yes"), nullptr};
    const pid_t child = spawnEnvProvider(envp);
    CHECK(self.has_value() && child >= 0);
    if (!self || child < 0) {
      if (child > 0) {
        ::kill(child, SIGKILL);
        ::waitpid(child, nullptr, 0);
      }
      return EXIT_FAILURE;
    }
    const bool exec_done = waitForExec(child);
    CHECK(exec_done);
    const ProcessEnvironmentManager manager;
    if (!exec_done) {
      ::kill(child, SIGKILL);
      ::waitpid(child, nullptr, 0);
    } else {
      const std::optional<ProcessIdentity> child_identity =
          ProcessIdentity::current(child);
      CHECK(child_identity.has_value());
      if (child_identity) {
        const ProcessEnvironmentResult child_result =
            manager.inspect(*child_identity);
        const ProcessEnvironmentResult self_result = manager.inspect(*self);
        ::kill(child, SIGKILL);
        ::waitpid(child, nullptr, 0);
        // "CHILD_ONLY_MARKER" appears for the child, never for self (there is no
        // cross-process reuse: an inspection reads exactly one PID's snapshot).
        CHECK(findEntry(child_result.entries, "CHILD_ONLY_MARKER") != nullptr);
        CHECK(findEntry(self_result.entries, "CHILD_ONLY_MARKER") == nullptr);
      }
    }
  }

  // --- Size limit. -------------------------------------------------------
  run("manager: the byte limit stops the read and reports truncation");
  {
    const std::optional<ProcessIdentity> self =
        ProcessIdentity::current(getpid());
    CHECK(self.has_value());
    if (!self) {
      return EXIT_FAILURE;
    }
    const ProcessEnvironmentManager manager;
    const ProcessEnvironmentResult result =
        manager.inspect(*self, /*max_bytes=*/16,
                        kDefaultMaxEnvironmentVariables);
    if (result.status == EnvironmentStatus::Success) {
      // Our own environment is certainly longer than 16 bytes.
      CHECK(result.size_truncated);
      CHECK(result.byte_count == 16);
    }
  }

  run("manager: the variable limit is enforced on real data");
  {
    const std::optional<ProcessIdentity> self =
        ProcessIdentity::current(getpid());
    CHECK(self.has_value());
    if (!self) {
      return EXIT_FAILURE;
    }
    const ProcessEnvironmentManager manager;
    const ProcessEnvironmentResult result =
        manager.inspect(*self, kDefaultMaxEnvironmentBytes, 2);
    CHECK(result.status == EnvironmentStatus::Success);
    CHECK(result.entries.size() == 2);
    CHECK(result.variable_truncated);
  }

  // --- PID reuse / identity gate. ---------------------------------------
  run("manager: PID reuse / forged identity aborts the inspection");
  {
    const pid_t self_pid = getpid();
    const std::optional<ProcessIdentity> self =
        ProcessIdentity::current(self_pid);
    CHECK(self.has_value());
    if (!self) {
      return EXIT_FAILURE;
    }
    const ProcessEnvironmentManager manager;
    ProcessIdentity forged{self_pid, self->starttime_ticks + 1ULL};
    const ProcessEnvironmentResult result = manager.inspect(forged);
    CHECK(result.status == EnvironmentStatus::ProcessReused);
    CHECK(result.entries.empty());
  }

  run("manager: invalid PID is rejected before any read");
  {
    const ProcessEnvironmentManager manager;
    const ProcessEnvironmentResult zero =
        manager.inspect(ProcessIdentity{0, 1});
    CHECK(zero.status == EnvironmentStatus::InvalidPid);
    const ProcessEnvironmentResult negative =
        manager.inspect(ProcessIdentity{-5, 1});
    CHECK(negative.status == EnvironmentStatus::InvalidPid);
  }

  // --- Vanished process. -------------------------------------------------
  run("manager: a disappeared process is reported, never crashes");
  {
    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
      ::_exit(0);
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    const ProcessEnvironmentManager manager;
    const ProcessEnvironmentResult result =
        manager.inspect(ProcessIdentity{child, 12345});
    CHECK(result.status == EnvironmentStatus::IdentityUnknown ||
          result.status == EnvironmentStatus::ProcessNotFound);
    CHECK(!result.success());
  }

  // --- Permission errors (best effort, like the memory-map tests). ------
  run("manager: permission errors map to a meaningful state (best effort)");
  {
    // environ is only readable with ptrace access; kernel/other-user processes
    // usually yield EACCES. Scan and skip gracefully when none is restricted.
    const ProcessEnvironmentManager manager;
    bool found_denied = false;
    for (::pid_t pid = 1; pid <= 512 && !found_denied; ++pid) {
      const std::optional<ProcessIdentity> identity =
          ProcessIdentity::current(pid);
      if (!identity) {
        continue;
      }
      const ProcessEnvironmentResult result = manager.inspect(*identity);
      if (result.status == EnvironmentStatus::PermissionDenied) {
        found_denied = true;
        CHECK(result.entries.empty());
        CHECK(!result.success());
      }
    }
    if (!found_denied) {
      std::fprintf(stderr, "  (no restricted process found: permission-denied "
                           "runtime case skipped)\n");
    }
  }

  std::fprintf(stderr, "\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}