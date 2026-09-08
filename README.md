# Arch Task Manager

A native Linux task manager / system monitor, built specifically for Arch
Linux. It reads system information **directly from Linux interfaces** such as
`/proc/stat`, `/proc/meminfo`, and `/proc/<pid>/` — no shelling out to `ps`,
`free`, `top`, `htop`, or other external tools.

> Stage: **Step 12** — CPU, RAM, swap, process monitoring, process actions, the
> process tree, disk/storage monitoring, network monitoring, GPU monitoring,
> temperature & hardware sensor monitoring, systemd service management,
> startup application management, and system information / hardware overview.
> Everything else on the roadmap is intentionally **not** implemented yet, but
> the code is structured so future modules can be added without rewriting the
> existing ones.

## Why is this being built?

To produce a lightweight, dependency-free system monitor written in modern
C++ that talks to the Linux kernel directly. Arch's philosophy is simplicity
and user control, so the goal is a tool with minimal bloat, readable code, and
no third-party runtime dependencies. It is developed incrementally: one
feature per milestone, hosted on GitHub.

## Current features

### Completed

- [x] **CPU monitoring** — overall CPU utilization via `/proc/stat`,
      calculated from the difference between two successive samples.
- [x] **RAM / Memory monitoring** — total, used, available, free, cached and
      buffered memory via `/proc/meminfo`.
- [x] **Swap monitoring** — total, used and free swap via `/proc/meminfo`.
- [x] **Process monitoring** — a live process table (PID, name, state, UID,
      CPU %, RAM, threads, command line plus total/running/sleeping/stopped/
      zombie counters) scanned directly from `/proc/<pid>/`.
- [x] **Process actions** — terminate (SIGTERM), kill (SIGKILL), pause
      (SIGSTOP), resume (SIGCONT) and change scheduling priority
      (`setpriority(2)`, niceness −20…19) for a process chosen from the live
      table, using the `kill(2)`/`setpriority(2)` system calls directly.
- [x] **Process tree** — a hierarchical parent/child view of the process
      population built from the PPID field of the existing `/proc/<pid>/stat`
      scan, with box-drawing connectors (├──, └──, │), orphan handling and
      dynamic tree statistics.
- [x] **Disk / storage monitoring** — physical filesystem capacities via
      `statvfs(2)` over the mounts listed in `/proc/mounts`, real-time
      read/write throughput from two samples of `/proc/diskstats`, and whole
      physical disks (sizes shown) enumerated from `/sys/block`. Virtual,
      temporary and network filesystems are filtered out so `/proc`, `/sys`,
      tmpfs, overlay, NFS etc. are never presented as disk capacity.
- [x] **Network monitoring** — per-interface RX/TX speeds computed from two
      samples of `/proc/net/dev`, cumulative RX/TX bytes, packet/error/dropped
      counters, physical link state from `/sys/class/net/<name>/operstate`,
      plus a per-interface detail screen (press `i`). Loopback is shown last
      and excluded from the aggregate totals.
- [x] **GPU monitoring** — automatic GPU detection from `/sys/class/drm/card*`
      with PCI vendor/model identification, and live GPU utilization, VRAM,
      clock and power where the vendor's kernel driver exposes them. AMD
      (amdgpu) reads `gpu_busy_percent`, VRAM files and `pp_dpm_sclk`; Intel
      (i915) reads the per-GT clocks and derives GT busy from the RC6
      residency delta; NVIDIA and unknown vendors report everything they
      cannot expose as "N/A". Unavailable metrics always degrade gracefully.
      Press `g` for the full per-GPU detail screen.
- [x] **Temperature & hardware sensors** — automatic discovery of every hwmon
      temperature and fan channel under `/sys/class/hwmon` (CPU cores/package
      from `coretemp`/`k10temp`, GPU from `amdgpu`/`nouveau`/`i915`, NVMe/SATA
      from `nvme`/`drivetemp`, motherboard/PCH/ACPI zones, plus fan RPM where
      the driver exposes it), with sensor labels when available and safe
      fallbacks when not. Temperatures are reported in °C with their
      `tempN_max`/`tempN_crit` limits and a simple NORMAL/WARM/HIGH/CRITICAL
      status; the `s` detail screen shows every sensor. Nothing is shelled out
      to `sensors`/`lm-sensors` and the app runs fine on hardware with no
      sensors at all.
- [x] **Systemd service management** — discover and list loaded `*.service`
      units through systemd's native D-Bus API (`org.freedesktop.systemd1`
      via `sd-bus`/`libsystemd`), with live runtime status (active/inactive/
      FAILED/activating/deactivating/reloading), boot-time enablement
      (enabled/disabled/static/…), a case-insensitive search, sortable
      columns, and a per-service detail view (description, load/active/sub
      state, enabled state, main PID, unit path). Management operations are
      available from the `u` detail screen: **Start / Stop / Restart /
      Enable / Disable**, each gated behind a confirmation prompt. No shell
      commands (`systemctl`, `system()`, `popen()`) are used — everything
      goes through the D-Bus API, so systemd's normal authorization/polkit
      rules apply and privileged operations that the user is not permitted
      to perform report a clear "Permission denied." error instead of
      crashing. The `u` screen never asks for a password. If systemd cannot
      be reached the section reports "Unable to connect to systemd." while
      every other monitor keeps working.
- [x] **Startup applications** — an XDG autostart manager that discovers
      desktop autostart entries from the system directories
      (`/etc/xdg/autostart` plus `$XDG_CONFIG_DIRS` entries) and the user's
      own `~/.config/autostart` (or `$XDG_CONFIG_HOME/autostart`), showing
      name, description, `Exec=` command, icon, scope (User/System), and the
      effective enabled state per the XDG rules (`Hidden=true`, `OnlyShowIn`/
      `NotShowIn`, `X-GNOME-Autostart-enabled=false`). It supports a
      case-insensitive search, sorting by name/enabled/scope, a per-application
      detail view, and **Enable / Disable** management from the `a` screen.
      Management never modifies `/etc/xdg`; enabling or disabling a system
      entry writes a user-level override (with `Hidden=true`/`false`) into the
      user's autostart directory, and unrelated fields/comments in `.desktop`
      files are preserved. This is configuration management only — commands
      are never executed — and it is deliberately separate from the systemd
      service manager (Step 10).
- [x] **System information & hardware overview** — a `SystemInfoProvider`
      module that reads and caches the high-level machine identity: hostname,
      operating system and distribution from `/etc/os-release`, kernel version
      and architecture via `uname(2)`, CPU model, logical/physical core counts
      from `/proc/cpuinfo`, total RAM and swap (reusing the existing memory
      monitor), uptime from `/proc/uptime`, GPU names (from the existing GPU
      monitor), and DMI hardware/firmware identity (manufacturer, model,
      motherboard, BIOS/UEFI) from `/sys/class/dmi/id`. Press `y` for the full
      breakdown, which also offers an on-demand **refresh** — the static files
      are read once and cached, never rescanned every second. Everything is
      strictly read-only and missing fields always degrade to "N/A".

### Planned

- [ ] Process tree — interactive expand/collapse (deferred to the GUI)
- [ ] Arch Linux package/update information
- [ ] Historical graphs
- [ ] System alerts
- [ ] GUI

## Technology

- Language: **C++20** (ISO standard, no GNU extensions)
- Build system: **CMake** (works for both Debug and Release)
- Compiler: **g++** (`GCC`)
- OS interfaces: the `/proc` and `/sys` filesystems, `statvfs(2)`,
  `sysconf(3)`, the standard `std::filesystem` API, and systemd's D-Bus API
  (`sd-bus`/`libsystemd`)
- Standard library plus `libsystemd` (the only third-party dependency; it
  provides the `sd-bus` D-Bus client used for systemd service management)
- No shelling out to external tools

## Build on Arch Linux

Requirements:

```bash
sudo pacman -S base-devel cmake systemd-libs
```

`base-devel` provides `g++`; `cmake` provides the build tooling; `systemd-libs`
provides the `libsystemd`/`sd-bus` D-Bus client used for systemd service
management (it is present by default on any Arch Linux system that boots
with systemd).

Configure and build:

```bash
cd arch-task-manager
mkdir -p build
cd build
cmake ..                # defaults to a Release build
cmake --build .
```

For a Debug build:

```bash
cmake -DCMAKE_BUILD_TYPE=Debug ..
cmake --build .
```

The compiler runs with `-Wall -Wextra -Wpedantic` enabled.

## Run

From the project root:

```bash
./build/arch-task-manager
```

Expected output (refreshed every second, in place):

```text
========================================
ARCH TASK MANAGER
=================

CPU Usage:       35.2%
Memory Usage:    52.4%

## SYSTEM INFORMATION

Operating System:    Arch Linux
Kernel:              6.x.x-arch1-1
Architecture:        x86_64
Uptime:              2 days, 5 hours, 32 minutes

## MEMORY

Total:           15.5 GB
...
## SWAP

Total:               3.8 GB
...

## STORAGE

FILESYSTEMS

## Mount Point                   Total      Used      Free   Type
/                               127 GB   51.4 GB   75.5 GB   Physical
/boot                           196 MB   97.8 MB   98.2 MB   Physical
Excluded: 24 virtual/temporary/network mount(s)

---

## DISK ACTIVITY

Read:                124 MB/s
Write:               34 MB/s

## DEVICES

## DEVICE         SIZE        READ      WRITE
sda             238 GB    124 MB/s    34 MB/s

## NETWORK

Interface         RX Speed    TX Speed        RX        TX  State
docker0            0.0 B/s     0.0 B/s     0.0 B     0.0 B  DOWN
enp1s0             0.0 B/s     0.0 B/s     0.0 B     0.0 B  DOWN
wlp2s0           802 B/s     410 B/s   94.8 MB    119 MB   UP
lo                 710 B/s     710 B/s    8.1 MB    8.1 MB   UNKNOWN (loopback)

Total RX: 802 B/s
Total TX: 410 B/s
Loopback traffic is excluded from the totals.
Detailed stats: press 'i' (then Enter)

## GPU

GPU                   Usage                VRAM     Clock
Kaby Lake-R GT2 [UHD  35.0%               N/A    300 MHz

Detailed GPU info: press 'g' (then Enter)

## SENSORS

CPU
Package id 0       52.0 °C
Core 0             49.0 °C
Core 1             51.0 °C
Core 2             48.0 °C
Core 3             50.0 °C

Motherboard
acpitz             42.0 °C

FANS
Fan 1              1240 RPM

Detailed sensor info: press 's' (then Enter)

## SYSTEMD SERVICES

Service                    Status       Enabled       Description
NetworkManager.service     active       enabled       Network Manager
bluetooth.service          inactive     disabled      Bluetooth service
systemd-journald.service   active       static        Journal Service

Service detail: press 'u' (then Enter)

## STARTUP APPLICATIONS

Name                    Enabled   Scope  Command
Discord                    Yes    User   /usr/bin/discord --start-minimized
Bluetooth Manager          Yes  System   blueman-applet
NetworkManager Applet      Yes  System   nm-applet

Startup apps: press 'a' (then Enter) to manage autostart

## PROCESSES

    PID  NAME             CPU       RAM     STATE
   1245  firefox         18.4%     1.2 GB  Running
   2187  code            12.1%     856 MB  Sleeping
    982  hyprland         4.2%     320 MB  Sleeping
    431  systemd          0.1%      18 MB  Sleeping

## Process Statistics

Total:               186
Running:               3
Sleeping:            178
Stopped:               1
Zombie:                4

---

Processes: 186
Sort: [1] CPU  [2] Memory  [3] PID  [4] Name (current: CPU)
View: [l] Process List  [t] Process Tree (current: List)
Manage: press 'm' (then Enter) to control a process by PID
Network detail: press 'i' (then Enter) to inspect an interface
GPU detail: press 'g' (then Enter) to inspect a GPU
Sensor detail: press 's' (then Enter) to inspect a sensor
Systemd services: press 'u' (then Enter) to manage services
Startup apps: press 'a' (then Enter) to manage autostart
System info: press 'y' (then Enter) for the hardware overview
Updating every 1 second...
```

Press `Ctrl+C` to stop.

### Choosing a view

On startup the application asks which view to show:

```text
Select view:
[1] Process List
[2] Process Tree

Enter a number, or press Enter for the default (Process List):
>
```

While it runs you can switch views at any time (then Enter):

- `l` — Process List (flat table)
- `t` — Process Tree (hierarchical)
- `i` — inspect one network interface in detail (list view only)
- `g` — read the full per-GPU breakdown (list view only)
- `s` — read the full sensor breakdown with limits and status (list view only)

### Sorting

The **table** is sorted by **CPU usage (descending)** by default. While the
application runs, press a digit **and then Enter** to change the sort order on
the next refresh:

- `1` — CPU usage (descending)
- `2` — Memory usage (descending)
- `3` — PID (ascending)
- `4` — Process name (ascending, case-insensitive)

The tree is always drawn with children ordered by **PID ascending** — sorting
options for the tree are deliberately not added yet to keep the CLI simple.
The same keys still work for the table after you switch back with `l`.

## Process tree

### How Linux parent/child relationships work

Every process on Linux (except the very first, PID 1) is created by exactly
one other process via `fork()`/`clone()` (or, across boot, by the kernel
spawning kernel threads). That creator is the *parent*; the new process is its
*child*. Each child holds one immutable **PPID (Parent PID)**; a normal
process never has more than one parent. Tree discipline follows a few rules:

- PID 1 (`systemd` on Arch) reparents any process whose parent died while it
  was still alive — an *orphan* is adopted by the init process, never left
  parentless.
- Zombie children are stored under their parent until it reaps them with
  `wait()`; a parent that exits without reaping leaves zombies behind.
- Kernel threads have PID 0 as their parent.

Because processes start and exit continuously, any two consecutive `/proc`
scans can disagree about the relationships: a parent can vanish before its
children are read, so the tree must be robust to references to PIDs that no
longer exist.

### How PPID is obtained

`ProcessMonitor` already reads `/proc/<pid>/stat` once per refresh — **the
tree does not rescan `/proc`**. The PPID is field 4 of that file, which is a
space-separated set of tokens *after* a `(...)` comm block (comm can contain
spaces and parentheses, so the parser takes everything between the first `(`
and the last `)` as the name):

```text
pid (comm) state ppid pgrp session tty_nr tpgid flags minflt ...
#                ^^^^ — this is the Parent PID
```

The value ends up in `Process.parent_pid` (see parseStat in
`src/process_monitor.cpp`). The process tree consumes that count, never the
raw `/proc` entry.

### How the tree is built

The `ProcessTree` module (`src/process_tree.cpp`) turns a `ProcessMonitor`
snapshot into a hierarchy in O(n):

1. A `std::unordered_map<pid_t, index>` indexes every process by PID once.
2. Each process whose PPID is present in the map is recorded as a child of
   that index; everything else (PPID 0, missing parent, self-reference) is
   flagged as a root.
3. Roots and child lists are sorted by PID ascending, so the drawing is
   deterministic no matter what order `/proc` returned.
4. A value-semantics `ProcessTreeNode { Process process; std::vector<...>
   children; }` hierarchy is materialized from the root set.

Self-references and (hypothetically malformed) parent/child cycles are
guarded, and recursion is depth-capped, so a pathological snapshot can never
crash the build.

### How orphan processes are handled

A process becomes a **root** of the tree in exactly three cases:

- its PPID is 0 (kernel threads);
- its PPID names a process that is **not** in the current snapshot — the
  parent died between scans (or is unreadable, e.g. another user's); or
- its PPID equals its own PID (defensive; the kernel never produces this).

Orphans are drawn flush-left like any other root, and a missing parent never
aborts the build. Because the tree is rebuilt from a fresh snapshot every
second, an orphan moves back under its (new) parent automatically as soon as
that parent is visible again.

### How the tree is rendered

Rendering is the recursive classic "tree glyphs" algorithm:

```text
systemd (1)
├── NetworkManager (512)
├── Hyprland (982)
│   ├── waybar (1201)
│   ├── kitty (1402)
│   │   └── bash (1403)
│   └── rofi (1510)
└── firefox (1245)
    ├── firefox (1251)
    └── firefox (1252)
```

- `├── ` heads a node that has following siblings, `└── ` the last one.
- A parent with more siblings extends the vertical bar `│   ` down to its
  children; a last child instead draws nothing there (`    `).
- Roots are flush-left and their children start at column 0.
- Each line is `name (pid)` — deliberately minimal so deep trees stay
  readable; the CPU/RAM columns provide the live numbers in the *table* view.

Above/below the tree, dynamic statistics are printed:

```text
Total processes: 186
Root processes: 5
Maximum tree depth: 8
```

`Maximum tree depth` counts a root as depth 1. An empty snapshot renders
`No processes found.`

### Process actions from the tree

Pressing `m` in the tree view shows the tree as a selection aid, then asks
exactly like the table flow does:

```text
Enter PID to manage (blank to cancel):
>
```

The tree is used **only to identify the selected PID**. Validation is
identical to the table path (protected PID 1 and the monitor's own PID are
refused, unknown PIDs are rejected), and the action menu reuses the same
`ProcessActions` module — `kill(2)` (SIGTERM/SIGKILL/SIGSTOP/SIGCONT) and
`setpriority(2)` are never re-implemented in the tree code.

## Process actions

Press `m` **and then Enter** at any time to enter process-control mode. The
current process list is shown frozen; enter a PID (or leave it blank to
cancel). PID `0`, PID `1` and the Task Manager's own PID are protected from
every action. A PID that is absent from the current list is rejected — the
list is at most one second old, so reusing the PID for a *different* (newer)
process is impossible.

Select an action from the menu:

| Key | Action                    | System call                 | Notes                                    |
| --- | ------------------------- | --------------------------- | ---------------------------------------- |
| 1   | Terminate                 | `kill(pid, SIGTERM)`        | graceful shutdown request               |
| 2   | Kill                      | `kill(pid, SIGKILL)`        | force; extra WARNING + confirmation     |
| 3   | Pause                     | `kill(pid, SIGSTOP)`        | freezes the process                     |
| 4   | Resume                    | `kill(pid, SIGCONT)`        | resumes a paused process                |
| 5   | Change priority           | `setpriority(PRIO_PROCESS)` | niceness −20…19; lower = higher priority|
| 6   | Cancel                    | —                           | back to the live view                   |

Every action requires a `[y/N]` confirmation; anything other than `y`/`Y`/
`yes` cancels. After the action the list is refreshed immediately so the
effect is visible without waiting for the next tick. Quitting the live view
(`Ctrl+C`, or EOF on stdin) also cancels any prompt.

### How errors are handled

All operations go through the `ProcessActions` module, which calls `kill(2)`
and `setpriority(2)` directly — no shell, no `system()`/`popen()`, no `ps`/
`kill`/`renice` commands. The `errno` from a failed call is mapped to a
readable message:

| errno  | Meaning                                                          |
| ------ | ---------------------------------------------------------------- |
| `ESRCH`| the process exited between selection and the call                |
| `EPERM`| hold the process's `kill` permission, but the pid is not yours or belongs to another user; raising priority above your hard nice limit (for example negative values for an unprivileged user) also lands here |
| `EINVAL`| invalid priority (already range-checked before the call)         |
| other  | generic failure, reported with `strerror(errno)`                 |

The application **never recommends running as root**. Lower-priority (nicer)
changes and signals to your own processes work without privileges; making a
process run *faster* by giving it a negative niceness normally requires being
root. `PPid`-based permission checks, `getpriority(2)` reports the current
niceness, which is shown before a change so the requested value is explicit.

### A safe way to test

A background `sleep` is a harmless target for every action:

```bash
sleep 300 &
# note its PID, then in the Task Manager:
#   m → <sleep's PID> → 3 (Pause)   → table shows state "Stopped"
#   m → <sleep's PID> → 4 (Resume)  → state back to "Sleeping"
#   m → <sleep's PID> → 5 → 19      → priority lowered, then back to 0
#   m → <sleep's PID> → 1 (Terminate)/2 (Kill)
```

To exercise the tree, build a real parent/child chain and watch it appear
(and later disappear) with `t` / `m`:

```bash
bash -c 'sleep 300 & exec sleep 400' &
#   t  → the new "bash" node appears with "sleep" children under it
#   kill <bash's PID>  → both sleeps are reparented under systemd, then,
#                         being orphans, they survive as flush-left roots
```

Orphaned children stay alive and simply move under their new parent (usually
PID 1) on the next refresh; they never disappear into a crash.

## Project structure

```text
arch-task-manager/
├── CMakeLists.txt
├── README.md
├── .gitignore
├── LICENSE
├── include/
│   ├── cpu_monitor.hpp         # CpuTimes, readCpuTimes(), CpuMonitor
│   ├── memory_monitor.hpp      # MemoryInfo, readMemoryInfo(), MemoryMonitor
│   ├── process_monitor.hpp     # Process, ProcessState, ProcessMonitor
│   ├── process_actions.hpp     # ProcessActions, ActionResult, ActionStatus
│   ├── process_tree.hpp        # ProcessTreeNode, ProcessTree, build/render
│   ├── disk_monitor.hpp        # DiskUsage, BlockDevice, DiskSnapshot, DiskMonitor
│   ├── network_monitor.hpp     # NetworkInterfaceStats, NetworkSnapshot, NetworkMonitor
│   ├── gpu_monitor.hpp         # GpuStats, GpuSnapshot, GpuMonitor, GPU format helpers
│   ├── sensor_monitor.hpp      # TemperatureSensor, FanSensor, SensorSnapshot, SensorMonitor
│   ├── systemd_manager.hpp     # SystemdService, SystemdSnapshot, SystemdManager (D-Bus)
│   ├── startup_manager.hpp     # StartupApplication, StartupSnapshot, StartupManager (XDG)
│   ├── system_info.hpp         # SystemInfo, SystemInfoProvider (OS/kernel/CPU/DMI), formatUptime()
│   └── format_bytes.hpp        # shared byte-formatter (KB/MB/GB, used by disk + network)
├── src/
│   ├── main.cpp                # UI loop: frame rendering + 1 s refresh + control flow
│   ├── cpu_monitor.cpp         # /proc/stat reading + utilization math
│   ├── memory_monitor.cpp      # /proc/meminfo reading + memory/swap math
│   ├── process_monitor.cpp     # /proc scanning + per-process parsing
│   ├── process_actions.cpp     # kill(2)/setpriority(2) wrappers + errno mapping
│   ├── process_tree.cpp        # PID/PPID tree build + box-drawing renderer
│   ├── disk_monitor.cpp        # statvfs(2) usage + /proc/diskstats rates + /sys/block
│   ├── network_monitor.cpp     # /proc/net/dev two-sample rates + operstate
│   ├── gpu_monitor.cpp         # /sys/class/drm GPU discovery + vendor metrics
│   ├── sensor_monitor.cpp      # /sys/class/hwmon temperature/fan discovery + readings
│   ├── systemd_manager.cpp     # sd-bus / org.freedesktop.systemd1 discovery + management
│   ├── startup_manager.cpp     # XDG autostart scan, .desktop parse, enable/disable
│   └── system_info.cpp         # gethostname/uname, os-release, cpuinfo, DMI read + caching
└── build/                      # generated; never committed to git
```

`build/` is git-ignored.

## How CPU usage is calculated

`/proc/stat` (first line `cpu ...`) contains eight monotonically increasing
counters since boot, in units of `USER_HZ` (normally 100 ticks per second):

```text
cpu  user  nice  system  idle  iowait  irq  softirq  steal
```

To get utilization for a time window, we take **two samples** and subtract:

```text
idle  = idle + iowait
total = user + nice + system + idle + iowait + irq + softirq + steal

Δtotal = total_now − total_prev
Δidle  = idle_now  − idle_prev

usage% = (Δtotal − Δidle) / Δtotal * 100
```

This is a fraction of the **whole machine**, i.e. all online CPUs combined.

`iowait` counts time the CPU spent idle while waiting on I/O, so it is
treated as idle. The application records one baseline sample on startup, then
reads a new sample every second and prints the percentage computed from the
difference, never the raw counters.

## How process information is obtained

### Discovering processes

The kernel exposes every thread group as a directory named after its PID,
directly under `/proc`. Process discovery is therefore a directory scan of
`/proc`: every entry whose name is **all digits** is a process, and the name
is its PID. Nothing is ever shelled out to `ps`/`pgrep`/`top`.

For each PID four files are read:

| File                | What it provides                                              |
| ------------------- | ------------------------------------------------------------- |
| `/proc/<pid>/stat`  | name (comm), state character, parent PID, utime/stime, threads |
| `/proc/<pid>/status`| state, real UID, thread count, VmRSS (resident memory)         |
| `/proc/<pid>/cmdline`| full argv as a NUL-separated buffer (empty for kernel threads) |

### Process memory

Resident memory (`memory_kib`) is the **VmRSS** value from
`/proc/<pid>/status`, reported by the kernel in kB and formatted as kB/MB/GB
for display. Memory percentage is the share of total system RAM:

```text
memory% = VmRSS (kB) ÷ MemTotal (kB) × 100
```

`MemTotal` comes from the existing `MemoryMonitor` (one `/proc/meminfo` read
per second, shared with the memory section) so the two modules never parse it
twice. The kernel's raw state character (e.g. `R`) is kept internally; the
mapping shown to the user is:

| Char | Meaning     |
| ---- | ----------- |
| R    | Running     |
| S    | Sleeping    |
| D    | Disk Sleep  |
| T/t  | Stopped     |
| Z/X  | Zombie      |
| I    | Idle        |

### Per-process CPU usage and its convention

Per-process CPU uses the same two-sample-delta technique as the system CPU,
but with the convention **100% = one full online CPU (thread)**, so a process
spinning one core reads ~100% and a process spread over multiple cores can
read >100%:

```text
process_delta  = (utime + stime)_now − (utime + stime)_prev   # /proc/<pid>/stat
total_delta    = total CPU ticks_now − total CPU ticks_prev   # /proc/stat
number_of_cpus = count of online CPUs                          # sysconf(_SC_NPROCESSORS_ONLN)

usage% = process_delta ÷ total_delta × number_of_cpus × 100
```

Both per-process and system counters are in the same `USER_HZ` tick units, so
the division yields the fraction of one core the process used during the
elapsed window. The system-wide "CPU Usage" heading, by contrast, is
normalized to the whole machine (0–100% total), which is why individual
processes and the machine summary are intentionally on different scales.

Because the per-process percentage is a delta, the **first** scan only
records a baseline and reports 0% for every process; from the second scan on
each refresh reports the usage of the preceding ~1 s window.

### Why processes can disappear, and how errors are handled

Scanning `/proc` is racy by nature. A process can:

- exit between the directory scan and the `stat` read;
- be a zombie whose `status`/`cmdline` files are empty or gone;
- belong to another user, making `/proc/<pid>/*` unreadable (EPERM).

Every per-process file read is optional: opening or parsing it returns
`std::nullopt` and that process is **silently skipped**, including malformed
`stat` lines (comm contains spaces/parentheses, so it is parsed between the
first `(` and last `)`). No per-process failure is printed — a normal system
continuously spawns and reaps short-lived processes, and spamming the screen
with "process gone" errors would be useless. Numeric node parsing uses
`std::from_chars` (it cannot throw), the `/proc` scan itself is wrapped in a
try/catch for safety, and the program never terminates because a process
disappeared. The only fatal errors are a missing `/proc/stat` or
`/proc/meminfo`, which are reported once.

### Process statistics

The counters are derived from the state mapped above: `Running` = `R`,
`Sleeping` = `S`/`I`/`D`, `Stopped` = `T`/`t`, `Zombie` = `Z`/`X`. Unreadable
or unrecognized states count toward `Total` only.

## How RAM / swap usage is obtained

Memory information comes from `/proc/meminfo`, a kernel-generated file with
one `Field: value kB` pair per line. The relevant fields:

```text
MemTotal:      the total amount of physical memory
MemFree:       memory completely unused
MemAvailable:  an estimate of memory available to start new applications
Cached:        memory used for page cache (includes reclaimable slab)
Buffers:       memory used for raw disk buffers
SwapTotal:     total swap space
SwapFree:      swap space currently unused
```

All values are reported by the kernel in kibibytes (kB, 1024 bytes); the
application converts them to MB/GB for display. The reported figures are
derived as follows:

```text
Used RAM    = MemTotal − MemAvailable
RAM usage%  = Used RAM ÷ MemTotal × 100
Swap used   = SwapTotal − SwapFree
Swap usage% = Swap used ÷ SwapTotal × 100
```

`SwapTotal` (and thus swap usage) is 0% when no swap is configured; the
application guards that division-by-zero case instead of crashing.

## How disk / storage monitoring works

Disk monitoring is split into three independent parts, all reading
kernel-provided interfaces — nothing is ever shelled out to `df`, `du`,
`lsblk`, `iostat`, `iotop`, `stat`, `free`, or `ps`.

### Filesystem usage (capacities)

1. **Mount discovery** — `/proc/mounts` is parsed line-by-line. The fields are
   `device mount_point filesystem`; because literal spaces in paths are escaped
   there as octal sequences (`\040`, `\011`, `\012`, `\134`, …), each token is
   whitespace-split first and then unescaped, so mount points with spaces work.
2. **Capacity** — for each mount, `statvfs(2)` is called on the mount point.
   `statvfs(3)` returns a `struct statvfs` whose relevant fields are:
   - `f_frsize` — fundamental block size (the unit every other count is in;
     `f_bsize` is only a fallback when it is zero);
   - `f_blocks` — total blocks;
   - `f_bavail` — blocks available to *unprivileged* users (excludes
     root-reserved space; this is what `df` reports under **Avail**).

   Capacity is then:

   ```text
   total     = f_blocks × f_frsize
   available = f_bavail × f_frsize
   used      = total − available          # clamped to zero
   usage %   = used ÷ total × 100          # 0 when total is zero
   ```

   A filesystem whose capacity is zero, or a mount that vanished between the
   scan and the `statvfs` call, is skipped silently — never a crash and never
   terminal spam.

### Read/write speed

`/proc/diskstats` prints one line per block device with the three classic
counters:

```text
major minor name  reads  reads_merged  sectors_read  time_reading  writes  writes_merged  sectors_written  ...
   8     0   sda   68316      24338       5562359         32297     50580       57294        2966812     ...
  ^---  family counters: reads completed, reads merged, sectors read -------^                    ^---- sectors written
```

Newer kernels append discard/flush counters after these; the parser reads only
the first ten fields and ignores the rest. The `diskstats` ABI historically
counts **sectors as 512-byte units**, which is why the default sector size is
512.

Throughput is the classic two-sample-delta technique used by CPU monitoring:

1. Record the current `sectors_read`/`sectors_written` for every device,
   together with a `steady_clock` timestamp.
2. After the ~1 s refresh interval, re-read `/proc/diskstats`.
3. `bytes = Δsectors × sector_size`; `rate = bytes ÷ elapsed_seconds`,
   using the *real* elapsed time, never an assumed 1 s.
4. If a counter is smaller than the previous sample (rollover, or a device was
   torn down and reused), the baseline is silently reset to the new value
   instead of reporting a bogus negative delta.

The sector size prefers the device's advertised logical block size
(`/sys/class/block/<name>/queue/logical_block_size`, falling back to
`hw_sector_size`) and only falls back to the 512-byte ABI default when neither
is readable. The aggregate **Read / Write** figures are the sum over whole
disk devices. A brand-new device gets a baseline and shows a rate from the
next tick onward.

### Physical block devices

Whole disks are discovered by enumerating `/sys/block` (each entry is a whole
disk: `sda`, `nvme0n1`, `mmcblk0`, `sr0`, … — partitions like `sda1` live
under `/sys/class/block` and are not listed as disks). Obvious virtual devices
are filtered by name prefix (`loop*`, `ram*`, `zram*`, `dm-*`, `md*`, `fd*`,
`drbd`, `rbd`) so they are not shown as physical storage. The capacity shown
is the device's size since boot, read from `/sys/class/block/<name>/size`
(a count of 512-byte sectors, multiplied by 512 → bytes). Names are sorted,
device types are not assumed — SATA/SCSI (`sd`), NVMe (`nvme0n1`), USB,
MMC, and optical (`sr`) all work generically.

### How virtual filesystems are handled

Every filesystem type is classified:

| Category    | Types (examples)                                     | In the UI                       |
| ----------- | ---------------------------------------------------- | ------------------------------- |
| Physical    | `ext2` `ext3` `ext4` `btrfs` `xfs` `vfat` `ntfs` `iso9660` `zfs` … | shown with capacity |
| Temporary   | `tmpfs` `ramfs`                                       | excluded (counted)              |
| Network     | `nfs` `nfs4` `cifs` `smbfs` `9p` `ceph` `sshfs` `fuse.sshfs` … | excluded (counted)     |
| Virtual     | `proc` `sysfs` `devpts` `devtmpfs` `cgroup`/`cgroup2` `pstore` `bpf` `autofs` `overlay` generic `fuse` … | excluded (counted) |

Only **Physical** mounts appear in the FILESYSTEMS table. `/proc`, `/sys`,
`/dev`, `/run`, tmpfs, overlay and network mounts are filtered out and counted
on a single footer line (`Excluded: N virtual/temporary/network mount(s)`), so
they can never be mistaken for physical disk capacity. Everything not in a
known physical/network/temporary set is conservatively treated as Virtual.

The tree view deliberately does not embed the storage sections: it stays
focused on the process hierarchy, and the storage section belongs to the table
view.

### A safe way to test

On an idle machine the rates read near 0. A small, short-lived write to a real
disk filesystem makes the numbers move — for example (not on a tmpfs mount!):

```bash
dd if=/dev/zero of=~/burst.bin bs=32M count=2 conv=fdatasync && rm ~/burst.bin
```

The write rate column should jump during the flush window.

## How network monitoring works

Network monitoring reads `/proc/net/dev` directly — nothing is ever shelled
out to `ip`, `ifconfig`, `ss`, `bmon`, or `nload`.

### `/proc/net/dev`

Each non-bridge interface contributes one line:

```text
Inter-|   Receive                                                |  Transmit
 face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed
    lo: 8485632    19841    0    0    0     0          0         0  8485632    19841    0    0    0     0          0         0
wlp2s0: 99496400   269910    0    0    0     0          0         0 124783716   108928    0    0    0     0          0         0
```

Per interface the parser reads 16 numeric, whitespace-separated columns
(`bytes packets errs drop` × receive/transmit) and ignores everything after
them, so the file keeps working if the kernel appends more columns in the
future. The name column ends before a `:`. Lines may be indented (`lo:`) or
flush-left (`wlp2s0:`), and the header and blank line are skipped.

### RX / TX speed

Speeds use the same two-sample-delta technique as CPU and disk monitoring:

1. Record each interface's `rx_bytes`/`tx_bytes` plus a `steady_clock`
   timestamp (the startup read only establishes a baseline, so no speeds on
   the first frame).
2. After the ~1 s refresh interval, re-read `/proc/net/dev`.
3. `rate = Δbytes ÷ real_elapsed_seconds`, using the real elapsed time, never
   an assumed 1 s.
4. A counter that is *smaller* than the previous sample means the counters
   were reset (interface recreated, driver reload); the interface's baseline
   is silently re-seeded that tick instead of reporting a negative rate.

### Link state, loopback, totals

- The physical link state is read from `/sys/class/net/<name>/operstate`
  (`up`, `down`, `unknown`, `dormant`, …); a missing file (an interface that
  vanished mid-scan) defaults to `unknown`.
- **Loopback** (`lo`) is drawn last and flagged `(loopback)` so it is never
  mistaken for a real connection; the **Total RX / Total TX** figures sum
  non-loopback interfaces only.
- Non-loopback interfaces are sorted by name; the aggregate is the sum over
  the interfaces shown, matching what `ss`/`nload` would report for the
  machine's real uplink.

### The `i` detail screen

`i` (list view) freezes the network table and asks for an interface name:

```text
Enter interface name to inspect (blank to cancel):
>
```

It then prints the full breakdown — RX/TX speed, cumulative bytes, packets
(with thousands separators), errors, dropped, and link state, plus a
`Loopback: yes` note where applicable. The snapshot used is the most recent
one (at most ~1 s old); a blank name cancels and an unknown name is rejected,
so the detail screen never sleeps on a vanished interface. Like the tree view,
the `i` screen is available from list view; the tree view deliberately stays a
pure process hierarchy.

## How GPU monitoring works

GPU monitoring (`GpuMonitor`, `src/gpu_monitor.cpp`) reads the kernel's DRM
and driver sysfs interfaces directly — nothing is ever shelled out to
`nvidia-smi`, `glxinfo`, `intel_gpu_top`, or `lspci`, and no GPU *control*
(overclocking, fan/voltage/power-limit changes, resets) is performed or even
possible. The UI is `## GPU` in the live list view, plus a `g` detail screen
showing every metric per GPU.

### GPU detection

Available GPUs are discovered by enumerating `/sys/class/drm/card*` — the
kernel's *DRM primary nodes*. Each `cardN` maps to one graphics device (or
display adapter): `card0`, `card1`, … Connectors (`card1-eDP-1`) and render
nodes (`renderD128`) are not primary nodes and are skipped. Devices are
ordered by card number ascending.

Per card, stable identity is read once and cached from the kernel:

| Source                                  | Provides                        |
| --------------------------------------- | ------------------------------- |
| `card*/device/vendor`                   | PCI vendor ID (`0x8086`)        |
| `card*/device/device`                   | PCI device ID (`0x5917`)        |
| `card*/device/driver`  (symlink target) | bound driver (`i915`, `amdgpu`) |
| `card*/device/uevent`                   | `PCI_SLOT_NAME` (`0000:00:02.0`)|

A friendly model name is resolved from the read-only PCI ID database
(`/usr/share/hwdata/pci.ids` or `/usr/share/misc/pci.ids` when installed,
e.g. *"Kaby Lake-R GT2 [UHD Graphics 620]"*). If the database is missing the
name falls back to `Vendor 0x<device-id>`. The vendor comes from the PCI
vendor ID via a small fixed mapping (`0x1002` AMD, `0x10de` NVIDIA,
`0x8086` Intel); anything else is reported as *Unknown*.

### Supported vendors and metrics

What is actually available depends entirely on the GPU and its Linux driver.
Every metric is `std::optional` internally and is rendered as `N/A` when the
driver does not expose it — an unsupported value is never faked as 0.

| Vendor  | Usage                      | VRAM                     | Clock                          | Power                |
| ------- | -------------------------- | ------------------------ | ------------------------------ | -------------------- |
| AMD     | `gpu_busy_percent`         | `mem_info_vram_total/used` | `pp_dpm_sclk` (active state) | hwmon `power1_input` |
| Intel   | RC6 residency delta        | — (shared system RAM)    | `gt/gtN/rps_act_freq_mhz` / `gt_act_freq_mhz` | hwmon `power1_input` (rare) |
| NVIDIA  | —                          | —                        | —                              | hwmon `power1_input` (rare) |
| Unknown | —                          | —                        | —                              | hwmon `power1_input` (rare) |

All of these are read **only if the file exists**; `read()` never fails or
crashes because a value is absent, malformed, unreadable, or the GPU was
removed. The dynamic metrics are refreshed every tick of the main loop (the
monitor never runs its own thread), while identity is cached and only
re-discovered if the set of DRM primary nodes changes.

> **NVIDIA note.** The proprietary `nvidia` driver does not export these
> values through generic kernel interfaces, so NVIDIA GPUs are detected
> (vendor, model, driver) and reported with `N/A` for utilization/VRAM/clock
> unless a hwmon power sensor happens to exist. The application deliberately
> does **not** shell out to `nvidia-smi` every second — that would be a
> fragile, potentially leaking dependency.

### GPU utilization

- **AMD:** the kernel exposes `gpu_busy_percent` (already 0–100), read
  directly.
- **Intel:** the kernel exposes cumulative `rc6_residency_ms` (time the GT
  spent in its deepest sleep state) per GT engine. The monitor samples it
  like `CpuMonitor` samples `/proc/stat` and derives busy time from the delta:

  ```text
  busy% = 100 − rc6_delta_ms ÷ wall_elapsed_ms × 100   (clamped to 0–100)
  ```

  This is the same measurement `intel_gpu_top` uses for older kernels. The
  first sample only records a baseline, so `N/A` appears on the very first
  frame and a real percentage from the second tick on. A counter reset (RC6
  counter smaller than before) re-seeds the baseline instead of printing a
  bogus figure.
- **Everything else:** displayed as `Usage: N/A`.

### VRAM

AMD GPUs expose `mem_info_vram_total` and `mem_info_vram_used` in bytes;
`free` and `usage%` are derived with a division-by-zero guard (`usage% = used
÷ total × 100`, computed only when `total > 0`). Intel iGPUs share system RAM
and expose no VRAM interface, so the whole memory block is `N/A`. In the
summary table VRAM shows as `used/total`, e.g. `3.2 GB/8.0 GB`.

### Frequency and power

Clock is reported as `1200 MHz` below 1 GHz and `1.20 GHz` at/above it. Power
is read from the generic hwmon `power1_input` sensor (microwatts converted to
watts, shown as `45.2 W`) when the driver exposes one; it is **never guessed**
from utilization. Both degrade to `N/A` when unavailable.

### The `g` detail screen

`g` (list view) shows the frozen GPU summary followed by the full per-GPU
breakdown — name, vendor, driver, PCI slot, usage, memory (Total/Used/Free/
Usage) and clock/power:

```text
GPU 0 (card1)

Name:                Kaby Lake-R GT2 [UHD Graphics 620]
Vendor:              Intel
Driver:              i915
PCI Slot:            0000:00:02.0

Usage: 35.0%

Memory:
  Total:             N/A
  Used:              N/A
  Free:              N/A
  Usage:             N/A

Frequency: 300 MHz
Power: N/A
```

Multiple GPUs each get their own block, labeled `GPU 0`, `GPU 1`, … Press
Enter to return to the live view.

## Temperature & Hardware Sensors

Temperature and fan monitoring is provided by `SensorMonitor`
(`src/sensor_monitor.cpp`). It reads the kernel's hwmon interface directly —
**nothing is ever shelled out to `sensors(1)`, `lm-sensors` or `watch`**, and
it performs no hardware *control* (fan speed, overclocking, voltage or power
settings are never written; the module is strictly read-only).

### `/sys/class/hwmon`

The Linux hwmon subsystem exposes every kernel-registered hardware monitoring
device under `/sys/class/hwmon/hwmonN/`. Each device carries a `name` file
(such as `coretemp`, `k10temp`, `amdgpu`, `nvme`, `thinkpad` or `nct6775`)
plus numbered attribute channels:

```text
temp1_input   temp1_label   temp1_max   temp1_crit
fan1_input    fan1_label
```

Temperature values are reported in **millidegrees Celsius** (`45000` = `45.0 °C`)
and converted with `value / 1000.0`. `SensorMonitor` enumerates every `hwmonN`
directory and every `temp*_input` / `fan*_input` channel; it never hard-codes
a driver, so whatever sensors the current hardware and loaded drivers expose
are picked up automatically.

### Sensor discovery and refresh

- **Discovery** is static: `discover()` reads each device's `name`, resolves
  each channel's label (`tempN_label` / `fanN_label`, falling back to
  "Temperature N" / "Fan N"), classifies the channel, and caches the file
  paths. This happens once at startup.
- **Refresh** reads only the dynamic values (`tempN_input`, `fanN_input` and
  the `tempN_max`/`tempN_crit` limits) every tick of the main loop. As a
  guard against hotplugged/unplugged hardware, each refresh cheaply re-lists
  the `/sys/class/hwmon` directory and re-runs discovery only when the set of
  devices changes.

### What is monitored

| Category     | Driver / devices               | Example                    |
| ------------ | ------------------------------ | -------------------------- |
| CPU          | `coretemp`, `k10temp`, `x86_pkg_temp`, `zenpower`, … | `Package id 0`, `Core 0` … |
| GPU          | `amdgpu`, `nouveau`, `radeon`, `nvidia`, `i915` | "edge" / "junction" temps, shown with the GPU model |
| Storage      | `nvme`, `drivetemp` (SATA), `ataN` | NVMe `Composite`, SATA drive temp |
| Motherboard  | `acpitz`, `pch_*`, `it87`, `nct6775`, … | ACPI zones, PCH/chipset temp |
| Fans         | any `fanN_input`               | CPU/chassis fan RPM        |

Channel types are derived from the hwmon **device name and label together**,
never from the label alone. Sensors that do not fit a known category fall
back to `Other` and are shown in the same group as motherboard sensors.

### Limits and status

Where the driver exposes them, the `tempN_max` and `tempN_crit` limits are
shown and a simple status is derived:

| Status    | Meaning                                             |
| --------- | --------------------------------------------------- |
| `NORMAL`  | below 85% of the nearest operating limit            |
| `WARM`    | 85% … limit                                         |
| `HIGH`    | at/above `tempN_max`                                |
| `CRITICAL`| at/above `tempN_crit`                               |
| `N/A`     | no `max`/`crit` exposed — the application never invents limits |

This is display-only; there is no alerting or notification in this step.

### The `s` detail screen

`s` (list view) prints every temperature with its `Current` / `Maximum` /
`Critical` / `Status` block grouped per hwmon device, followed by every fan's
speed:

```text
CPU Package
--------------------------------
Current:             52.0 °C
Maximum:             95.0 °C
Critical:            100.0 °C
Status:              NORMAL
```

### Driver and hardware limitations

Sensor availability depends entirely on the hardware and the loaded Linux
drivers:

- **CPU temperatures** need the CPU's hwmon driver loaded (`coretemp` on Intel,
  `k10temp` on AMD). A missing package or core sensor simply means that
  channel is absent.
- **GPU temperatures** come from the driver's hwmon device (`amdgpu` exposes
  edge/junction/SOC/HBM temps; some `nouveau` cards expose them too). The
  proprietary `nvidia` driver exposes them only through non-standard files, so
  NVIDIA temperatures may be missing; the section simply omits them.
- **Storage temperatures** are only present when the drive exposes them
  (NVMe controllers report a `Composite` temperature; only some SATA/USB
  drives do, via `drivetemp`).
- **Fans** appear only when the machine has a measurable, driver-exposed fan.
- A laptop battery or AC-adapter hwmon device contributes no temperature; it is
  discovered but not shown.

Any channel whose value is missing, malformed, unreadable or implausible is
skipped for that refresh — a single broken sensor never crashes the
application, and a machine with **no** sensors at all simply shows:

```text
## SENSORS

No hardware temperature sensors available.
```

## Systemd Services

Systemd service management is implemented as a dedicated `SystemdManager`
module and is kept strictly separate from the process manager. It talks to
systemd entirely through its native D-Bus API — **no** `systemctl`,
`system()` or `popen()` is used anywhere.

### Systemd / D-Bus integration

The module connects to the system D-Bus via the `sd-bus` client from
`libsystemd` and interacts with the primary service manager:

- Destination: `org.freedesktop.systemd1`
- Path: `/org/freedesktop/systemd1`
- Interfaces: `org.freedesktop.systemd1.Manager`, `org.freedesktop.systemd1.Unit`

Methods and properties used:

| Purpose | Call |
|---|---|
| List loaded services | `Manager.ListUnits` |
| Per-unit boot enablement | `Unit` `UnitFileState` property |
| Main process PID | `Unit` `MainPID` property |
| Start | `Manager.StartUnit(name, "replace")` |
| Stop | `Manager.StopUnit(name, "replace")` |
| Restart | `Manager.RestartUnit(name, "replace")` |
| Reload | `Manager.ReloadUnit(name, "replace")` |
| Enable | `Manager.EnableUnitFiles(asbb)` |
| Disable | `Manager.DisableUnitFiles(asb)` |

### Service discovery

Only `*.service` units are surfaced; `.target`, `.socket`, `.timer`,
`.mount`, `.device` and other unit types are intentionally excluded. The
relatively static identity (name, description, unit object path) is cached,
while dynamic values (active/sub state, `MainPID`) are refreshed on the
application's normal 1-second tick. The manager never starts its own thread —
it integrates into the existing update loop.

### Service status

Each service shows its runtime state (derived from `ActiveState`) and its
boot-time enablement (derived from the unit-file state):

```text
Service                    Status       Enabled       Description
NetworkManager.service     active       enabled       Network Manager
bluetooth.service          inactive     disabled      Bluetooth service
systemd-journald.service   active       static        Journal Service
```

A service can validly be `active` while `disabled`, or `inactive` while
`enabled` — runtime state and boot-time enablement are tracked separately, not
conflated. Failed services are highlighted `FAILED` in the status column.

### Start / stop / restart / enable / disable

From the `u` detail screen you can start, stop, restart, enable or disable a
service by name. **Every operation requires an explicit confirmation
prompt** — nothing is changed silently. Failures (including permission
denials and unknown unit names) are reported cleanly without crashing.

### Permission handling

Systemd operations may require elevated privileges. The app does **not**
collect or store passwords, and it does not run anything through `sudo`. It
relies on systemd's normal authorization mechanism (polkit/D-Bus): when the
calling user is not permitted to perform an operation, the D-Bus call fails
with an access-denied error that is reported as `Permission denied.`

### Read-only failure mode

If the D-Bus connection to systemd cannot be established (e.g. the system does
not use systemd), the section shows:

```text
Unable to connect to systemd.
Service management unavailable.
```

and all other monitors (CPU, memory, processes, disk, network, GPU, sensors)
continue to function normally.

### Limitations

- Only `.service` units are listed; other unit types are deferred.
- Journal/log viewing is not implemented and is a future feature.
- No automatic startup-application or package-installation management.
- `MainPID` and some unit values are only shown when systemd reports them
  reliably (a value of `0` means systemd reports no main process).

## Startup Applications

Startup application management is a dedicated `StartupManager` module that
implements the **standard XDG autostart mechanism** for graphical desktop
applications. It is deliberately separate from the systemd service manager
(which handles `.service` units) and from other startup mechanisms it does
**not** manage: `/etc/rc.local`, cron, shell profiles (`.bashrc`, `.zshrc`),
bootloader configuration, or kernel parameters.

### XDG autostart

The manager scans the standard autostart directories:

- System: `/etc/xdg/autostart` plus one `autostart` directory per
  `$XDG_CONFIG_DIRS` entry (defaulting to `/etc/xdg`).
- User: `~/.config/autostart`, or `$XDG_CONFIG_HOME/autostart` when that
  variable is set to an absolute path.

Directories that do not exist are simply skipped. Only `*.desktop` files are
considered; hidden/temporary/backup files (names starting with `.`, `#` or
containing `~`) and non-regular files (including broken symlinks) are ignored.
The home directory is read from `$HOME` (never hard-coded), so the tool works
for any user.

### `[Desktop Entry]` parsing

A small, dependency-free parser reads the `[Desktop Entry]` group of each
file. Localized keys (e.g. `Name[de]`) are folded into their base key (the plain
`Name=` wins; otherwise the first localized value is used). The supported keys:

| Key | Purpose |
|---|---|
| `Name` | display name (falls back to the file name when missing) |
| `Comment` | description |
| `Exec` | the command line — displayed only, **never executed** |
| `Icon` | icon name |
| `Hidden` | `true` hides the entry (see override behavior below) |
| `OnlyShowIn` / `NotShowIn` | desktop-environment restrictions |
| `X-GNOME-Autostart-enabled` | GNOME-specific enable switch (recognized without requiring GNOME) |

Entries with no `[Desktop Entry]` group or with a `Type=` other than
`Application` are ignored, as are entries whose `Name` and `Exec` are both
absent-and-unusable. A single malformed file never prevents the others from
loading.

### Enabled state

The effective enabled state is computed with the XDG rules:

- a missing or unparseable boolean field does **not** mean "disabled";
- `Hidden=true` disables the entry;
- `X-GNOME-Autostart-enabled=false` disables the entry;
- `OnlyShowIn`/`NotShowIn` are applied against `$XDG_CURRENT_DESKTOP` when a
  desktop environment is running (e.g. `Hyprland`, `GNOME`, `KDE`); when no
  desktop environment is set, those keys do not disable the entry.

### User vs system entries and overrides

Every entry is tagged with its scope (`User` or `System`). The standard XDG
precedence applies automatically: a user entry with the same file name as a
system entry **shadows** it, and the UI flags the result with
`Overrides System: Yes`.

Enabling or disabling never writes below `/etc/xdg`:

- **User entry** — enable/disable rewrites the user's own file in place,
  only adding or updating `Hidden=` and `X-GNOME-Autostart-enabled=`.
  Comments and all unrelated keys are preserved.
- **System entry** — enable/disable copies the system file into the user's
  autostart directory and sets the same two keys there, creating
  `~/.config/autostart` if needed. The system file is never touched. A copied
  override keeps the original `Exec`, `Name`, and other fields, so the
  application still launches exactly as before.

Both operations require an explicit confirmation prompt.

### Management screen

Press `a` (then Enter) to open the frozen management screen:

```text
[1] Enable Startup Application
[2] Disable Startup Application
[3] View Application Details
[4] Search/Filter Applications
[5] Sort Applications
[6] Refresh Application List
[0] Cancel
```

Search covers name, description, command and file name (case-insensitive) and
filters the already-loaded list. Sorting covers Name (default), Enabled state
and Scope. The `STARTUP APPLICATIONS` section in the live view shows the same
columns and reflects enable/disable overrides immediately. The directories are
scanned once and cached; `refresh()` rescans on demand (startup, `[6]`, and
after every management action), so the filesystem is not polled continuously.

### Security and permissions

`.desktop` files are treated as configuration data: the `Exec=` value is
displayed but never run (`system()`/`popen()`/`exec()` are not used for
startup entries), no passwords are collected, and writes are confined to the
user's autostart directory. Reading `/etc/xdg/autostart` normally needs no
privileges; if a file cannot be read or written, the app reports a clear
`Permission denied.`/error message and continues running.

### Desktop-environment limitations

The implementation follows the standard XDG autostart spec first, and only
recognizes `X-GNOME-Autostart-enabled` as a convenience field — GNOME is not a
dependency. It does not emulate DE-specific autostart features such as
`AutostartCondition=` (GSettings-backed conditions), KDE's `X-KDE-*` phases, or
`StartupNotify`. Startup **timing** (`X-GNOME-Autostart-Phase`,
`X-KDE-autostart-phase`) is not acted upon; those keys are ignored.

## System Information

A dedicated `SystemInfoProvider` module provides a high-level, mostly **static**
overview of the machine. It is intentionally separate from the continuously
sampled CPU, memory, GPU and disk monitors: those diff fast-changing counters
every second, while System Information answers the "what is this machine?"
question from **immutable** sources read once and cached.

Press `y` (then Enter) in the list view to open the full breakdown:

```text
SYSTEM INFORMATION
--------------------------------
Hostname:            archlinux
Operating System:    Arch Linux
Distribution:        arch
Kernel:              6.x.x-arch1-1
Kernel Release:      #1 SMP PREEMPT_DYNAMIC ...
Architecture:        x86_64
Uptime:              2 days, 5 hours, 32 minutes

CPU
--------------------------------
Model:               AMD Ryzen 7 5800X ...
Architecture:        x86_64
Logical CPUs:        16
Physical cores:      8

MEMORY
--------------------------------
RAM:                 15.5 GB
Swap:                8.0 GB

GPU
--------------------------------
GPU 0:               AMD Radeon RX 6600

HARDWARE
--------------------------------
Manufacturer:        ASUSTeK COMPUTER INC.
Model:               PRIME B550-A
Product Version:     Rev 1.xx
Motherboard:         ASUSTeK PRIME B550-A

FIRMWARE
--------------------------------
Vendor:              American Megatrends
Version:             3603
Date:                12/20/2023
```

The live list view shows a compact summary (`## SYSTEM INFORMATION` with
operating system, kernel, architecture and uptime) near the top; the `y` screen
always offers `[1] Refresh System Information`, which re-reads every static
source on demand. Nothing is rescanned continuously.

### Sources

| Value | Source |
|---|---|
| Hostname | `gethostname(2)` |
| Operating system / distribution | `/etc/os-release` (`PRETTY_NAME`/`NAME`, `ID`, `VERSION_ID`) |
| Kernel version / architecture | `uname(2)` (`release`, `version`, `machine`) |
| CPU model | `/proc/cpuinfo` (`model name`, first `processor` block) |
| Logical CPUs | `processor` entries in `/proc/cpuinfo` |
| Physical cores | `cpu cores` × distinct `physical id` in `/proc/cpuinfo` |
| Total RAM / swap | existing `readMemoryInfo()` (`/proc/meminfo`) — never parsed twice |
| Uptime | first field of `/proc/uptime` |
| GPU names | existing `GpuMonitor` snapshot (shared, no re-detection) |
| Manufacturer / model | `/sys/class/dmi/id/sys_vendor`, `product_name`, `product_version` |
| Motherboard | `/sys/class/dmi/id/board_vendor`, `board_name`, `board_version` |
| BIOS/UEFI | `/sys/class/dmi/id/bios_vendor`, `bios_version`, `bios_date` |

No shell commands are used for any of this (`uname`, `hostname`, `lsb_release`,
`hostnamectl`, `lscpu`, `free`, `dmidecode`, `neofetch` are never invoked;
`system()`/`popen()` are not used).

### OS/kernel identification

`/etc/os-release` is parsed as plain `KEY=value` configuration data (quoted
values are supported) and the application is **not** hard-coded to Arch Linux:
any distribution that reports itself via os-release — Debian, Fedora, NixOS,
… — is identified correctly. The kernel version and machine architecture come
straight from the `uname(2)` syscall. Machine strings are mapped to friendly
names (`x86_64`, `arm64`, …); anything unrecognized is shown verbatim.

### CPU topology

Logical CPUs are the `processor` lines of `/proc/cpuinfo`. Physical cores are
derived as cores-per-package (`cpu cores`) multiplied by the number of distinct
`physical id` packages — the standard reliable x86 relationship. When a
platform does not expose these topology fields the physical count is left
unknown and displayed as `N/A` (never guessed).

### Uptime

`/proc/uptime`'s first field is the whole number of seconds since boot. It is
formatted by a shared `formatUptime()` helper into "45 seconds", "12 minutes",
"4 hours, 25 minutes" or "2 days, 7 hours, 10 minutes", dropping unnecessary
precision and never overflowing the integer math.

### Hardware (DMI) availability

Manufacturer, model, motherboard and BIOS/UEFI strings come from the kernel's
DMI interface under `/sys/class/dmi/id/`. **These files may not exist on every
machine** — and even on machines that have DMI tables, some fields are readable
only by root (the kernel rides the `dmi=` restrictions / `%p` hashing), some
laptop/OEM firmware leaves fields blank, and virtual machines often provide
only generic strings. Every single field degrades to `N/A` when its source
file is missing, unreadable or empty; one unavailable field never prevents the
others from loading, and the feature never crashes because a DMI value is
absent.

### Security

This feature is strictly **read-only**: it never modifies `/sys`, `/proc`,
`/etc/os-release` or any DMI data, never changes the hostname, kernel or
firmware settings, and never executes external commands.

## License

MIT — see [LICENSE](LICENSE).