#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "process_memory_map.hpp"
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

using atm::MemoryMapCategory;
using atm::MemoryMapStatus;
using atm::ProcessIdentity;
using atm::ProcessMemoryMap;
using atm::ProcessMemoryMapManager;

// Parses one maps line into a mapping, or nullopt on malformed input.
static std::optional<ProcessMemoryMap> line(std::string_view text) {
  return atm::parseMemoryMapLine(text);
}

int main() {
  // --- Parser: a fully-populated file-backed mapping. ----------------------
  run("parses a complete /usr/lib mapping");
  {
    const auto m = line(
        "7f1000000000-7f1000021000 r-xp 00021000 08:01 123456 "
        "/usr/lib/libexample.so");
    CHECK(m.has_value());
    CHECK(m->start == 0x7f1000000000ULL);
    CHECK(m->end == 0x7f1000021000ULL);
    CHECK(m->size() == 0x7f1000021000ULL - 0x7f1000000000ULL);
    CHECK(m->offset == 0x00021000ULL);
    CHECK(m->device_major == 8);
    CHECK(m->device_minor == 1);
    CHECK(m->inode == 123456);
    CHECK(m->permissions == "r-xp");
    CHECK(m->readable);
    CHECK(!m->writable);
    CHECK(m->executable);
    CHECK(!m->shared);  // 'p' = private
    CHECK(m->pathname == "/usr/lib/libexample.so");
    CHECK(m->category == MemoryMapCategory::SharedLibrary);
  }

  run("a 132 KB code mapping has the expected size");
  {
    const auto m = line(
        "7f1000000000-7f1000021000 r-xp 00000000 08:01 123456 "
        "/usr/lib/libexample.so");
    CHECK(m.has_value());
    CHECK(m->size() == 0x21000ULL);  // 132 kB
  }

  // --- Parser: permission and sharing bits. -------------------------------
  run("parses permission combinations and the shared/private flag");
  {
    const auto ro = line("0-1000 r--p 00000000 00:00 0");
    CHECK(ro.has_value());
    CHECK(ro->readable && !ro->writable && !ro->executable && !ro->shared);

    const auto rw = line("1000-2000 rw-p 00000000 00:00 0");
    CHECK(rw.has_value());
    CHECK(rw->readable && rw->writable && !rw->executable && !rw->shared);

    const auto rwx = line("2000-3000 rwxp 00000000 00:00 0");
    CHECK(rwx.has_value());
    CHECK(rwx->readable && rwx->writable && rwx->executable && !rwx->shared);

    const auto shared = line("3000-4000 rw-s 00000000 00:00 0");
    CHECK(shared.has_value());
    CHECK(shared->readable && shared->writable && !shared->executable &&
          shared->shared);

    const auto exec_only = line("4000-5000 --xp 00000000 00:00 0");
    CHECK(exec_only.has_value());
    CHECK(!exec_only->readable && !exec_only->writable && exec_only->executable);
  }

  // --- Parser: special mappings without a file backing. -------------------
  run("recognises [heap], [stack], [vdso], [vvar], [vsyscall] by name");
  {
    const auto heap = line("55a000000000-55a000020000 rw-p 00000000 00:00 0 "
                           "[heap]");
    CHECK(heap.has_value());
    CHECK(heap->pathname == "[heap]");
    CHECK(heap->category == MemoryMapCategory::Heap);

    const auto stack = line("7ffd00000000-7ffd00021000 rw-p 00000000 00:00 0 "
                            "[stack]");
    CHECK(stack.has_value());
    CHECK(stack->pathname == "[stack]");
    CHECK(stack->category == MemoryMapCategory::Stack);

    const auto vdso = line("7fff10000000-7fff10002000 r-xp 00000000 00:00 0 "
                           "[vdso]");
    CHECK(vdso.has_value());
    CHECK(vdso->category == MemoryMapCategory::Vdso);

    const auto vvar = line("7fff10002000-7fff10006000 rw-p 00000000 00:00 0 "
                           "[vvar]");
    CHECK(vvar.has_value());
    CHECK(vvar->category == MemoryMapCategory::Vvar);

    const auto vsyscall =
        line("ffffffffff600000-ffffffffff601000 --xp 00000000 00:00 0 "
             "[vsyscall]");
    CHECK(vsyscall.has_value());
    CHECK(vsyscall->category == MemoryMapCategory::Vsyscall);
  }

  // --- Parser: pathname is optional. --------------------------------------
  run("anonymous mappings keep an empty pathname");
  {
    const auto anon = line(
        "7f3e8a8aa000-7f3e8a8ad000 rw-p 00000000 00:00 0 ");
    CHECK(anon.has_value());
    CHECK(anon->pathname.empty());
    CHECK(anon->inode == 0);
    CHECK(anon->device_major == 0);
    CHECK(anon->device_minor == 0);
    CHECK(anon->category == MemoryMapCategory::Anonymous);
  }

  run("a pathname containing spaces is preserved");
  {
    const auto m = line(
        "7f0000000000-7f0000010000 r--p 00000000 08:02 42 "
        "/tmp/some file with spaces");
    CHECK(m.has_value());
    CHECK(m->pathname == "/tmp/some file with spaces");
  }

  // --- Range validation. --------------------------------------------------
  run("rejects empty, reversed and malformed address ranges");
  {
    CHECK(!line("55a000000000-55a000000000 r-xp 00000000 00:00 0").has_value());
    CHECK(!line("55a0000002000-55a0000001000 r-xp 00000000 00:00 0").has_value());
    CHECK(!line("nothex-000000001000 r-xp 00000000 00:00 0").has_value());
    CHECK(!line("55a000000000-abc r-xp 00000000 00:00 0").has_value());
    CHECK(!line("55a000000000 r-xp 00000000 00:00 0").has_value());  // no '-'
  }

  run("rejects lines with too few fields and bad field encodings");
  {
    CHECK(!line("").has_value());
    CHECK(!line("   ").has_value());
    CHECK(!line("55a000000000-55a000001000 r-xp 00000000 08:01").has_value());
    CHECK(!line("55a000000000-55a000001000 r-xp zzz 08:01 5").has_value());
    CHECK(!line("55a000000000-55a000001000 r-xp 00000000 08:z1 5").has_value());
    CHECK(!line("55a000000000-55a000001000 r-xp 00000000 08:01 zz").has_value());
  }

  run("a full-width range does not overflow size arithmetic");
  {
    const auto m = line("fffffffffffffffe-ffffffffffffffff r-xp 00000000 "
                        "00:00 0");
    CHECK(m.has_value());
    CHECK(m->size() == 1);
  }

  // --- parseMemoryMaps: whole-file parsing, ascending order, truncation. --
  run("parses a multi-line maps file in ascending order");
  {
    const std::string contents =
        "55a000000000-55a000021000 r-xp 00000000 08:01 111 /usr/bin/cat\n"
        "55a000021000-55a000043000 r--p 00021000 08:01 111 /usr/bin/cat\n"
        "55a000043000-55a000044000 rw-p 00042000 08:01 111 /usr/bin/cat\n"
        "55a001000000-55a001100000 rw-p 00000000 00:00 0 \n"
        "7f0000000000-7f0001000000 r-xp 00000000 08:01 222 "
        "/usr/lib/libc.so.6\n"
        "7ffd00000000-7ffd00020000 rw-p 00000000 00:00 0     [stack]\n"
        "7ffd00020000-7ffd00024000 r--s 00000000 00:00 0     [vvar]\n"
        "ffffffffff600000-ffffffffff601000 --xp 00000000 00:00 0 "
        "[vsyscall]\n";
    bool truncated = false;
    const std::vector<ProcessMemoryMap> maps =
        atm::parseMemoryMaps(contents, atm::kDefaultMaxMappings, truncated);
    CHECK(!truncated);
    CHECK(maps.size() == 8);
    for (std::size_t i = 1; i < maps.size(); ++i) {
      CHECK(maps[i - 1].start < maps[i].start);  // ascending by start
    }
    CHECK(maps.back().category == MemoryMapCategory::Vsyscall);
    CHECK(maps[6].category == MemoryMapCategory::Vvar);
    CHECK(maps[6].shared);  // r--s
  }

  run("malformed lines are skipped, not fatal");
  {
    const std::string contents =
        "garbage line that is not a maps entry\n"
        "55a000000000-55a000021000 r-xp 00000000 08:01 111 /usr/bin/cat\n"
        "\n"
        "55a000021000-55a000021000 r--p 00021000 08:01 111\n"
        "55a000030000-55a000031000 rw-p 00000000 00:00 0 .\n";
    bool truncated = false;
    const std::vector<ProcessMemoryMap> maps =
        atm::parseMemoryMaps(contents, atm::kDefaultMaxMappings, truncated);
    CHECK(!truncated);
    CHECK(maps.size() == 2);
    CHECK(maps[1].pathname == ".");
  }

  run("truncation stops at the limit and reports it");
  {
    std::string contents;
    for (int i = 0; i < 5; ++i) {
      char buf[128];
      std::snprintf(buf, sizeof(buf),
                    "%x0-9%x0 r-xp 00000000 08:01 %d /bin/f%d\n", i + 1, i + 1,
                    i, i);
      contents += buf;
    }
    bool truncated = false;
    const std::vector<ProcessMemoryMap> maps =
        atm::parseMemoryMaps(contents, /*max_mappings=*/2, truncated);
    CHECK(truncated);
    CHECK(maps.size() == 2);
  }

  // --- Classification. ----------------------------------------------------
  run("classification of libraries, executables and anonymous regions");
  {
    const auto lib = line("0-1000 r-xp 00000000 08:01 1 /usr/lib/libc.so.6");
    CHECK(lib.has_value());
    CHECK(lib->category == MemoryMapCategory::SharedLibrary);

    const auto lib_data =
        line("1000-2000 r--p 00000000 08:01 1 /usr/lib/libc.so.6");
    CHECK(lib_data.has_value());
    CHECK(lib_data->category == MemoryMapCategory::SharedLibrary);

    const auto prog = line("2000-3000 r-xp 00000000 08:01 2 /usr/bin/cat");
    CHECK(prog.has_value());
    CHECK(prog->category == MemoryMapCategory::Executable);

    const auto plain = line("3000-4000 r--p 00000000 08:01 3 /etc/hostname");
    CHECK(plain.has_value());
    CHECK(plain->category == MemoryMapCategory::FileBacked);

    const auto jit = line("4000-5000 rwxp 00000000 00:00 0");
    CHECK(jit.has_value());
    CHECK(jit->category == MemoryMapCategory::Executable);

    const auto anon = line("5000-6000 rw-p 00000000 00:00 0");
    CHECK(anon.has_value());
    CHECK(anon->category == MemoryMapCategory::Anonymous);
  }

  // --- Manager integration. -----------------------------------------------
  run("inspects the current process against its own identity");
  {
    const auto self = ProcessIdentity::current(::getpid());
    CHECK(self.has_value());
    if (!self.has_value()) {
      return EXIT_FAILURE;
    }
    const ProcessMemoryMapManager manager;
    const auto result = manager.inspect(*self);
    CHECK(result.status == MemoryMapStatus::Success);
    CHECK(!result.maps.empty());
    CHECK(result.total_bytes > 0);
    CHECK(result.executable_count > 0);
    CHECK(result.maps.size() > 1);
    bool ascending = true;
    for (std::size_t i = 1; i < result.maps.size(); ++i) {
      if (result.maps[i - 1].start >= result.maps[i].start) {
        ascending = false;
        break;
      }
    }
    CHECK(ascending);
    CHECK(result.anonymous_count > 0);
    CHECK(result.file_backed_count > 0);
  }

  run("rejects an invalid pid before any system access");
  {
    const ProcessMemoryMapManager manager;
    const auto result = manager.inspect(ProcessIdentity{0, 0});
    CHECK(result.status == MemoryMapStatus::InvalidPid);
    CHECK(!result.success());
  }

  run("refuses a reused PID (identity mismatch) and discards the result");
  {
    const auto self = ProcessIdentity::current(::getpid());
    CHECK(self.has_value());
    if (!self.has_value()) {
      return EXIT_FAILURE;
    }
    const ProcessIdentity reused{self->pid, self->starttime_ticks + 1};
    const ProcessMemoryMapManager manager;
    const auto result = manager.inspect(reused);
    CHECK(result.status == MemoryMapStatus::ProcessReused);
    CHECK(result.maps.empty());  // stale data is never returned
  }

  run("a vanished process yields IdentityUnknown / ProcessNotFound");
  {
    const ::pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
      _exit(0);
    }
    int status = 0;
    static_cast<void>(::waitpid(child, &status, 0));
    const ProcessMemoryMapManager manager;
    const auto result = manager.inspect(ProcessIdentity{child, 0});
    CHECK(result.status == MemoryMapStatus::IdentityUnknown ||
          result.status == MemoryMapStatus::ProcessNotFound);
    CHECK(result.maps.empty());
  }

  run("the mapping limit is enforced and truncation is reported");
  {
    const auto self = ProcessIdentity::current(::getpid());
    CHECK(self.has_value());
    if (!self.has_value()) {
      return EXIT_FAILURE;
    }
    const ProcessMemoryMapManager manager;
    const auto result = manager.inspect(*self, /*max_mappings=*/2);
    CHECK(result.status == MemoryMapStatus::Success);
    CHECK(result.truncated);
    CHECK(result.maps.size() == 2);
  }

  run("permission errors map to a meaningful error state (best effort)");
  {
    // A real EACCES/EPERM needs a process whose maps we cannot open; on plain
    // mounts every /proc/<pid>/maps is world-readable, so this scans a few
    // kernel PIDs and skips gracefully when none is restricted.
    const ProcessMemoryMapManager manager;
    bool found_denied = false;
    for (::pid_t pid = 1; pid <= 256 && !found_denied; ++pid) {
      const auto identity = ProcessIdentity::current(pid);
      if (!identity.has_value()) {
        continue;
      }
      const auto result = manager.inspect(*identity);
      if (result.status == MemoryMapStatus::PermissionDenied) {
        found_denied = true;
        CHECK(result.maps.empty());
      }
    }
    if (!found_denied) {
      std::fprintf(stderr, "  (no restricted process found: permission-denied "
                           "runtime case skipped)\n");
    }
  }

  std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
  if (g_failures != 0) {
    return EXIT_FAILURE;
  }
  std::fprintf(stderr, "ALL TESTS PASSED\n");
  return EXIT_SUCCESS;
}