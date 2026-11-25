#include "json_render.hpp"

#include <nlohmann/json.hpp>

namespace realmem {

namespace {

using json = nlohmann::json;

json row_to_json(const DisplayRow& row, const Config& cfg) {
  json obj = json::object();
  if (cfg.group) {
    obj["name"] = row.name;
    obj["process_count"] = row.proc_count;
  } else {
    obj["pid"] = row.pid;
    obj["name"] = row.name;
  }

  obj["footprint_bytes"] = row.footprint;
  obj["peak_bytes"] = row.peak;

  if (cfg.show_gpu) {
    if (row.gpu_footprint.has_value()) {
      obj["gpu_bytes"] = *row.gpu_footprint;
    } else {
      obj["gpu_bytes"] = nullptr;
    }
  }

  if (!cfg.group && !row.cmdline.empty()) {
    obj["cmdline"] = row.cmdline;
  }
  return obj;
}

json summary_to_json(const QuerySummary& summary, const Config& cfg) {
  json obj = {
      {"readable_processes", summary.total_readable},
      {"total_processes", summary.total_pids},
      {"total_footprint_bytes", summary.total_footprint},
  };

  if (cfg.group) {
    obj["total_groups"] = summary.total_groups;
  }
  if (cfg.show_gpu) {
    obj["gpu_readable_processes"] = summary.gpu_readable_procs;
  }
  if (cfg.pattern) {
    obj["pattern"] = *cfg.pattern;
    obj["matched_processes"] = summary.matched_procs;
    obj["matched_footprint_bytes"] = summary.matched_footprint;
    if (summary.matched_procs > 0) {
      obj["largest_pid"] = summary.largest_pid;
      obj["largest_footprint_bytes"] = summary.largest_footprint;
    }
  }
  return obj;
}

json system_to_json(const SystemMemory& sys) {
  json obj = {
      {"total_bytes", sys.total},
      {"wired_bytes", sys.wired},
      {"anonymous_bytes", sys.anonymous},
      {"compressor_bytes", sys.compressor},
      {"file_cache_bytes", sys.file_cache},
      {"free_bytes", sys.free_bytes},
  };
  if (sys.gpu.valid) {
    obj["gpu"] = {
        {"allocated_bytes", sys.gpu.allocated},
        {"in_use_bytes", sys.gpu.in_use},
    };
  }
  return obj;
}

}  // namespace

std::string render_json(const QuerySummary& summary,
                        const SystemMemory& sys,
                        const Config& cfg) {
  json root = {
      {"mode", cfg.group ? "grouped" : "processes"},
      {"summary", summary_to_json(summary, cfg)},
  };

  if (!cfg.sum_only) {
    json rows = json::array();
    for (const auto& row : summary.rows) {
      rows.push_back(row_to_json(row, cfg));
    }
    root["rows"] = std::move(rows);

    if (cfg.show_system && sys.valid) {
      root["system"] = system_to_json(sys);
    }
  }

  return root.dump(2) + "\n";
}

}  // namespace realmem
