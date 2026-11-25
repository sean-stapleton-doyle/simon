#pragma once

#include "render/renderer.hpp"

#include <ftxui/dom/elements.hpp>

namespace realmem {

// Reusable FTXUI widgets shared between one-shot TableRenderer and live TuiApp.
ftxui::Element build_memory_stacked_bar(const SystemMemory& sys, int bar_width);
ftxui::Element build_system_summary_element(const SystemSnapshot& snap,
                                            const Config& cfg,
                                            int width);

class TableRenderer final : public IRenderer {
 public:
  std::string render(const QuerySummary& summary,
                     const SystemSnapshot& snap,
                     const Config& cfg,
                     const RenderContext& ctx) const override;
};

}  // namespace realmem
