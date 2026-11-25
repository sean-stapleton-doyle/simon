#pragma once

#include "platform/macos/metal_info.hpp"
#include "platform/samplers.hpp"

#include <cstdint>
#include <string>

namespace simon {

// Combines graphics ledger counters into a non-negative byte count.
std::uint64_t combine_graphics_ledger(std::int64_t footprint, std::int64_t compressed);

class MacGpuSampler final : public IGpuSampler {
 public:
  MacGpuSampler();

  GpuInfo sample_system() override;
  std::optional<std::uint64_t> sample_process(pid_t pid) override;

 private:
  MetalDeviceInfo metal_;
  std::string cached_model_;
  std::uint32_t cached_cores_ = 0;
};

}  // namespace simon
