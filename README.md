# Arch Task Manager

A native Linux task manager / system monitor, built specifically for Arch
Linux. It reads system information **directly from Linux interfaces** such as
`/proc/stat` and `/proc/meminfo` — no shelling out to `free`, `top`, `htop`,
or other external tools.

> Stage: **Step 2** — CPU, RAM, and swap monitoring. Everything else on the
> roadmap is intentionally **not** implemented yet, but the code is structured
> so future modules can be added without rewriting the existing ones.

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

### Planned

- [ ] Process manager
- [ ] Process tree
- [ ] Process actions
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
- OS interfaces: the `/proc` filesystem
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

## CPU

Usage:              34.7%

## MEMORY

Total:              15.5 GB
Used:               8.2 GB
Available:          7.3 GB
Free:               4.1 GB
Cached:             3.2 GB
Buffers:            512 MB
Usage:              52.9%

## SWAP

Total:              8.0 GB
Used:               1.2 GB
Free:               6.8 GB
Usage:              15.0%

---

# Updating every 1 second...
```

Press `Ctrl+C` to stop.

## Project structure

```text
arch-task-manager/
├── CMakeLists.txt
├── README.md
├── .gitignore
├── LICENSE
├── include/
│   ├── cpu_monitor.hpp         # CpuTimes, readCpuTimes(), CpuMonitor
│   └── memory_monitor.hpp      # MemoryInfo, readMemoryInfo(), MemoryMonitor
├── src/
│   ├── main.cpp                # UI loop: banner + 1 s refresh
│   ├── cpu_monitor.cpp         # /proc/stat reading + utilization math
│   └── memory_monitor.cpp      # /proc/meminfo reading + memory/swap math
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

`iowait` counts time the CPU spent idle while waiting on I/O, so it is
treated as idle. The application records one baseline sample on startup, then
reads a new sample every second and prints the percentage computed from the
difference, never the raw counters.

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