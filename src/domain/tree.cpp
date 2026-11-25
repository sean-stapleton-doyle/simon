#include "domain/tree.hpp"

#include <algorithm>
#include <unordered_map>

namespace simon {

namespace {

struct TreeNode {
  DisplayRow row;
  std::vector<std::size_t> children;
};

template <typename T>
void add_optional(std::optional<T>& target, const std::optional<T>& added) {
  if (added) {
    target = target.value_or(T{0}) + *added;
  }
}

DisplayRow row_from_record(const ProcessRecord& proc) {
  DisplayRow row;
  row.pid = proc.pid;
  row.ppid = proc.ppid;
  row.proc_count = 1;
  row.footprint = proc.footprint;
  row.subtree_footprint = proc.footprint;
  row.peak = proc.lifetime_max;
  row.gpu_footprint = proc.gpu_footprint;
  row.cpu_pct = proc.cpu_pct;
  row.p_cpu_pct = proc.p_cpu_pct;
  row.disk_read_bytes = proc.disk_read_bytes;
  row.disk_write_bytes = proc.disk_write_bytes;
  row.disk_read_bps = proc.disk_read_bps;
  row.disk_write_bps = proc.disk_write_bps;
  row.name = proc.name;
  row.cmdline = proc.cmdline;
  return row;
}

void rollup_subtree(std::size_t idx, std::vector<TreeNode>& nodes, std::vector<bool>& visited) {
  visited[idx] = true;
  TreeNode& node = nodes[idx];
  for (std::size_t child_idx : node.children) {
    if (visited[child_idx]) {
      continue;  // cycle safety
    }
    rollup_subtree(child_idx, nodes, visited);
    const DisplayRow& child = nodes[child_idx].row;
    node.row.proc_count += child.proc_count;
    node.row.subtree_footprint += child.subtree_footprint;
    node.row.peak += child.peak;
    add_optional(node.row.gpu_footprint, child.gpu_footprint);
    add_optional(node.row.cpu_pct, child.cpu_pct);
    add_optional(node.row.p_cpu_pct, child.p_cpu_pct);
    node.row.disk_read_bytes += child.disk_read_bytes;
    node.row.disk_write_bytes += child.disk_write_bytes;
    add_optional(node.row.disk_read_bps, child.disk_read_bps);
    add_optional(node.row.disk_write_bps, child.disk_write_bps);
  }
}

void sort_indices(std::vector<std::size_t>& indices,
                  const std::vector<TreeNode>& nodes,
                  SortField sort,
                  bool reverse) {
  std::sort(indices.begin(), indices.end(), [&](std::size_t a, std::size_t b) {
    return compare_display_rows(nodes[a].row, nodes[b].row, sort, reverse);
  });
}

void flatten_node(std::size_t idx,
                  int depth,
                  const std::string& ancestor_prefix,
                  bool is_last,
                  std::vector<TreeNode>& nodes,
                  SortField sort,
                  bool reverse,
                  std::vector<DisplayRow>& out) {
  TreeNode& node = nodes[idx];
  DisplayRow row = node.row;
  row.tree_depth = depth;
  if (depth > 0) {
    row.tree_prefix = ancestor_prefix + (is_last ? "\xe2\x94\x94\xe2\x94\x80 "  // └─
                                                 : "\xe2\x94\x9c\xe2\x94\x80 ");  // ├─
  }
  out.push_back(std::move(row));

  sort_indices(node.children, nodes, sort, reverse);
  const std::string child_prefix =
      (depth == 0) ? std::string{}
                   : (ancestor_prefix + (is_last ? "   " : "\xe2\x94\x82  "));  // │
  for (std::size_t i = 0; i < node.children.size(); ++i) {
    const bool child_is_last = (i + 1 == node.children.size());
    flatten_node(node.children[i], depth + 1, child_prefix, child_is_last, nodes, sort, reverse,
                 out);
  }
}

}  // namespace

std::vector<DisplayRow> ProcessTreeBuilder::build(const std::vector<const ProcessRecord*>& procs,
                                                  SortField sort,
                                                  bool reverse) {
  std::vector<TreeNode> nodes;
  nodes.reserve(procs.size());
  std::unordered_map<pid_t, std::size_t> pid_to_idx;
  pid_to_idx.reserve(procs.size());

  for (const ProcessRecord* proc : procs) {
    pid_to_idx.emplace(proc->pid, nodes.size());
    nodes.push_back(TreeNode{row_from_record(*proc), {}});
  }

  std::vector<std::size_t> roots;
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    const pid_t ppid = nodes[i].row.ppid;
    auto it = pid_to_idx.find(ppid);
    if (ppid > 1 && ppid != nodes[i].row.pid && it != pid_to_idx.end()) {
      nodes[it->second].children.push_back(i);
    } else {
      roots.push_back(i);
    }
  }

  std::vector<bool> visited(nodes.size(), false);
  for (std::size_t r : roots) {
    rollup_subtree(r, nodes, visited);
  }

  sort_indices(roots, nodes, sort, reverse);

  std::vector<DisplayRow> out;
  out.reserve(nodes.size());
  for (std::size_t i = 0; i < roots.size(); ++i) {
    flatten_node(roots[i], 0, "", i + 1 == roots.size(), nodes, sort, reverse, out);
  }
  return out;
}

}  // namespace simon
