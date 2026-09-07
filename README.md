# Arch Task Manager

A native Linux task manager / system monitor, built specifically for Arch
Linux. It reads system information **directly from Linux interfaces** such as
`/proc/stat`, `/proc/meminfo`, and `/proc/<pid>/` — no shelling out to `ps`,
`free`, `top`, `htop`, or other external tools.

> Stage: **Step 4** — CPU, RAM, swap, process monitoring, and process actions.
> Everything else on the roadmap is intentionally **not** implemented yet, but
> the code is structured so future modules (process tree, disk, etc.) can be
> added without rewriting the existing ones.

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

### Planned

- [ ] Process tree
- [ ] Disk monitoring
- [ ] Network monitoring
- [ ] GPU monitoring
- [ ] Temperature monitoring
- [ ] Systemd service management
- [ ] Startup applications
- [ ] Arch Linux package/update information
- [ ] Historical graphs
- [ ] System alerts
- [ ] GUI

## Technology

- Language: **C++20** (ISO standard, no GNU extensions)
- Build system: **CMake** (works for both Debug and Release)
- Compiler: **g++** (`GCC`)
- OS interfaces: the `/proc` filesystem, `sysconf(3)`
- Standard library only (`std::thread`, `std::chrono`, `<fstream>`, …)
- No third-party dependencies

## Build on Arch Linux

Requirements:

```bash
sudo pacman -S base-devel cmake
```

`base-devel` provides `g++`; `cmake` provides the build tooling.

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

## MEMORY

Total:           15.5 GB
...

## SWAP

Total:           8.0 GB
...

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
Manage: press 'm' (then Enter) to control a process by PID
Updating every 1 second...
```

Press `Ctrl+C` to stop.

### Sorting

The table is sorted by **CPU usage (descending)** by default. While the
application runs, press a digit **and then Enter** to change the sort order on
the next refresh:

- `1` — CPU usage (descending)
- `2` — Memory usage (descending)
- `3` — PID (ascending)
- `4` — Process name (ascending, case-insensitive)

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
│   └── process_actions.hpp     # ProcessActions, ActionResult, ActionStatus
├── src/
│   ├── main.cpp                # UI loop: frame rendering + 1 s refresh + control flow
│   ├── cpu_monitor.cpp         # /proc/stat reading + utilization math
│   ├── memory_monitor.cpp      # /proc/meminfo reading + memory/swap math
│   ├── process_monitor.cpp     # /proc scanning + per-process parsing
│   └── process_actions.cpp     # kill(2)/setpriority(2) wrappers + errno mapping
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

## License

MIT — see [LICENSE](LICENSE).