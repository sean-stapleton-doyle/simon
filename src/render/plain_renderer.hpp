#pragma once

#include "render/renderer.hpp"

namespace simon {

class PlainRenderer final : public IRenderer {
 public:
  std::string render(const QuerySummary& summary,
                     const SystemSnapshot& snap,
                     const Config& cfg,
                     const RenderContext& ctx) const override;
};

}  // namespace simon
