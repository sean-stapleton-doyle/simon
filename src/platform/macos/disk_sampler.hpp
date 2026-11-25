#pragma once

#include "platform/samplers.hpp"

namespace simon {

class MacDiskSampler final : public IDiskSampler {
 public:
  DiskInfo sample() override;
};

}  // namespace simon
