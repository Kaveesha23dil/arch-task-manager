# Arch Task Manager

A native Linux task manager / system monitor, built specifically for Arch
Linux. It reads system information **directly from Linux interfaces** such as
`/proc/stat` — no shelling out to `top`, `htop`, or other external tools.

> Stage: **Step 1** — project foundation with a single feature
> (CPU monitoring). Everything else on the roadmap is intentionally **not**
> implemented yet, but the code is structured so future modules can be added
> without rewriting the CPU module.

## Why is this being built?

To produce a lightweight, dependency-free system monitor written in modern
C++ that talks to the Linux kernel directly. Arch's philosophy is simplicity
and user control, so the goal is a tool with minimal bloat, readable code, and
no third-party runtime dependencies. It is developed incrementally: one
feature per milestone, hosted on GitHub.

## Current features

- [x] **CPU monitoring** — overall CPU utilization via `/proc/stat`,
      calculated from the difference between two successive samples.
- [ ] RAM monitoring
- [ ] Process manager
- [ ] Process tree
- [ ] Disk monitoring
- [ ] Network monitoring
- [ ] GPU monitoring
- [ ] Temperature monitoring
- [ ] Systemd service management
- [ ] Startup applications
- [ ] Arch Linux package/update information
- [ ] Process actions (terminate, kill, pause, resume)
- [ ] Historical graphs
- [ ] System alerts

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

Expected output (the value updates every second, in place):

```text
========================================
ARCH TASK MANAGER
=================

CPU Usage:  34.7%

Updating every 1 second...

========================================

CPU Usage:  35.1%
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
│   └── cpu_monitor.hpp        # CpuTimes, readCpuTimes(), CpuMonitor
├── src/
│   ├── main.cpp               # UI loop: banner + 1 s refresh
│   └── cpu_monitor.cpp        # /proc/stat reading + utilization math
└── build/                     # generated; never committed to git
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

## Planned future features

- **RAM monitoring** — `meminfo`-based usage
- **Process manager** — listing, sorting, and search over processes
- **Process tree** — hierarchical process view
- **Disk monitoring** — usage and throughput
- **Network monitoring** — per-interface traffic
- **GPU monitoring**
- **Temperature monitoring**
- **Systemd service management**
- **Startup applications**
- **Arch Linux package / update information**
- **Process actions** — terminate, kill, pause, resume
- **Historical graphs**
- **System alerts**

## License

MIT — see [LICENSE](LICENSE).