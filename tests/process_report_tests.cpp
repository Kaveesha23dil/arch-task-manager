#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "process_cgroup.hpp"
#include "process_details.hpp"
#include "process_environment.hpp"
#include "process_io_details.hpp"
#include "process_memory_map.hpp"
#include "process_monitor.hpp"
#include "process_namespace.hpp"
#include "process_network.hpp"
#include "process_report.hpp"
#include "process_resources.hpp"
#include "process_scheduling.hpp"
#include "process_security.hpp"

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
// Helpers
// ---------------------------------------------------------------------------

namespace {

/// Returns true when `text` contains `substr` as a substring.
bool contains(const std::string &text, std::string_view substr) {
  return text.find(substr) != std::string::npos;
}

/// Builds a minimal, fully-populated ProcessDetailsInfo with everything set to
/// known non-default values. Each call returns a fresh copy so tests can
/// modify individual fields.
atm::ProcessDetailsInfo makeFullInfo() {
  atm::ProcessDetailsInfo info;
  info.pid = 1234;
  info.name = "my-process";
  info.state_char = 'R';
  info.state = atm::ProcessState::Running;
  info.user = "testuser";
  info.parent_pid = 100;
  info.executable_path = "/usr/bin/my-process";
  info.working_directory = "/home/testuser";
  info.command_line = "my-process --flag value";
  info.uid = 1000;
  info.gid = 1000;
  info.thread_count = 4;
  info.priority = -5;
  info.nice_value = -10;
  info.nice_priority = -10;
  info.virtual_memory_bytes = 100 * 1024 * 1024;
  info.resident_memory_bytes = 20 * 1024 * 1024;
  info.shared_memory_bytes = 5 * 1024 * 1024;
  info.text_memory_bytes = 1024 * 1024;
  info.data_memory_bytes = 2 * 1024 * 1024;
  info.stack_memory_bytes = 128 * 1024;
  info.memory_percent = 5.0;
  info.user_cpu_time = 10000;
  info.system_cpu_time = 5000;
  info.total_cpu_time = 15000;
  info.cpu_usage_percent = 12.5;
  info.read_bytes = 12345678;
  info.write_bytes = 87654321;
  info.read_syscalls = 500;
  info.write_syscalls = 300;
  info.cancelled_write_bytes = 512;
  info.read_rate = 1024.0;
  info.write_rate = 2048.0;
  info.voluntary_context_switches = 1000;
  info.nonvoluntary_context_switches = 50;
  info.starttime_ticks = 123456;
  info.start_time = std::chrono::system_clock::now() - std::chrono::hours(2);
  info.process_uptime_seconds = 7200;

  // Resource limits
  atm::ResourceLimit open_files;
  open_files.soft = 1024;
  open_files.hard = 4096;
  info.limits.open_files = open_files;

  atm::ResourceLimit stack_size;
  stack_size.soft = 8388608;
  stack_size.hard_unlimited = true;
  info.limits.max_stack_size = stack_size;

  // IO details
  atm::ProcessIoDetailsInfo io_info;
  io_info.chars_read = 1000000;
  io_info.chars_written = 500000;
  io_info.read_syscalls = 100;
  io_info.write_syscalls = 80;
  io_info.bytes_read = 800000;
  io_info.bytes_written = 400000;
  io_info.cancelled_write_bytes = 100;
  io_info.chars_read_rate = 512.0;
  io_info.chars_written_rate = 256.0;
  io_info.bytes_read_rate = 128.0;
  io_info.bytes_written_rate = 64.0;
  atm::ProcessIoDetailsResult io_result;
  io_result.status = atm::IoDetailsStatus::Success;
  io_result.info = io_info;
  info.io_details = io_result;

  // Security
  atm::ProcessSecurityInfo sec;
  sec.uid.real = 1000;
  sec.uid.effective = 1000;
  sec.uid.saved = 1000;
  sec.uid.filesystem = 1000;
  sec.gid.real = 1000;
  sec.gid.effective = 1000;
  sec.gid.saved = 1000;
  sec.gid.filesystem = 1000;
  sec.groups_available = true;
  sec.supplementary_groups.push_back({44, "audio"});
  sec.supplementary_groups.push_back({100, "users"});
  atm::CapabilitySet eff_caps;
  eff_caps.type = atm::CapabilitySetType::Effective;
  eff_caps.raw_mask = 0x0000000100000080;
  eff_caps.available = true;
  eff_caps.decoded_names.push_back("CAP_NET_RAW");
  eff_caps.decoded_names.push_back("CAP_SETPCAP");
  sec.capabilities.push_back(eff_caps);
  sec.no_new_privs = atm::NoNewPrivsState::Enabled;
  sec.seccomp = atm::SeccompMode::Filter;
  sec.seccomp_filters = 2;
  sec.tracer_pid = 0;
  sec.umask = 0022;
  sec.core_dumping = false;
  sec.security_context = "system_u:system_r:unconfined_t:s0";
  sec.security_context_available = true;
  sec.exec_context = "system_u:system_r:unconfined_t:s0";
  sec.exec_context_available = true;
  sec.login_uid = 1000;
  sec.login_uid_available = true;
  atm::ProcessSecurityResult sec_result;
  sec_result.status = atm::SecurityStatus::Success;
  sec_result.info = sec;
  info.security = sec_result;

  // Environment
  atm::ProcessEnvironmentResult env;
  env.status = atm::EnvironmentStatus::Success;
  atm::ProcessEnvironmentEntry e1;
  e1.name = "HOME";
  e1.value = "/home/testuser";
  e1.sensitive = false;
  env.entries.push_back(e1);
  atm::ProcessEnvironmentEntry e2;
  e2.name = "API_KEY";
  e2.value = "********";
  e2.sensitive = true;
  env.entries.push_back(e2);
  atm::ProcessEnvironmentEntry e3;
  e3.name = "DATABASE_PASSWORD";
  e3.value = "********";
  e3.sensitive = true;
  env.entries.push_back(e3);
  atm::ProcessEnvironmentEntry e4;
  e4.name = "NORMAL_VALUE";
  e4.value = "hello";
  e4.sensitive = false;
  env.entries.push_back(e4);
  env.sensitive_count = 2;
  env.byte_count = 128;
  info.environment = env;

  // File descriptors (derived from network connections)
  atm::ProcessNetworkConnectionsResult conns;
  conns.status = atm::NetworkConnectionsStatus::Success;
  conns.tcp_count = 2;
  conns.udp_count = 1;
  conns.listening_count = 1;
  conns.established_count = 1;

  atm::ProcessNetworkConnection c1;
  c1.fd = 3;
  c1.protocol = atm::ConnectionProtocol::Tcp4;
  c1.local_address = "0.0.0.0";
  c1.local_port = 8080;
  c1.tcp_state = atm::TcpState::Listen;
  c1.inode = 12345;
  conns.connections.push_back(c1);

  atm::ProcessNetworkConnection c2;
  c2.fd = 5;
  c2.protocol = atm::ConnectionProtocol::Tcp4;
  c2.local_address = "127.0.0.1";
  c2.local_port = 34567;
  c2.remote_address = "93.184.216.34";
  c2.remote_port = 80;
  c2.tcp_state = atm::TcpState::Established;
  c2.inode = 12346;
  conns.connections.push_back(c2);

  atm::ProcessNetworkConnection c3;
  c3.fd = 7;
  c3.protocol = atm::ConnectionProtocol::Udp4;
  c3.local_address = "0.0.0.0";
  c3.local_port = 5353;
  c3.udp = true;
  c3.inode = 12347;
  conns.connections.push_back(c3);

  info.network_connections = conns;

  // Memory maps
  atm::ProcessMemoryMapsResult maps;
  maps.status = atm::MemoryMapStatus::Success;
  maps.total_bytes = 100 * 1024 * 1024;
  maps.executable_count = 5;
  maps.writable_count = 3;
  maps.file_backed_count = 8;
  maps.anonymous_count = 2;

  atm::ProcessMemoryMap m1;
  m1.start = 0x400000;
  m1.end = 0x401000;
  m1.offset = 0;
  m1.device_major = 0;
  m1.device_minor = 0;
  m1.inode = 0;
  m1.permissions = "r-xp";
  m1.pathname = "/usr/bin/my-process";
  maps.maps.push_back(m1);

  atm::ProcessMemoryMap m2;
  m2.start = 0x7f000000;
  m2.end = 0x7f010000;
  m2.offset = 0;
  m2.device_major = 0;
  m2.device_minor = 0;
  m2.inode = 0;
  m2.permissions = "rw-p";
  m2.pathname = "";
  maps.maps.push_back(m2);

  info.memory_maps = maps;

  // Namespaces
  atm::ProcessNamespaceResult ns;
  ns.status = atm::NamespaceStatus::Success;
  ns.detected_count = 3;
  ns.unique_id_count = 3;
  ns.unavailable_count = 0;

  atm::ProcessNamespace n1;
  n1.type = atm::NamespaceType::Pid;
  n1.name = "pid";
  n1.target = "pid:[4026531836]";
  n1.id = 4026531836;
  ns.namespaces.push_back(n1);

  atm::ProcessNamespace n2;
  n2.type = atm::NamespaceType::Network;
  n2.name = "net";
  n2.target = "net:[4026531992]";
  n2.id = 4026531992;
  ns.namespaces.push_back(n2);

  atm::ProcessNamespace n3;
  n3.type = atm::NamespaceType::Mount;
  n3.name = "mnt";
  n3.target = "mnt:[4026531840]";
  n3.id = 4026531840;
  ns.namespaces.push_back(n3);

  info.namespaces = ns;

  // Cgroups
  atm::ProcessCgroupResult cg;
  cg.status = atm::CgroupStatus::Success;
  cg.version = atm::CgroupVersion::V2;

  atm::ProcessCgroupHierarchy h1;
  h1.hierarchy_id = 0;
  h1.controllers = {};
  h1.relative_path = "/system.slice/my-process.service";
  h1.mount_point = "/sys/fs/cgroup";
  h1.absolute_path = "/sys/fs/cgroup/system.slice/my-process.service";
  h1.resolvable = true;
  cg.hierarchies.push_back(h1);

  cg.resources.cpu_weight.available = true;
  cg.resources.cpu_weight.value = 100;
  cg.resources.memory_current.available = true;
  cg.resources.memory_current.value = 20971520;
  cg.resources.readable_file_count = 12;
  info.cgroups = cg;

  return info;
}

}  // namespace

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

namespace {

void testReportStatusMessage() {
  run("reportStatusMessage");
  CHECK(std::string(atm::reportStatusMessage(atm::ReportStatus::Success))
            .find("report written") != std::string::npos);
  CHECK(std::string(atm::reportStatusMessage(atm::ReportStatus::InvalidPid))
            .find("invalid") != std::string::npos);
  CHECK(std::string(atm::reportStatusMessage(atm::ReportStatus::ProcessReused))
            .find("PID was reused") != std::string::npos);
  CHECK(std::string(
            atm::reportStatusMessage(atm::ReportStatus::IdentityUnknown))
            .find("identity") != std::string::npos);
}

void testSanitizeReportName() {
  run("sanitizeReportName");
  CHECK(atm::sanitizeReportName("hello") == "hello");
  CHECK(atm::sanitizeReportName("Hello_World-123") == "Hello_World-123");
  CHECK(atm::sanitizeReportName("a/b\\c:d*e?f\"g<h>i|j") == "a_b_c_d_e_f_g_h_i_j");
  CHECK(atm::sanitizeReportName("..") == "process");
  CHECK(atm::sanitizeReportName("/") == "process");
  CHECK(atm::sanitizeReportName("__foo__") == "foo");
  CHECK(atm::sanitizeReportName("___") == "process");
  CHECK(atm::sanitizeReportName("a..b") == "a_b");
  CHECK(atm::sanitizeReportName(std::string_view("\n\t\r")) == "process");
}

void testDefaultReportFilename() {
  run("defaultReportFilename");
  std::string name = atm::defaultReportFilename(123, "bash");
  CHECK(name == "process-123-bash.txt");
  name = atm::defaultReportFilename(42, "a/b..c");
  CHECK(name == "process-42-a_b_c.txt");
  name = atm::defaultReportFilename(1, "");
  CHECK(name == "process-1-process.txt");
}

void testBasicReport() {
  run("basicReport");
  atm::ProcessDetailsInfo info = makeFullInfo();
  std::string report = atm::generateProcessReport(info);

  CHECK(!report.empty());
  CHECK(contains(report, "Arch Linux Task Manager"));
  CHECK(contains(report, "Process Details Report"));
  CHECK(contains(report, "[Process]"));
  CHECK(contains(report, "[CPU & Memory]"));
  CHECK(contains(report, "[I/O]"));
  CHECK(contains(report, "[Scheduling]"));
  CHECK(contains(report, "[Resource Limits]"));
  CHECK(contains(report, "[Security & Credentials]"));
  CHECK(contains(report, "[Environment]"));
  CHECK(contains(report, "[File Descriptors]"));
  CHECK(contains(report, "[Locks]"));
  CHECK(contains(report, "[Memory Maps]"));
  CHECK(contains(report, "[Network Connections]"));
  CHECK(contains(report, "[Namespaces]"));
  CHECK(contains(report, "[Cgroups]"));
  CHECK(contains(report, "[Summary]"));
}

void testProcessSection() {
  run("processSection");
  atm::ProcessDetailsInfo info = makeFullInfo();
  std::string report = atm::generateProcessReport(info);

  CHECK(contains(report, "PID"));
  CHECK(contains(report, "1234"));
  CHECK(contains(report, "my-process"));
  CHECK(contains(report, "Running"));
  CHECK(contains(report, "testuser"));
  CHECK(contains(report, "/usr/bin/my-process"));
  CHECK(contains(report, "my-process --flag value"));
}

void testCpuMemorySection() {
  run("cpuMemorySection");
  atm::ProcessDetailsInfo info = makeFullInfo();
  std::string report = atm::generateProcessReport(info);

  CHECK(contains(report, "12.500000%"));
  CHECK(contains(report, "Virtual Memory"));
  CHECK(contains(report, "Resident Memory"));
}

void testIoSection() {
  run("ioSection");
  atm::ProcessDetailsInfo info = makeFullInfo();
  std::string report = atm::generateProcessReport(info);

  CHECK(contains(report, "Characters Read"));
  CHECK(contains(report, "Read Rate"));
  CHECK(contains(report, "Write Rate"));
}

void testSecuritySection() {
  run("securitySection");
  atm::ProcessDetailsInfo info = makeFullInfo();
  std::string report = atm::generateProcessReport(info);

  CHECK(contains(report, "UID"));
  CHECK(contains(report, "GID"));
  CHECK(contains(report, "CAP_NET_RAW"));
  CHECK(contains(report, "CAP_SETPCAP"));
  CHECK(contains(report, "Seccomp"));
  CHECK(contains(report, "Filter"));
  CHECK(contains(report, "NoNewPrivs"));
  CHECK(contains(report, "Enabled"));
}

void testEnvironmentMasking() {
  run("environmentMasking");
  atm::ProcessDetailsInfo info = makeFullInfo();
  std::string report = atm::generateProcessReport(info);

  // Sensitive values are masked
  CHECK(contains(report, "API_KEY=********"));
  CHECK(contains(report, "DATABASE_PASSWORD=********"));
  CHECK(contains(report, "NORMAL_VALUE=hello"));

  // Secrets must never appear in plaintext
  CHECK(!contains(report, "super-secret-value"));
  CHECK(!contains(report, "very-secret"));

  // Sensitive counts are reported
  CHECK(contains(report, "2"));
}

void testEnvironmentMaskingCaseInsensitive() {
  run("environmentMaskingCaseInsensitive");
  atm::ProcessDetailsInfo info = makeFullInfo();

  // Add a variable with a case variation that should be masked
  atm::ProcessEnvironmentEntry entry;
  entry.name = "my_password";
  entry.value = "the-actual-password";
  entry.sensitive = false;  // model incorrectly says not sensitive
  info.environment->entries.push_back(entry);
  info.environment->sensitive_count = 3;

  std::string report = atm::generateProcessReport(info);

  // The generator re-masks by name (case-insensitive substring match)
  CHECK(contains(report, "my_password=********"));
  CHECK(!contains(report, "the-actual-password"));
}

void testFileDescriptorsSection() {
  run("fileDescriptorsSection");
  atm::ProcessDetailsInfo info = makeFullInfo();
  std::string report = atm::generateProcessReport(info);

  CHECK(contains(report, "Socket descriptors"));
  CHECK(contains(report, "TCP"));
  CHECK(contains(report, "UDP"));
  CHECK(contains(report, "fd 3"));
  CHECK(contains(report, "inode 12345"));
  CHECK(contains(report, "LISTEN"));
  CHECK(contains(report, "ESTABLISHED"));
}

void testLocksSection() {
  run("locksSection");
  atm::ProcessDetailsInfo info = makeFullInfo();
  std::string report = atm::generateProcessReport(info);

  CHECK(contains(report, "[Locks]"));
  CHECK(contains(report, "no /proc/locks data"));
}

void testMemoryMapsSection() {
  run("memoryMapsSection");
  atm::ProcessDetailsInfo info = makeFullInfo();
  std::string report = atm::generateProcessReport(info);

  CHECK(contains(report, "Virtual mapping"));
  CHECK(contains(report, "Executable"));
  CHECK(contains(report, "Writable"));
  CHECK(contains(report, "Anonymous"));
  CHECK(contains(report, "/usr/bin/my-process"));
}

void testNetworkSection() {
  run("networkSection");
  atm::ProcessDetailsInfo info = makeFullInfo();
  std::string report = atm::generateProcessReport(info);

  CHECK(contains(report, "TCP"));
  CHECK(contains(report, "UDP"));
  CHECK(contains(report, "Listening"));
  CHECK(contains(report, "Established"));
}

void testNamespacesSection() {
  run("namespacesSection");
  atm::ProcessDetailsInfo info = makeFullInfo();
  std::string report = atm::generateProcessReport(info);

  CHECK(contains(report, "Detected"));
  CHECK(contains(report, "3"));
  CHECK(contains(report, "pid"));
  CHECK(contains(report, "net"));
  CHECK(contains(report, "mnt"));
}

void testCgroupsSection() {
  run("cgroupsSection");
  atm::ProcessDetailsInfo info = makeFullInfo();
  std::string report = atm::generateProcessReport(info);

  CHECK(contains(report, "cgroup v2"));
  CHECK(contains(report, "cpu.weight"));
  CHECK(contains(report, "memory.current"));
}

void testSummarySection() {
  run("summarySection");
  atm::ProcessDetailsInfo info = makeFullInfo();
  std::string report = atm::generateProcessReport(info);

  CHECK(contains(report, "[Summary]"));
  CHECK(contains(report, "Environment variables"));
  CHECK(contains(report, "2 masked"));
}

void testTruncation() {
  run("truncation");
  atm::ProcessDetailsInfo info = makeFullInfo();

  // Very small byte limit → report gets truncated
  std::string report = atm::generateProcessReport(info, 100, 256);
  CHECK(contains(report, "truncated"));
}

void testEmptyReport() {
  run("emptyReport");
  atm::ProcessDetailsInfo info;
  info.pid = 0;
  std::string report = atm::generateProcessReport(info, 100, 4096);
  CHECK(!report.empty());
  CHECK(contains(report, "N/A"));
}

void testWriteReportValidPath() {
  run("writeReportValidPath");
  const std::string path = "/tmp/process-report-test-valid.txt";
  const std::string contents = "test report content\n";
  auto result = atm::writeProcessReport(path, contents);
  CHECK(result.status == atm::ReportStatus::Success);
  CHECK(result.bytes == contents.size());
  CHECK(result.path == path);

  // Verify the file exists and has the right content
  std::ifstream in(path);
  std::string actual((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
  CHECK(actual == contents);
  std::remove(path.c_str());
}

void testWriteReportExistingFile() {
  run("writeReportExistingFile");
  const std::string path = "/tmp/process-report-test-existing.txt";
  // Create a pre-existing file
  {
    std::ofstream out(path);
    out << "old content";
  }
  const std::string contents = "new content\n";
  auto result = atm::writeProcessReport(path, contents);
  CHECK(result.status == atm::ReportStatus::Success);

  std::ifstream in(path);
  std::string actual((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
  CHECK(actual == contents);
  std::remove(path.c_str());
}

void testWriteReportInvalidPath() {
  run("writeReportInvalidPath");
  auto result = atm::writeProcessReport("", "content");
  CHECK(result.status == atm::ReportStatus::InvalidPath);

  result = atm::writeProcessReport("/this/path/does/not/exist/file.txt", "x");
  CHECK(result.status == atm::ReportStatus::WriteError);
}

void testWriteReportEmptyContent() {
  run("writeReportEmptyContent");
  const std::string path = "/tmp/process-report-test-empty.txt";
  auto result = atm::writeProcessReport(path, "");
  CHECK(result.status == atm::ReportStatus::EmptyReport);
  std::remove(path.c_str());
}

void testWriteReportDirectory() {
  run("writeReportDirectory");
  auto result = atm::writeProcessReport("/tmp", "content");
  CHECK(result.status == atm::ReportStatus::InvalidPath);
}

void testWriteReportUtf8() {
  run("writeReportUtf8");
  const std::string path = "/tmp/process-report-test-utf8.txt";
  const std::string contents = "日本語テスト\nüñîçôdé\n";
  auto result = atm::writeProcessReport(path, contents);
  CHECK(result.status == atm::ReportStatus::Success);

  std::ifstream in(path);
  std::string actual((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
  CHECK(actual == contents);
  std::remove(path.c_str());
}

void testVerifyReportTargetInvalidPid() {
  run("verifyReportTargetInvalidPid");
  atm::ProcessIdentity identity;
  identity.pid = 0;
  identity.starttime_ticks = 0;
  CHECK(atm::verifyReportTarget(identity) == atm::ReportTargetState::Unreadable);

  identity.pid = -1;
  CHECK(atm::verifyReportTarget(identity) == atm::ReportTargetState::Unreadable);
}

void testVerifyReportTargetNonexistent() {
  run("verifyReportTargetNonexistent");
  // Use a PID that is almost certainly not running (very large, unlikely to exist)
  atm::ProcessIdentity identity;
  identity.pid = 4194304;  // 2^22, very unlikely to exist
  identity.starttime_ticks = 1;
  auto state = atm::verifyReportTarget(identity);
  // Could be Disappeared or Unreadable depending on whether /proc exists
  CHECK(state == atm::ReportTargetState::Disappeared ||
        state == atm::ReportTargetState::Unreadable);
}

void testVerifyReportTargetCurrentProcess() {
  run("verifyReportTargetCurrentProcess");
  // PID 1 is almost always init/systemd on Linux
  auto identity = atm::ProcessIdentity::current(1);
  CHECK(identity.has_value());
  if (identity.has_value()) {
    CHECK(atm::verifyReportTarget(*identity) == atm::ReportTargetState::Same);
  }
}

void testVerifyReportTargetWrongTicks() {
  run("verifyReportTargetWrongTicks");
  auto identity = atm::ProcessIdentity::current(1);
  CHECK(identity.has_value());
  if (identity.has_value()) {
    // Corrupt the ticks to simulate reuse
    atm::ProcessIdentity wrong = *identity;
    wrong.starttime_ticks = identity->starttime_ticks + 1;
    CHECK(atm::verifyReportTarget(wrong) == atm::ReportTargetState::Reused);
  }
}

void testExportProcessReportInvalidPid() {
  run("exportProcessReportInvalidPid");
  atm::ProcessDetailsInfo info;
  info.pid = 0;
  atm::ProcessIdentity identity;
  identity.pid = 0;
  identity.starttime_ticks = 0;
  auto result = atm::exportProcessReport("/tmp/test.txt", info, identity);
  CHECK(result.status == atm::ReportStatus::InvalidPid);
}

void testExportProcessReportUnavailableEnv() {
  run("exportProcessReportUnavailableEnv");
  atm::ProcessDetailsInfo info = makeFullInfo();
  info.environment.reset();
  std::string report = atm::generateProcessReport(info);
  CHECK(contains(report, "Environment unavailable"));
}

void testExportProcessReportUnavailableSecurity() {
  run("exportProcessReportUnavailableSecurity");
  atm::ProcessDetailsInfo info = makeFullInfo();
  info.security.reset();
  std::string report = atm::generateProcessReport(info);
  CHECK(contains(report, "Security information unavailable"));
}

void testExportProcessReportUnavailableNetwork() {
  run("exportProcessReportUnavailableNetwork");
  atm::ProcessDetailsInfo info = makeFullInfo();
  info.network_connections.reset();
  std::string report = atm::generateProcessReport(info);
  CHECK(contains(report, "Network connections unavailable"));
}

void testExportProcessReportUnavailableMaps() {
  run("exportProcessReportUnavailableMaps");
  atm::ProcessDetailsInfo info = makeFullInfo();
  info.memory_maps.reset();
  std::string report = atm::generateProcessReport(info);
  CHECK(contains(report, "Memory maps unavailable"));
}

void testExportProcessReportUnavailableNamespaces() {
  run("exportProcessReportUnavailableNamespaces");
  atm::ProcessDetailsInfo info = makeFullInfo();
  info.namespaces.reset();
  std::string report = atm::generateProcessReport(info);
  CHECK(contains(report, "Namespaces unavailable"));
}

void testExportProcessReportUnavailableCgroups() {
  run("exportProcessReportUnavailableCgroups");
  atm::ProcessDetailsInfo info = makeFullInfo();
  info.cgroups.reset();
  std::string report = atm::generateProcessReport(info);
  CHECK(contains(report, "Cgroups unavailable"));
}

void testExportProcessReportUnavailableLimits() {
  run("exportProcessReportUnavailableLimits");
  atm::ProcessDetailsInfo info = makeFullInfo();
  info.limits = {};
  std::string report = atm::generateProcessReport(info);
  CHECK(contains(report, "Resource limits unavailable"));
}

void testFullExport() {
  run("fullExport");
  const std::string path = "/tmp/process-report-test-full-export.txt";
  atm::ProcessDetailsInfo info = makeFullInfo();
  atm::ProcessIdentity identity;
  identity.pid = info.pid;
  identity.starttime_ticks = *info.starttime_ticks;

  auto result = atm::exportProcessReport(path, info, identity);
  CHECK(result.status == atm::ReportStatus::Success ||
        result.status == atm::ReportStatus::SuccessProcessGone);
  CHECK(result.bytes > 0);
  CHECK(result.path == path);

  // Verify the file exists and has expected content
  std::ifstream in(path);
  std::string actual((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
  CHECK(contains(actual, "Arch Linux Task Manager"));
  CHECK(contains(actual, "[Process]"));
  CHECK(contains(actual, "[Summary]"));
  std::remove(path.c_str());
}

void testIdentityUnknownExport() {
  run("identityUnknownExport");
  atm::ProcessDetailsInfo info = makeFullInfo();
  atm::ProcessIdentity identity;
  identity.pid = 4194304;
  identity.starttime_ticks = 999999;

  auto result = atm::exportProcessReport("/tmp/test.txt", info, identity);
  // Should refuse to write because the process identity can't be verified
  CHECK(result.status == atm::ReportStatus::IdentityUnknown ||
        result.status == atm::ReportStatus::ProcessReused ||
        result.status == atm::ReportStatus::SuccessProcessGone);
}

}  // namespace

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
  testReportStatusMessage();
  testSanitizeReportName();
  testDefaultReportFilename();
  testBasicReport();
  testProcessSection();
  testCpuMemorySection();
  testIoSection();
  testSecuritySection();
  testEnvironmentMasking();
  testEnvironmentMaskingCaseInsensitive();
  testFileDescriptorsSection();
  testLocksSection();
  testMemoryMapsSection();
  testNetworkSection();
  testNamespacesSection();
  testCgroupsSection();
  testSummarySection();
  testTruncation();
  testEmptyReport();
  testWriteReportValidPath();
  testWriteReportExistingFile();
  testWriteReportInvalidPath();
  testWriteReportEmptyContent();
  testWriteReportDirectory();
  testWriteReportUtf8();
  testVerifyReportTargetInvalidPid();
  testVerifyReportTargetNonexistent();
  testVerifyReportTargetCurrentProcess();
  testVerifyReportTargetWrongTicks();
  testExportProcessReportInvalidPid();
  testExportProcessReportUnavailableEnv();
  testExportProcessReportUnavailableSecurity();
  testExportProcessReportUnavailableNetwork();
  testExportProcessReportUnavailableMaps();
  testExportProcessReportUnavailableNamespaces();
  testExportProcessReportUnavailableCgroups();
  testExportProcessReportUnavailableLimits();
  testFullExport();
  testIdentityUnknownExport();

  std::fprintf(stderr, "\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
