#pragma once

#include "platform/samplers.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace simon {

struct PerfLevelTopology {
  std::string name;
  std::uint32_t core_count = 0;
};

class MacCpuSampler final : public ICpuSampler {
 public:
  MacCpuSampler();

  CpuInfo sample() override;

 private:
  struct CoreTicks {
    std::uint64_t user = 0;
    std::uint64_t system = 0;
    std::uint64_t idle = 0;
    std::uint64_t nice = 0;
  };

  std::vector<CoreTicks> read_core_ticks() const;
  void compute_tier_utils(const std::vector<double>& core_utils, CpuInfo& out) const;

  std::string brand_;
  std::vector<PerfLevelTopology> topology_;
  std::vector<CoreTicks> prev_ticks_;
};

}  // namespace simon
