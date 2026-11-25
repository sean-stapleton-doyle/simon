#pragma once

#include "platform/samplers.hpp"

namespace realmem {

// Maps kern.memorystatus_vm_pressure_level integer (1, 2, 4) to enum.
MemoryPressureLevel parse_pressure_level(int raw);

class MacMemorySampler final : public IMemorySampler {
 public:
  SystemMemory sample() override;
};

}  // namespace realmem
