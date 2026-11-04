#pragma once

#include "globals.hpp"
#include <hyprland/src/render/Renderer.hpp>

namespace OverviewRender {

// Framebuffer coordinates, scoped to the workspace-owned camera plane only.
extern const CBox* canvasViewportClip;

// IHyprRenderer's scene helpers are protected in 0.57. These queue the same
// native elements into ctx's current pass for the overview's own scene.
void queueWindow(Render::CRenderContext& ctx, const PHLWINDOW& window, const PHLMONITOR& monitor, const Time::steady_tp& now);
void queueLayer(Render::CRenderContext& ctx, const PHLLS& layer, const PHLMONITOR& monitor, const Time::steady_tp& now, bool popups = false);
void queueBackground(Render::CRenderContext& ctx, const PHLMONITOR& monitor);

void flushPass(Render::CRenderContext& ctx, PHLMONITOR monitor);
void queueBlur(Render::CRenderContext& ctx, const CBox& box, int rounding, float roundingPower, float alpha, bool usePrecomputedBlur);
void renderBlur(Render::CRenderContext& ctx, PHLMONITOR monitor, const CBox& box, int rounding, float roundingPower, float alpha, bool usePrecomputedBlur);

}
