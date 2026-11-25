#pragma once

#include "platform/samplers.hpp"

namespace realmem {

class MacDiskSampler final : public IDiskSampler {
 public:
  DiskInfo sample() override;
};

}  // namespace realmem
