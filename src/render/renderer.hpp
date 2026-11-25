#pragma once

#include "config/config.hpp"
#include "domain/query.hpp"
#include "domain/snapshot.hpp"

#include <memory>
#include <string>

namespace simon {

struct RenderContext {
  int width = 120;
  bool is_tty = false;
};

class IRenderer {
 public:
  virtual ~IRenderer() = default;
  virtual std::string render(const QuerySummary& summary,
                             const SystemSnapshot& snap,
                             const Config& cfg,
                             const RenderContext& ctx) const = 0;
};

// Factory returning the appropriate renderer for `cfg` and `ctx`.
std::unique_ptr<IRenderer> make_renderer(const Config& cfg, const RenderContext& ctx);

// Convenience helper for one-shot rendering.
std::string render_output(const QuerySummary& summary,
                          const SystemSnapshot& snap,
                          const Config& cfg,
                          const RenderContext& ctx);

}  // namespace simon
