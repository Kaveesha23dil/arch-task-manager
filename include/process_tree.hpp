#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "process_monitor.hpp"

namespace atm {

/// A process and its recursively-defined children. Value semantics (no raw
/// owning pointers); the nesting depth is bounded by the process-tree depth
/// actually present on the system.
struct ProcessTreeNode {
  Process process;
  std::vector<ProcessTreeNode> children;
};

/// Summary counters computed while a tree is built.
struct ProcessTreeStats {
  std::size_t total = 0;       // number of processes the tree was built from
  std::size_t root_count = 0;  // roots: PPID 0 / missing / orphaned parents
  std::size_t max_depth = 0;   // longest root-to-leaf path (a root is depth 1)
};

/// A process hierarchy. Roots are processes whose parent is absent.
struct ProcessTree {
  std::vector<ProcessTreeNode> roots;
  ProcessTreeStats stats;
};

/**
 * Builds a process tree from one ProcessMonitor snapshot.
 *
 * Construction is O(n): every process is linked under the process named by
 * its PPID through a PID -> index hash map, so a parent is never found by
 * scanning the list. A process becomes a root when its PPID is 0, its parent
 * is missing from the snapshot, or its PPID equals its own PID (a
 * self-reference guard). The result is deterministic: roots and every child
 * list are sorted by PID ascending. A parent that died between scans never
 * crashes the build — the process simply surfaces as an orphan root.
 */
[[nodiscard]] ProcessTree
buildProcessTree(const std::vector<Process> &processes);

/// Renders "name (pid)" lines using box-drawing connectors (├──, └──, │) with
/// "│   " / "    " continuation prefixes. Returns "No processes found." for an
/// empty tree. Recursion is depth-capped so malformed input cannot recurse
/// forever.
[[nodiscard]] std::string renderProcessTree(const ProcessTree &tree);

}  // namespace atm