#include "platform/monitor.hpp"

#include "domain/delta.hpp"
#include "platform/macos/cpu_sampler.hpp"
#include "platform/macos/disk_sampler.hpp"
#include "platform/macos/gpu_sampler.hpp"
#include "platform/macos/ioreport_sampler.hpp"
#include "platform/macos/memory_sampler.hpp"
#include "platform/macos/process_sampler.hpp"

#include <chrono>
#include <thread>

namespace simon {

SampleRequest build_sample_request(const Config& cfg, std::optional<pid_t> self_pid) {
  SampleRequest req;
  req.need_processes = !cfg.short_mode;
  req.need_cmdline = req.need_processes &&
                     (cfg.pattern.has_value() || (cfg.long_format && !cfg.group) ||
                      (cfg.group && !cfg.group_rules.empty()));
  req.need_path = req.need_processes && cfg.group;
  req.need_ppid = req.need_processes && (cfg.tree || cfg.watch);
  req.need_process_gpu = req.need_processes && (cfg.show_gpu || cfg.sort == SortField::Gpu);
  req.need_system_memory = cfg.short_mode || cfg.watch || (!cfg.sum_only && cfg.show_system);
  req.need_cpu = cfg.watch || cfg.show_cpu || cfg.sort == SortField::Cpu;
  req.need_disk = cfg.watch || cfg.show_disk || cfg.sort == SortField::Disk;
  req.need_telemetry = req.need_cpu || cfg.watch;
  if (req.need_processes && cfg.pattern && self_pid) {
    req.exclude_pid = *self_pid;
  }
  return req;
}

SystemMonitor::SystemMonitor(std::unique_ptr<IProcessSampler> proc_sampler,
                             std::unique_ptr<IMemorySampler> mem_sampler,
                             std::unique_ptr<IGpuSampler> gpu_sampler,
                             std::unique_ptr<ICpuSampler> cpu_sampler,
                             std::unique_ptr<IDiskSampler> disk_sampler,
                             std::unique_ptr<ITelemetrySampler> telemetry_sampler)
    : proc_sampler_(std::move(proc_sampler)),
      mem_sampler_(std::move(mem_sampler)),
      gpu_sampler_(std::move(gpu_sampler)),
      cpu_sampler_(std::move(cpu_sampler)),
      disk_sampler_(std::move(disk_sampler)),
      telemetry_sampler_(std::move(telemetry_sampler)) {}

SystemSnapshot SystemMonitor::sample_raw(const SampleRequest& req) {
  SystemSnapshot snap;
  snap.timestamp = std::chrono::steady_clock::now();

  if (req.need_processes && proc_sampler_) {
    snap.procs = proc_sampler_->sample(req);
  }
  if (req.need_system_memory && mem_sampler_) {
    snap.memory = mem_sampler_->sample();
    if (gpu_sampler_) {
      snap.memory.gpu = gpu_sampler_->sample_system();
    }
  }
  if (req.need_cpu && cpu_sampler_) {
    snap.cpu = cpu_sampler_->sample();
  }
  if (req.need_disk && disk_sampler_) {
    snap.disk = disk_sampler_->sample();
  }
  if (req.need_telemetry && telemetry_sampler_) {
    telemetry_sampler_->sample_into(snap.cpu, snap.memory.gpu, snap.telemetry);
  }
  return snap;
}

SystemSnapshot SystemMonitor::sample(const SampleRequest& req) {
  SystemSnapshot curr = sample_raw(req);
  if (previous_) {
    DeltaCalculator::apply(*previous_, curr);
  }
  previous_ = curr;
  return curr;
}

SystemSnapshot SystemMonitor::sample_with_rates(const SampleRequest& req, int sample_ms) {
  if (!previous_) {
    (void)sample(req);
  }
  if (sample_ms > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(sample_ms));
  }
  return sample(req);
}

SystemMonitor create_macos_monitor() {
  auto gpu = std::make_unique<MacGpuSampler>();
  auto proc = std::make_unique<MacProcessSampler>(gpu.get());
  auto mem = std::make_unique<MacMemorySampler>();
  auto cpu = std::make_unique<MacCpuSampler>();
  auto disk = std::make_unique<MacDiskSampler>();
  auto telemetry = std::make_unique<IoReportSampler>();
  return SystemMonitor(std::move(proc),
                       std::move(mem),
                       std::move(gpu),
                       std::move(cpu),
                       std::move(disk),
                       std::move(telemetry));
}

}  // namespace simon
