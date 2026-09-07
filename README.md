# Arch Task Manager

A native Linux task manager / system monitor, built specifically for Arch
Linux. It reads system information **directly from Linux interfaces** such as
`/proc/stat`, `/proc/meminfo`, and `/proc/<pid>/` — no shelling out to `ps`,
`free`, `top`, `htop`, or other external tools.

> Stage: **Step 6** — CPU, RAM, swap, process monitoring, process actions, the
> process tree, and disk/storage monitoring. Everything else on the roadmap is
> intentionally **not** implemented yet, but the code is structured so future
> modules (network, GPU, temperature, etc.) can be added without rewriting the
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

### Planned

- [ ] Process tree — interactive expand/collapse (deferred to the GUI)
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
- OS interfaces: the `/proc` and `/sys` filesystems, `statvfs(2)`,
  `sysconf(3)`
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
│   └── disk_monitor.hpp        # DiskUsage, BlockDevice, DiskSnapshot, DiskMonitor
├── src/
│   ├── main.cpp                # UI loop: frame rendering + 1 s refresh + control flow
│   ├── cpu_monitor.cpp         # /proc/stat reading + utilization math
│   ├── memory_monitor.cpp      # /proc/meminfo reading + memory/swap math
│   ├── process_monitor.cpp     # /proc scanning + per-process parsing
│   ├── process_actions.cpp     # kill(2)/setpriority(2) wrappers + errno mapping
│   ├── process_tree.cpp        # PID/PPID tree build + box-drawing renderer
│   └── disk_monitor.cpp        # statvfs(2) usage + /proc/diskstats rates + /sys/block
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

## License

MIT — see [LICENSE](LICENSE).