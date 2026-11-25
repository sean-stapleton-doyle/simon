#pragma once

#include "config/config.hpp"
#include "domain/query.hpp"
#include "domain/snapshot.hpp"

#include <vector>

namespace realmem {

// Builds a parent/child process forest with bottom-up rolled-up subtree footprints,
// sorts siblings at every level, and flattens into indented DisplayRows.
class ProcessTreeBuilder {
 public:
  static std::vector<DisplayRow> build(const std::vector<const ProcessRecord*>& procs,
                                       SortField sort,
                                       bool reverse);
};

}  // namespace realmem
