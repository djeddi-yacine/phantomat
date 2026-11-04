#pragma once

#include "globals.hpp"
#include <hyprland/src/render/Renderer.hpp>

namespace OverviewRender {

// Framebuffer coordinates, scoped to the workspace-owned camera plane only.
extern const CBox* canvasViewportClip;

void flushPass(Render::CRenderContext& ctx, PHLMONITOR monitor);
void queueBlur(Render::CRenderContext& ctx, const CBox& box, int rounding, float roundingPower, float alpha, bool usePrecomputedBlur);
void renderBlur(Render::CRenderContext& ctx, PHLMONITOR monitor, const CBox& box, int rounding, float roundingPower, float alpha, bool usePrecomputedBlur);

}
