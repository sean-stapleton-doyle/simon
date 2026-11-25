#include "render/json_renderer.hpp"

#include "render/format_utils.hpp"

#include <nlohmann/json.hpp>

namespace realmem {

namespace {

using json = nlohmann::json;

std::string_view mode_name(const Config& cfg) {
  if (cfg.tree) return "tree";
  if (cfg.group) return "grouped";
  return "processes";
}

json row_to_json(const DisplayRow& row, const Config& cfg) {
  json obj = json::object();
  if (cfg.group) {
    obj["name"] = row.name;
    obj["process_count"] = row.proc_count;
  } else {
    obj["pid"] = row.pid;
    obj["name"] = row.name;
    if (cfg.tree) {
      obj["ppid"] = row.ppid;
      obj["depth"] = row.tree_depth;
      obj["subtree_processes"] = row.proc_count;
      obj["subtree_footprint_bytes"] = row.subtree_footprint;
    }
  }

  obj["footprint_bytes"] = row.footprint;
  obj["peak_bytes"] = row.peak;

  if (cfg.show_gpu) {
    obj["gpu_bytes"] = row.gpu_footprint ? json(*row.gpu_footprint) : json(nullptr);
  }
  if (cfg.show_cpu) {
    obj["cpu_pct"] = row.cpu_pct ? json(*row.cpu_pct) : json(nullptr);
    obj["p_cpu_pct"] = row.p_cpu_pct ? json(*row.p_cpu_pct) : json(nullptr);
  }
  if (cfg.show_disk) {
    obj["disk_read_bytes"] = row.disk_read_bytes;
    obj["disk_write_bytes"] = row.disk_write_bytes;
    if (row.disk_read_bps) obj["disk_read_bps"] = *row.disk_read_bps;
    if (row.disk_write_bps) obj["disk_write_bps"] = *row.disk_write_bps;
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
  if (cfg.group) obj["total_groups"] = summary.total_groups;
  if (cfg.show_gpu) obj["gpu_readable_processes"] = summary.gpu_readable_procs;
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

json gpu_to_json(const GpuInfo& gpu) {
  json obj = json::object();
  if (!gpu.model.empty()) obj["model"] = gpu.model;
  if (gpu.core_count > 0) obj["core_count"] = gpu.core_count;
  if (gpu.valid) {
    obj["allocated_bytes"] = gpu.allocated;
    obj["in_use_bytes"] = gpu.in_use;
  }
  if (const auto ceiling = gpu.effective_ceiling(); ceiling > 0) {
    obj["ceiling_bytes"] = ceiling;
    if (auto head = gpu.headroom_bytes()) obj["headroom_bytes"] = *head;
  }
  if (gpu.device_util_pct) obj["device_util_pct"] = *gpu.device_util_pct;
  if (gpu.renderer_util_pct) obj["renderer_util_pct"] = *gpu.renderer_util_pct;
  if (gpu.tiler_util_pct) obj["tiler_util_pct"] = *gpu.tiler_util_pct;
  if (gpu.freq_mhz) obj["freq_mhz"] = *gpu.freq_mhz;
  if (gpu.power_watts) obj["power_watts"] = *gpu.power_watts;
  return obj;
}

json system_to_json(const SystemSnapshot& snap, const Config& cfg) {
  const SystemMemory& sys = snap.memory;
  json obj = {
      {"total_bytes", sys.total},
      {"wired_bytes", sys.wired},
      {"anonymous_bytes", sys.anonymous},
      {"compressor_bytes", sys.compressor},
      {"file_cache_bytes", sys.file_cache},
      {"free_bytes", sys.free_bytes},
  };
  if (const double ratio = sys.compression_ratio(); ratio > 0.0) {
    obj["compression_ratio"] = ratio;
  }
  if (sys.swap.valid) {
    obj["swap"] = {
        {"total_bytes", sys.swap.total},
        {"used_bytes", sys.swap.used},
        {"free_bytes", sys.swap.free_bytes},
        {"encrypted", sys.swap.encrypted},
    };
  }
  if (sys.pressure != MemoryPressureLevel::Unknown) {
    obj["pressure"] = pressure_label(sys.pressure);
  }
  if (sys.memory_free_pct) {
    obj["memory_free_pct"] = *sys.memory_free_pct;
  }
  if (sys.gpu.valid || sys.gpu.effective_ceiling() > 0) {
    obj["gpu"] = gpu_to_json(sys.gpu);
  }
  if (cfg.show_cpu && snap.cpu.valid) {
    json tiers = json::array();
    for (const auto& t : snap.cpu.tiers) {
      json tj = {{"name", t.name}, {"cores", t.core_count}, {"util_pct", t.util_pct}};
      if (t.freq_mhz) tj["freq_mhz"] = *t.freq_mhz;
      tiers.push_back(std::move(tj));
    }
    json cpu_obj = {{"total_util_pct", snap.cpu.total_util_pct}, {"tiers", std::move(tiers)}};
    if (snap.cpu.power_watts) cpu_obj["power_watts"] = *snap.cpu.power_watts;
    if (snap.telemetry.dram_bandwidth_gbps) {
      cpu_obj["dram_bandwidth_gbps"] = *snap.telemetry.dram_bandwidth_gbps;
    }
    obj["cpu"] = std::move(cpu_obj);
  }
  if (cfg.show_disk && snap.disk.valid) {
    json disk_obj = {
        {"total_read_bytes", snap.disk.total_read_bytes},
        {"total_write_bytes", snap.disk.total_write_bytes},
    };
    if (snap.disk.read_bps) disk_obj["read_bps"] = *snap.disk.read_bps;
    if (snap.disk.write_bps) disk_obj["write_bps"] = *snap.disk.write_bps;
    obj["disk"] = std::move(disk_obj);
  }
  return obj;
}

}  // namespace

std::string JsonRenderer::render(const QuerySummary& summary,
                                 const SystemSnapshot& snap,
                                 const Config& cfg,
                                 const RenderContext&) const {
  json root = {
      {"mode", mode_name(cfg)},
      {"summary", summary_to_json(summary, cfg)},
  };

  if (!cfg.sum_only) {
    json rows = json::array();
    for (const auto& row : summary.rows) {
      rows.push_back(row_to_json(row, cfg));
    }
    root["rows"] = std::move(rows);

    if (cfg.show_system && snap.memory.valid) {
      root["system"] = system_to_json(snap, cfg);
    }
  }

  return root.dump(2) + "\n";
}

}  // namespace realmem
