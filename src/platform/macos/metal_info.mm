#include "platform/macos/metal_info.hpp"

#import <Metal/Metal.h>

namespace realmem {

MetalDeviceInfo query_metal_device_info() {
  MetalDeviceInfo info;
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil) {
      return info;
    }
    info.recommended_max_working_set = static_cast<std::uint64_t>(device.recommendedMaxWorkingSetSize);
    info.has_unified_memory = device.hasUnifiedMemory ? true : false;
    info.valid = true;
  }
  return info;
}

}  // namespace realmem
