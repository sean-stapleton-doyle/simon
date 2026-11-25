#pragma once

#include "config.hpp"
#include "model.hpp"
#include "process.hpp"

#include <string>

namespace realmem {

// Serializes the query result and system memory breakdown to pretty-printed JSON.
std::string render_json(const QuerySummary& summary,
                        const SystemMemory& sys,
                        const Config& cfg);

}  // namespace realmem
