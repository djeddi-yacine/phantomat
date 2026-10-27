#pragma once

#include "globals.hpp"

namespace OverviewRender {

// Framebuffer coordinates, scoped to the workspace-owned camera plane only.
extern const CBox* canvasViewportClip;

void flushPass(PHLMONITOR monitor);
void queueBlur(const CBox& box, int rounding, float roundingPower, float alpha, bool usePrecomputedBlur);
void renderBlur(PHLMONITOR monitor, const CBox& box, int rounding, float roundingPower, float alpha, bool usePrecomputedBlur);

}
