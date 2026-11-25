#pragma once

#include "domain/snapshot.hpp"

namespace realmem {

// Computes live rate metrics (per-process CPU %, P-core %, Disk MB/s;
// system disk MB/s & IOPS; swap-in/out rates) from two consecutive snapshots.
class DeltaCalculator {
 public:
  static void apply(const SystemSnapshot& prev, SystemSnapshot& curr);
};

}  // namespace realmem
