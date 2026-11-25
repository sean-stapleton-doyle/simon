#pragma once

#include "render/renderer.hpp"

namespace realmem {

class JsonRenderer final : public IRenderer {
 public:
  std::string render(const QuerySummary& summary,
                     const SystemSnapshot& snap,
                     const Config& cfg,
                     const RenderContext& ctx) const override;
};

}  // namespace realmem
