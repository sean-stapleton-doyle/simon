#include "render/renderer.hpp"

#include "render/json_renderer.hpp"
#include "render/plain_renderer.hpp"
#include "render/table_renderer.hpp"

namespace simon {

namespace {

bool use_rich_output(const Config& cfg, const RenderContext& ctx) {
  if (cfg.plain) {
    return false;
  }
  switch (cfg.color) {
    case ColorMode::Always: return true;
    case ColorMode::Never: return false;
    case ColorMode::Auto: return ctx.is_tty;
  }
  return false;
}

}  // namespace

std::unique_ptr<IRenderer> make_renderer(const Config& cfg, const RenderContext& ctx) {
  switch (cfg.output) {
    case OutputFormat::Json:
      return std::make_unique<JsonRenderer>();
    case OutputFormat::Plain:
      return std::make_unique<PlainRenderer>();
    case OutputFormat::Table:
      return std::make_unique<TableRenderer>();
    case OutputFormat::Auto:
      return use_rich_output(cfg, ctx)
                 ? std::unique_ptr<IRenderer>(std::make_unique<TableRenderer>())
                 : std::unique_ptr<IRenderer>(std::make_unique<PlainRenderer>());
  }
  return std::make_unique<PlainRenderer>();
}

std::string render_output(const QuerySummary& summary,
                          const SystemSnapshot& snap,
                          const Config& cfg,
                          const RenderContext& ctx) {
  return make_renderer(cfg, ctx)->render(summary, snap, cfg, ctx);
}

}  // namespace simon
