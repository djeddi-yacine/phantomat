#include "OverviewRender.hpp"
#include <chrono>
#include <cmath>
#include <sstream>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>

namespace OverviewRender {

    const CBox* canvasViewportClip = nullptr;

    namespace {
        struct SRendererAccess : Render::IHyprRenderer {
            using IHyprRenderer::renderBackground;
            using IHyprRenderer::renderLayer;
            using IHyprRenderer::renderWindow;
        };
    }

    void queueWindow(Render::CRenderContext& ctx, const PHLWINDOW& window, const PHLMONITOR& monitor, const Time::steady_tp& now) {
        (g_pHyprRenderer.get()->*(&SRendererAccess::renderWindow))(ctx, window, monitor, window->presentation().renderPresentation(), now, false, Render::RENDER_PASS_ALL, false,
                                                                   false);
    }

    void queueLayer(Render::CRenderContext& ctx, const PHLLS& layer, const PHLMONITOR& monitor, const Time::steady_tp& now, bool popups) {
        (g_pHyprRenderer.get()->*(&SRendererAccess::renderLayer))(ctx, layer, monitor, now, popups, false, false);
    }

    void queueBackground(Render::CRenderContext& ctx, const PHLMONITOR& monitor) {
        (g_pHyprRenderer.get()->*(&SRendererAccess::renderBackground))(ctx, monitor);
    }

    void flushPass(Render::CRenderContext& ctx, PHLMONITOR monitor) {
        if (!monitor || Render::IHyprRenderer::currentPass(ctx).empty())
            return;

        auto damage = CRegion{CBox{{}, monitor->m_transformedSize}};
        if (canvasViewportClip)
            damage.intersect(*canvasViewportClip);
        const auto previousDamage      = ctx.m_data.damage.copy();
        const auto previousFinalDamage = ctx.m_data.finalDamage.copy();
        const auto renderedDamage      = Render::IHyprRenderer::currentPass(ctx).render(ctx, damage);
        Render::IHyprRenderer::currentPass(ctx).clear();
        if (canvasViewportClip) {
            // Clipping this plane must not shrink native damage already rendered
            // outside it, especially when the incoming workspace is empty.
            ctx.m_data.damage      = previousDamage;
            ctx.m_data.finalDamage = previousFinalDamage.copy().add(renderedDamage);
        }
    }

    void queueBlur(Render::CRenderContext& ctx, const CBox& box, int rounding, float roundingPower, float alpha, bool usePrecomputedBlur) {
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
        Render::IHyprRenderer::addPassElement(ctx, makeUnique<CRectPassElement>(data));
    }

    void renderBlur(Render::CRenderContext& ctx, PHLMONITOR monitor, const CBox& box, int rounding, float roundingPower, float alpha, bool usePrecomputedBlur) {
        if (!monitor)
            return;

        queueBlur(ctx, box, rounding, roundingPower, alpha, usePrecomputedBlur);
        flushPass(ctx, monitor);
    }

}
