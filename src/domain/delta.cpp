#include "domain/delta.hpp"

#include <chrono>
#include <unordered_map>

namespace realmem {

namespace {

double elapsed_seconds(const SystemSnapshot& prev, const SystemSnapshot& curr) {
  const auto ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(curr.timestamp - prev.timestamp).count();
  return ns > 0 ? static_cast<double>(ns) / 1e9 : 0.0;
}

void apply_process_deltas(const ProcessSnapshot& prev, ProcessSnapshot& curr, double dt_sec) {
  std::unordered_map<pid_t, const ProcessRecord*> prev_by_pid;
  prev_by_pid.reserve(prev.processes.size());
  for (const auto& p : prev.processes) {
    prev_by_pid.emplace(p.pid, &p);
  }

  const double dt_ns = dt_sec * 1e9;
  for (auto& proc : curr.processes) {
    auto it = prev_by_pid.find(proc.pid);
    if (it == prev_by_pid.end()) {
      continue;
    }
    const ProcessRecord& old = *it->second;

    if (proc.cpu_time_ns >= old.cpu_time_ns) {
      proc.cpu_pct =
          (static_cast<double>(proc.cpu_time_ns - old.cpu_time_ns) / dt_ns) * 100.0;
    }
    if (proc.p_cpu_time_ns >= old.p_cpu_time_ns) {
      proc.p_cpu_pct =
          (static_cast<double>(proc.p_cpu_time_ns - old.p_cpu_time_ns) / dt_ns) * 100.0;
    }
    if (proc.disk_read_bytes >= old.disk_read_bytes) {
      proc.disk_read_bps =
          static_cast<double>(proc.disk_read_bytes - old.disk_read_bytes) / dt_sec;
    }
    if (proc.disk_write_bytes >= old.disk_write_bytes) {
      proc.disk_write_bps =
          static_cast<double>(proc.disk_write_bytes - old.disk_write_bytes) / dt_sec;
    }
  }
}

void apply_disk_deltas(const DiskInfo& prev, DiskInfo& curr, double dt_sec) {
  if (!prev.valid || !curr.valid) {
    return;
  }
  if (curr.total_read_bytes >= prev.total_read_bytes) {
    curr.read_bps = static_cast<double>(curr.total_read_bytes - prev.total_read_bytes) / dt_sec;
  }
  if (curr.total_write_bytes >= prev.total_write_bytes) {
    curr.write_bps = static_cast<double>(curr.total_write_bytes - prev.total_write_bytes) / dt_sec;
  }
  if (curr.total_read_ops >= prev.total_read_ops) {
    curr.read_iops = static_cast<double>(curr.total_read_ops - prev.total_read_ops) / dt_sec;
  }
  if (curr.total_write_ops >= prev.total_write_ops) {
    curr.write_iops = static_cast<double>(curr.total_write_ops - prev.total_write_ops) / dt_sec;
  }
}

void apply_memory_deltas(const SystemMemory& prev, SystemMemory& curr, double dt_sec) {
  if (!prev.valid || !curr.valid) {
    return;
  }
  // swapins/swapouts are tracked in bytes on SystemMemory.
  if (curr.swapins >= prev.swapins) {
    curr.swapin_rate_bps = static_cast<double>(curr.swapins - prev.swapins) / dt_sec;
  }
  if (curr.swapouts >= prev.swapouts) {
    curr.swapout_rate_bps = static_cast<double>(curr.swapouts - prev.swapouts) / dt_sec;
  }
}

}  // namespace

void DeltaCalculator::apply(const SystemSnapshot& prev, SystemSnapshot& curr) {
  const double dt_sec = elapsed_seconds(prev, curr);
  if (dt_sec <= 0.0) {
    return;
  }
  apply_process_deltas(prev.procs, curr.procs, dt_sec);
  apply_disk_deltas(prev.disk, curr.disk, dt_sec);
  apply_memory_deltas(prev.memory, curr.memory, dt_sec);
}

}  // namespace realmem
