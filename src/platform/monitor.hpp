#pragma once

#include "config/config.hpp"
#include "domain/snapshot.hpp"
#include "platform/samplers.hpp"

#include <memory>
#include <optional>

namespace realmem {

// Builds a SampleRequest from CLI/TOML Config (only requesting expensive fields when needed).
SampleRequest build_sample_request(const Config& cfg, std::optional<pid_t> self_pid = std::nullopt);

// Coordinates platform samplers, delta calculations, and optional IOReport enrichment.
class SystemMonitor {
 public:
  SystemMonitor(std::unique_ptr<IProcessSampler> proc_sampler,
                std::unique_ptr<IMemorySampler> mem_sampler,
                std::unique_ptr<IGpuSampler> gpu_sampler,
                std::unique_ptr<ICpuSampler> cpu_sampler = nullptr,
                std::unique_ptr<IDiskSampler> disk_sampler = nullptr,
                std::unique_ptr<ITelemetrySampler> telemetry_sampler = nullptr);

  // Takes a single snapshot and automatically applies deltas if a previous snapshot exists.
  SystemSnapshot sample(const SampleRequest& req);

  // For one-shot CLI runs that need rate metrics (--cpu/--disk), takes a baseline sample,
  // waits `sample_ms`, and returns the delta-enriched snapshot.
  SystemSnapshot sample_with_rates(const SampleRequest& req, int sample_ms);

 private:
  SystemSnapshot sample_raw(const SampleRequest& req);

  std::unique_ptr<IProcessSampler> proc_sampler_;
  std::unique_ptr<IMemorySampler> mem_sampler_;
  std::unique_ptr<IGpuSampler> gpu_sampler_;
  std::unique_ptr<ICpuSampler> cpu_sampler_;
  std::unique_ptr<IDiskSampler> disk_sampler_;
  std::unique_ptr<ITelemetrySampler> telemetry_sampler_;
  std::optional<SystemSnapshot> previous_;
};

// Creates a SystemMonitor wired with native macOS samplers.
SystemMonitor create_macos_monitor();

}  // namespace realmem
