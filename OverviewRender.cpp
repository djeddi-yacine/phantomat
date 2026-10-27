#include "OverviewRender.hpp"
#include <chrono>
#include <cmath>
#include <sstream>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>

namespace OverviewRender {

const CBox* canvasViewportClip = nullptr;

void flushPass(PHLMONITOR monitor) {
    if (!monitor || g_pHyprRenderer->m_renderPass.empty())
        return;

    auto damage = CRegion{CBox{{}, monitor->m_transformedSize}};
    if (canvasViewportClip)
        damage.intersect(*canvasViewportClip);
    const auto previousDamage = g_pHyprRenderer->m_renderData.damage.copy();
    const auto previousFinalDamage = g_pHyprRenderer->m_renderData.finalDamage.copy();
    const auto renderedDamage = g_pHyprRenderer->m_renderPass.render(damage);
    g_pHyprRenderer->m_renderPass.clear();
    if (canvasViewportClip) {
        // Clipping this plane must not shrink native damage already rendered
        // outside it, especially when the incoming workspace is empty.
        g_pHyprRenderer->m_renderData.damage = previousDamage;
        g_pHyprRenderer->m_renderData.finalDamage = previousFinalDamage.copy().add(renderedDamage);
    }
}

void queueBlur(const CBox& box, int rounding, float roundingPower, float alpha, bool usePrecomputedBlur) {
    if (alpha <= 0.F)
        return;

    if (box.empty())
        return;

    CRectPassElement::SRectData data;
    data.box           = box;
    data.color         = CHyprColor{0.F, 0.F, 0.F, 0.F};
    data.round         = rounding;
    data.roundingPower = roundingPower;
    data.blur          = true;
    data.blurA         = alpha;
    data.xray          = usePrecomputedBlur;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(data));
}

void renderBlur(PHLMONITOR monitor, const CBox& box, int rounding, float roundingPower, float alpha, bool usePrecomputedBlur) {
    if (!monitor)
        return;

    queueBlur(box, rounding, roundingPower, alpha, usePrecomputedBlur);
    flushPass(monitor);
}

}
