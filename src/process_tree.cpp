#include "process_tree.hpp"

#include <algorithm>
#include <sstream>
#include <unordered_map>

namespace atm {

namespace {

// Build recursion is capped purely as insurance: with the kernel's data a
// process has exactly one PPID and the hierarchy is acyclic, so truncating at
// this depth only ever protects against malformed input.
constexpr std::size_t kMaxBuildDepth = 256;

// Display depth cap: keeps pathological data from recursing through the
// renderer, which would otherwise be the deepest possible stack consumer.
constexpr std::size_t kMaxRenderDepth = 128;

/// Depth of a subtree where `depth + 1` is the root's own depth.
std::size_t treeDepth(const ProcessTreeNode &node, std::size_t depth) {
  const std::size_t own = depth + 1;
  if (depth >= kMaxBuildDepth) {
    return own;
  }
  std::size_t best = own;
  for (const ProcessTreeNode &child : node.children) {
    best = std::max(best, treeDepth(child, depth + 1));
  }
  return best;
}

/**
 * Emits one node as a "name (pid)" line and recurses into its children.
 *
 * Roots are flush-left (no connector). Every other node gets a horizontal
 * connector — "├── " for a non-last sibling, "└── " for a last one — prefixed
 * by `prefix`, which carries the vertical "│   " / blank "    " expansion of
 * all ancestors. A root's children start at column 0 (see the doc comment in
 * renderProcessTree); deeper levels append "│   " when the parent has more
 * siblings, "    " when it is the last child.
 */
void renderNode(std::ostringstream &out, const ProcessTreeNode &node,
                const std::string &prefix, bool is_last, bool is_root,
                std::size_t depth) {
  if (!is_root) {
    out << prefix << (is_last ? "└── " : "├── ");
  }
  out << node.process.name << " (" << node.process.pid << ")\n";
  if (depth >= kMaxRenderDepth) {
    return;
  }

  const std::string child_prefix =
      is_root ? "" : (prefix + (is_last ? "    " : "│   "));
  const std::size_t count = node.children.size();
  for (std::size_t i = 0; i < count; ++i) {
    renderNode(out, node.children[i], child_prefix, i + 1 == count, false,
               depth + 1);
  }
}

}  // namespace

ProcessTree buildProcessTree(const std::vector<Process> &processes) {
  ProcessTree tree;
  tree.stats.total = processes.size();
  if (processes.empty()) {
    return tree;
  }

  // pid -> index into `processes`, for O(1) parent lookup.
  std::unordered_map<int, std::size_t> index;
  index.reserve(processes.size() * 2);
  for (std::size_t i = 0; i < processes.size(); ++i) {
    index.emplace(processes[i].pid, i);  // first entry wins on duplicate pids
  }

  // Link every linked process to its parent; collect the child lists. A node
  // with PPID 0, with a missing parent, or whose PPID is its own PID stays a
  // root (has_parent stays false).
  std::vector<bool> has_parent(processes.size(), false);
  std::vector<std::vector<std::size_t>> children(processes.size());
  for (std::size_t i = 0; i < processes.size(); ++i) {
    const int ppid = processes[i].parent_pid;
    if (ppid <= 0) {
      continue;  // kernel idle/scheduler or garbage PPID
    }
    const auto parent = index.find(ppid);
    if (parent == index.end()) {
      continue;  // parent not in this snapshot -> orphan root
    }
    if (parent->second == i) {
      continue;  // self-reference guard
    }
    has_parent[i] = true;
    children[parent->second].push_back(i);
  }

  // Deterministic ordering: children and roots ascending by PID (the task
  // manager never relies on /proc's unsorted directory order).
  const auto by_pid = [&processes](std::size_t a, std::size_t b) {
    return processes[a].pid < processes[b].pid;
  };
  for (std::vector<std::size_t> &list : children) {
    std::sort(list.begin(), list.end(), by_pid);
  }

  std::vector<std::size_t> roots;
  roots.reserve(processes.size());
  for (std::size_t i = 0; i < processes.size(); ++i) {
    if (!has_parent[i]) {
      roots.push_back(i);
    }
  }
  std::sort(roots.begin(), roots.end(), by_pid);

  // Materialize the value-semantics hierarchy. `visited` is a cycle breaker:
  // real /proc data is acyclic, but a malformed self/cycle reference can then
  // never recurse forever.
  std::vector<bool> visited(processes.size(), false);
  const auto make_node = [&](const auto &self, std::size_t i,
                             std::size_t depth) -> ProcessTreeNode {
    ProcessTreeNode node;
    node.process = processes[i];
    if (depth < kMaxBuildDepth && !visited[i]) {
      visited[i] = true;
      node.children.reserve(children[i].size());
      for (const std::size_t child : children[i]) {
        node.children.push_back(self(self, child, depth + 1));
      }
    }
    return node;
  };

  tree.roots.reserve(roots.size());
  std::size_t max_depth = 0;
  for (const std::size_t root : roots) {
    tree.roots.push_back(make_node(make_node, root, 0));
    max_depth = std::max(max_depth, treeDepth(tree.roots.back(), 0));
  }
  tree.stats.root_count = tree.roots.size();
  tree.stats.max_depth = max_depth;
  return tree;
}

std::string renderProcessTree(const ProcessTree &tree) {
  std::ostringstream out;
  if (tree.roots.empty()) {
    out << "No processes found.";
    return out.str();
  }
  // Roots are displayed flush-left; a root's child connectors therefore start
  // at column 0 ("├── child"), and only descendants of a root that is not the
  // last sibling gain the "│   " vertical prefix.
  for (const ProcessTreeNode &root : tree.roots) {
    renderNode(out, root, std::string(), false, true, 0);
  }
  return out.str();
}

}  // namespace atm