#include "Window.hpp"
#include <algorithm>
#include <cmath>
#include <dlfcn.h>
#include <functional>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/keybinds/Manager.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Group.hpp>
#include <hyprland/src/desktop/view/Popup.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/plugins/PluginSystem.hpp>
#include <hyprland/src/render/pass/Pass.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/render/pass/RendererHintsPassElement.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/render/types.hpp>
#include <hyprland/src/render/decorations/CHyprGroupBarDecoration.hpp>
#include <hyprland/src/render/decorations/DecorationPositioner.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/protocols/XDGShell.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#include "Config.hpp"
#include "OverviewPassElement.hpp"
#include "OverviewRender.hpp"

// CRenderPass keeps its queued elements private. The overview captures one
// window's pass in its own CRenderPass and edits those surface elements before
// they join the frame pass.
struct SPassElementsTag {
    friend constexpr auto privateMember(SPassElementsTag);
};
template struct SPrivateMember<SPassElementsTag, &Render::CRenderPass::m_passElements>;

namespace OverviewWindow {
namespace {

struct SOverviewWindowMetrics {
    float renderScale             = 1.F;
    float pxScale                 = 1.F;
    float targetOpacity           = 1.F;
    float borderSize              = 0.F;
    int   borderPx                = 0;
    float borderPxScaled          = 0.F;
    float roundingBase            = 0.F;
    float roundingPower           = 2.F;
    float correctionOffset        = 0.F;
    float outerRound              = 0.F;
    int   roundingPx              = 0;
    int   outerRoundPx            = 0;
};

// SurfacePassElement only scales subsurface textures while the window size is
// marked as animated. Overview temporarily changes the current window size
// without starting a real animation, so expose that single bit for the duration
// of the synchronous overview render and restore it immediately afterwards.
struct COverviewAnimatedVariableAccess : Hyprutils::Animation::CBaseAnimatedVariable {
    static void setBeingAnimated(Hyprutils::Animation::CBaseAnimatedVariable* variable, bool animated) {
        if (!variable)
            return;

        rc<COverviewAnimatedVariableAccess*>(variable)->m_bIsBeingAnimated = animated;
    }
};


static auto& capturedPassElements(Render::CRenderPass& pass) {
    return pass.*privateMember(SPassElementsTag{});
}

static PHLWINDOW getOverviewWindowToShow(const PHLWINDOW& window) {
    if (!window)
        return nullptr;

    if (window->grouping().group())
        return window->grouping().group()->current();

    return window;
}

static bool shouldShowOverviewWindow(const PHLWINDOW& window) {
    const auto WINDOW = getOverviewWindowToShow(window);

    if (!validMapped(WINDOW))
        return false;

    if (windowPinned(WINDOW) && WINDOW->isFloating())
        return false;

    return true;
}

static float getOverviewWindowTargetOpacity(const PHLWINDOW& window) {
    if (!window)
        return 1.F;

    const bool  fullscreen     = Fullscreen::controller()->isFullscreen(window);
    const bool  active         = Desktop::focusState()->window() == window;
    float       targetOpacity  = fullscreen ? ScrollOverview::Config::getValue<float>("decoration:fullscreen_opacity") :
        active ? ScrollOverview::Config::getValue<float>("decoration:active_opacity") : ScrollOverview::Config::getValue<float>("decoration:inactive_opacity");
    const auto& ruleOpacityVar = fullscreen ? window->m_ruleApplicator->alphaFullscreen() : active ? window->m_ruleApplicator->alpha() : window->m_ruleApplicator->alphaInactive();

    targetOpacity = ruleOpacityVar.valueOr(Desktop::Types::SAlphaValue{}).applyAlpha(targetOpacity);

    return std::clamp(targetOpacity, 0.F, 1.F);
}

// Hyprland 0.57 keeps a window's decoration gradients private to its
// decorations. The overview draws its own border and shadow, so it derives the
// same colours from config and window rules for the window it treats as active.
// ponytail: no focus-change colour fade in previews; mirror the decoration's
// gradient animation if that is ever needed.
struct SOverviewDecorationColors {
    Config::CGradientValueData border;
    Config::CGradientValueData shadow;
    Config::CGradientValueData glow;
};

static SOverviewDecorationColors getOverviewDecorationColors(const PHLWINDOW& window, bool active) {
    const auto& GROUP       = window->grouping().group();
    const bool  GROUPLOCKED = Desktop::windowState()->groupsLocked() || (GROUP && GROUP->locked());
    const bool  NOGROUP     = window->grouping().rules() & Desktop::View::GROUP_DENY;
    const auto  BORDERKEY   = GROUP ? (active ? (GROUPLOCKED ? "group:col.border_locked_active" : "group:col.border_active") :
                                                (GROUPLOCKED ? "group:col.border_locked_inactive" : "group:col.border_inactive")) :
                                      (active ? (NOGROUP ? "general:col.nogroup_border_active" : "general:col.active_border") :
                                                (NOGROUP ? "general:col.nogroup_border" : "general:col.inactive_border"));
    const auto  BORDERCOLOR = ScrollOverview::Config::getValue<Config::CGradientValueData>(BORDERKEY);

    SOverviewDecorationColors colors;
    colors.border = active ? window->m_ruleApplicator->activeBorderColor().valueOr(BORDERCOLOR) : window->m_ruleApplicator->inactiveBorderColor().valueOr(BORDERCOLOR);

    const auto TRAITS = window->backend().traits();
    if (TRAITS.overrideRedirect || TRAITS.suggestsNoBorder) {
        colors.shadow = Config::CGradientValueData{CHyprColor{0, 0, 0, 0}};
        colors.glow   = colors.shadow;
        return colors;
    }

    const auto inactiveOr = [](const char* inactiveKey, const Config::CGradientValueData& activeColor) {
        return Config::mgr()->getConfigValue(inactiveKey).setByUser ? ScrollOverview::Config::getValue<Config::CGradientValueData>(inactiveKey) : activeColor;
    };
    const auto SHADOWACTIVE = ScrollOverview::Config::getValue<Config::CGradientValueData>("decoration:shadow:color");
    const auto GLOWACTIVE   = ScrollOverview::Config::getValue<Config::CGradientValueData>("decoration:glow:color");
    colors.shadow           = active ? SHADOWACTIVE : inactiveOr("decoration:shadow:color_inactive", SHADOWACTIVE);
    colors.glow             = active ? GLOWACTIVE : inactiveOr("decoration:glow:color_inactive", GLOWACTIVE);
    return colors;
}

static bool isOverviewActiveWindow(const PHLWINDOW& window, const PHLWINDOW& pseudoFocusWindow) {
    return window == (pseudoFocusWindow ? pseudoFocusWindow : Desktop::focusState()->window());
}

// Renders a window as if pseudoFocusWindow had keyboard focus: active opacity
// and dim are swapped for the duration of the queueing only. Border and shadow
// colours come from getOverviewDecorationColors.
struct SOverviewPseudoFocusState {
    PHLWINDOW                window;
    float                    previousOpacity = 1.F;
    float                    previousDim     = 0.F;
    bool                     active          = false;

    SOverviewPseudoFocusState(const PHLWINDOW& window_, const PHLWINDOW& pseudoFocusWindow) : window(window_) {
        if (!window || !pseudoFocusWindow)
            return;

        active               = true;
        previousOpacity      = window->presentation().alpha(Desktop::View::WINDOW_ALPHA_ACTIVE)->value();
        previousDim          = window->presentation().dimPercent();

        const bool ISACTIVE = window == pseudoFocusWindow;
        window->presentation().alpha(Desktop::View::WINDOW_ALPHA_ACTIVE)->value() = getOverviewWindowTargetOpacity(window);

        float dim = ISACTIVE || window->m_ruleApplicator->noDim().valueOrDefault() || !ScrollOverview::Config::getValue<bool>("decoration:dim_inactive") ?
            0.F :
            ScrollOverview::Config::getValue<float>("decoration:dim_strength");
        if (window->backend().traits().hasModalChild && ScrollOverview::Config::getValue<bool>("decoration:dim_modal"))
            dim += (1.F - dim) / 2.F;
        window->presentation().warpDimPercent(dim);
    }

    ~SOverviewPseudoFocusState() {
        if (!active)
            return;

        window->presentation().alpha(Desktop::View::WINDOW_ALPHA_ACTIVE)->value() = previousOpacity;
        window->presentation().warpDimPercent(previousDim);
    }
};


static void roundStandaloneWindowPassElements(const PHLWINDOW& window, PHLMONITOR monitor, float renderScale, Render::CRenderPass& windowPass) {
    if (!window || !monitor)
        return;

    if (Fullscreen::controller()->isFullscreen(window))
        return;

    const int   rounding      = sc<int>(std::round(window->presentation().rounding() * monitor->m_scale * renderScale));
    const float roundingPower = window->presentation().roundingPower();

    if (rounding <= 0)
        return;

    for (const auto& passElement : capturedPassElements(windowPass)) {
        if (!passElement.element)
            continue;

        auto* surfacePassElement = dc<CSurfacePassElement*>(passElement.element.get());
        if (!surfacePassElement || surfacePassElement->m_data.pWindow != window || surfacePassElement->m_data.popup)
            continue;

        surfacePassElement->m_data.dontRound     = false;
        surfacePassElement->m_data.rounding      = rounding;
        surfacePassElement->m_data.roundingPower = roundingPower;
    }
}

static void blockOverviewWindowBlurOptimization(const PHLWINDOW& window, Render::CRenderPass& windowPass) {
    if (!window)
        return;

    for (const auto& passElement : capturedPassElements(windowPass)) {
        if (!passElement.element)
            continue;

        auto* surfacePassElement = dc<CSurfacePassElement*>(passElement.element.get());
        if (!surfacePassElement || surfacePassElement->m_data.pWindow != window)
            continue;

        surfacePassElement->m_data.blockBlurOptimization = true;
    }
}

static void scaleOverviewChildSurfaceGeometry(const PHLWINDOW& window, const Vector2D& targetPosition, const Vector2D& targetSize,
                                              Render::CRenderPass& windowPass) {
    if (!window)
        return;

    const auto REPORTEDSIZE = window->backend().reportedSize();
    if (REPORTEDSIZE.x <= 0 || REPORTEDSIZE.y <= 0)
        return;

    const Vector2D SURFACESCALE = targetSize / REPORTEDSIZE;

    for (const auto& passElement : capturedPassElements(windowPass)) {
        if (!passElement.element)
            continue;

        auto* surfacePassElement = dc<CSurfacePassElement*>(passElement.element.get());
        if (!surfacePassElement || surfacePassElement->m_data.pWindow != window || surfacePassElement->m_data.mainSurface)
            continue;

        if (surfacePassElement->m_data.popup) {
            // Popup positions are relative to the unscaled top-level window,
            // while `pos` already contains the overview window position.
            // Scale that relative part, including the popup's own subsurface
            // offset, around the overview window origin.
            surfacePassElement->m_data.pos = targetPosition + (surfacePassElement->m_data.pos - targetPosition) * SURFACESCALE;
            surfacePassElement->m_data.localPos *= SURFACESCALE;

            // Hyprland deliberately disables squishing for popups, which also
            // disables its resize scaling. Re-enable it for this pass element,
            // but expand the clamp dimensions to the complete scaled surface
            // so the popup remains allowed to extend beyond the main window.
            const auto SURFACESIZE =
                surfacePassElement->m_data.surface->m_current.size.clamp({2.F, 2.F}, {INFINITY, INFINITY}) * SURFACESCALE;
            surfacePassElement->m_data.w                = std::max(1.0, surfacePassElement->m_data.localPos.x + SURFACESIZE.x + 1.0);
            surfacePassElement->m_data.h                = std::max(1.0, surfacePassElement->m_data.localPos.y + SURFACESIZE.y + 1.0);
            surfacePassElement->m_data.squishOversized = true;
            continue;
        }

        // Hyprland scales a regular subsurface texture during a resize, but
        // its cumulative position remains in the client's reported coordinate
        // space. Apply the same scale to the position so embedded dmabuf
        // surfaces (OBS preview, OrcaSlicer viewport, etc.) stay attached.
        surfacePassElement->m_data.localPos *= SURFACESCALE;
    }
}

// Moves the captured window pass into the frame pass. renderWindow queues
// popups after the main surface, but the overview adds its own decorations and
// border afterwards, so popups are held back and queued by commitWindowPopups.
static std::vector<UP<IPassElement>> commitWindowPass(Render::CRenderContext& ctx, const PHLWINDOW& window, Render::CRenderPass& windowPass) {
    std::vector<UP<IPassElement>> popups;
    for (auto& passElement : capturedPassElements(windowPass)) {
        if (!passElement.element)
            continue;

        const auto* surfacePassElement = dc<const CSurfacePassElement*>(passElement.element.get());
        if (surfacePassElement && surfacePassElement->m_data.pWindow == window && surfacePassElement->m_data.popup)
            popups.emplace_back(std::move(passElement.element));
        else
            Render::IHyprRenderer::addPassElement(ctx, std::move(passElement.element));
    }
    windowPass.clear();
    return popups;
}

static void commitWindowPopups(Render::CRenderContext& ctx, std::vector<UP<IPassElement>>& popups, const std::optional<Render::SRenderModifData>& popupTransform = std::nullopt) {
    if (popups.empty())
        return;

    if (popupTransform)
        Render::IHyprRenderer::addPassElement(ctx, makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = *popupTransform}));
    for (auto& popup : popups)
        Render::IHyprRenderer::addPassElement(ctx, std::move(popup));
    if (popupTransform)
        Render::IHyprRenderer::addPassElement(ctx, makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = Render::SRenderModifData{}}));
}

static SOverviewWindowMetrics getOverviewWindowMetrics(PHLMONITOR monitor, const PHLWINDOW& window, float renderScale) {
    SOverviewWindowMetrics metrics;
    metrics.renderScale   = renderScale;
    metrics.targetOpacity = getOverviewWindowTargetOpacity(window);

    if (!monitor || !window)
        return metrics;

    metrics.pxScale                 = monitor->m_scale * renderScale;
    metrics.borderSize              = window->presentation().borderSize();
    metrics.borderPx                = sc<int>(std::round(metrics.borderSize * monitor->m_scale));
    metrics.borderPxScaled          = metrics.borderSize * metrics.pxScale;
    metrics.roundingBase            = window->presentation().rounding();
    metrics.roundingPower           = window->presentation().roundingPower();
    metrics.correctionOffset        = metrics.borderSize * (M_SQRT2 - 1) * std::max(2.0 - metrics.roundingPower, 0.0);
    metrics.outerRound              = metrics.roundingBase > 0 ? (metrics.roundingBase + metrics.borderSize) - metrics.correctionOffset : 0.F;
    metrics.roundingPx              = sc<int>(std::round(metrics.roundingBase * metrics.pxScale));
    metrics.outerRoundPx            = sc<int>(std::round(metrics.outerRound * metrics.pxScale));

    return metrics;
}

static CBox getOverviewOuterBorderBox(const CBox& windowBox, const SOverviewWindowMetrics& metrics) {
    return windowBox.copy().expand(metrics.borderPx);
}

static CBox getOverviewBorderBox(const CBox& windowBox, const SOverviewWindowMetrics&) {
    return windowBox;
}

static void renderOverviewWindowShadow(Render::CRenderContext& ctx, PHLMONITOR monitor, const PHLWINDOW& window, const CBox& windowBox, const SOverviewWindowMetrics& metrics, const SOverviewDecorationColors& colors, bool selected) {
    if (!monitor || !window || !window->mapped())
        return;

    const auto PSHADOWS      = ScrollOverview::Config::getValue<int>("decoration:shadow:enabled");
    const auto PSHADOWSIZE   = ScrollOverview::Config::getValue<int>("decoration:shadow:range");
    const auto PSHADOWSCALE  = ScrollOverview::Config::getValue<float>("decoration:shadow:scale");
    const auto PSHADOWOFFSET = ScrollOverview::Config::getValue<Hyprlang::VEC2>("decoration:shadow:offset");

    if (PSHADOWS != 1 || PSHADOWSIZE <= 0)
        return;

    if (window->backend().traits().overrideRedirect || window->backend().traits().suggestsNoBorder || !window->m_ruleApplicator->decorate().valueOrDefault() ||
        window->m_ruleApplicator->noShadow().valueOrDefault())
        return;

    const int   rangePx          = sc<int>(std::round(PSHADOWSIZE * monitor->m_scale * metrics.renderScale));
    const float shadowScale      = std::clamp(PSHADOWSCALE, 0.F, 1.F);
    const auto  shadowOffset     = Vector2D{PSHADOWOFFSET.x, PSHADOWOFFSET.y} * monitor->m_scale * metrics.renderScale;

    if (rangePx <= 0)
        return;

    CBox shadowBaseBox = getOverviewOuterBorderBox(windowBox, metrics);
    CBox cutoutBox     = windowBox;
    CBox shadowBox     = shadowBaseBox.copy().expand(rangePx).scaleFromCenter(shadowScale).translate(shadowOffset);
    shadowBox.round();

    if (shadowBox.width < 1 || shadowBox.height < 1)
        return;

    const auto& shadowColor = colors.shadow;
    if (std::ranges::none_of(shadowColor.m_colors, [](const CHyprColor& color) { return color.a > 0.F; }))
        return;

    Render::IHyprRenderer::addPassElement(ctx, makeUnique<COverviewShadowPassElement>(COverviewShadowPassElement::SData{
        .monitor       = monitor,
        .fullBox       = shadowBox,
        .cutoutBox     = cutoutBox.round(),
        .rounding      = metrics.outerRoundPx,
        .roundingPower = metrics.roundingPower,
        .range         = rangePx,
        .renderPower   = ScrollOverview::Config::getValue<int>("decoration:shadow:render_power"),
        .color         = shadowColor,
        .alpha         = metrics.targetOpacity,
        .ignoreWindow  = true,
    }));
}

static void renderOverviewWindowBorder(Render::CRenderContext& ctx, PHLMONITOR monitor, const PHLWINDOW& window, const CBox& windowBox, const SOverviewWindowMetrics& metrics, const SOverviewDecorationColors& colors, bool selected) {
    if (!monitor || !window || !window->mapped())
        return;

    if (metrics.borderSize <= 0.F)
        return;


    // Match the surface's final pixel grid, as native Hyprland borders do.
    // Fractional camera bounds otherwise lose or gain a pixel on individual edges.
    const CBox borderBox = getOverviewBorderBox(windowBox, metrics).round();

    CBorderPassElement::SBorderData data;
    data.box           = borderBox;
    data.round         = metrics.roundingPx;
    data.outerRound    = metrics.outerRoundPx;
    data.roundingPower = metrics.roundingPower;
    data.a             = metrics.targetOpacity;
    data.borderSize    = metrics.borderSize;
    data.grad1         = colors.border;
    Render::IHyprRenderer::addPassElement(ctx, makeUnique<CBorderPassElement>(data));
}

static void renderOverviewGroupTabIndicators(Render::CRenderContext& ctx, PHLMONITOR monitor, const PHLWINDOW& window, const CBox& windowBox, const SOverviewWindowMetrics& metrics, float alpha) {
    if (!monitor || !window || !window->grouping().group() || window->grouping().group()->size() < 1)
        return;

    const auto PINDICATORHEIGHT = ScrollOverview::Config::getValue<int>("group:groupbar:indicator_height");
    const auto PINDICATORGAP    = ScrollOverview::Config::getValue<int>("group:groupbar:indicator_gap");
    const auto PHEIGHT          = ScrollOverview::Config::getValue<int>("group:groupbar:height");
    const auto PGRADIENTS       = ScrollOverview::Config::getValue<bool>("group:groupbar:gradients");
    const auto PRENDERTITLES    = ScrollOverview::Config::getValue<bool>("group:groupbar:render_titles");
    const auto PSTACKED         = ScrollOverview::Config::getValue<bool>("group:groupbar:stacked");
    const auto PROUNDING        = ScrollOverview::Config::getValue<int>("group:groupbar:rounding");
    const auto PROUNDINGPOWER   = ScrollOverview::Config::getValue<float>("group:groupbar:rounding_power");
    const auto POUTERGAP        = ScrollOverview::Config::getValue<int>("group:groupbar:gaps_out");
    const auto PINNERGAP        = ScrollOverview::Config::getValue<int>("group:groupbar:gaps_in");

    if (PINDICATORHEIGHT <= 0)
        return;

    const bool  groupLocked  = window->grouping().group()->locked() || Desktop::windowState()->groupsLocked();
    const auto  groupWindows = window->grouping().group()->windows();
    const auto  focusedWindow = Desktop::focusState()->window();
    const float indicatorH   = sc<float>(PINDICATORHEIGHT) * metrics.pxScale;
    const float outerGap     = sc<float>(POUTERGAP) * metrics.pxScale;
    const float innerGap     = sc<float>(PINNERGAP) * metrics.pxScale;
    const float oneBarHeight = sc<float>(POUTERGAP + PINDICATORHEIGHT + PINDICATORGAP + ((PGRADIENTS || PRENDERTITLES) ? PHEIGHT : 0)) * metrics.pxScale;
    const int   rounding     = sc<int>(std::round(PROUNDING * metrics.pxScale));
    CBox        indicatorArea = windowBox.copy().expand(metrics.borderPxScaled);
    auto* const GROUPCOLACTIVE         = sc<Config::CGradientValueData*>(ScrollOverview::Config::valueRef<Config::IComplexConfigValue>("group:groupbar:col.active").ptr());
    auto* const GROUPCOLINACTIVE       = sc<Config::CGradientValueData*>(ScrollOverview::Config::valueRef<Config::IComplexConfigValue>("group:groupbar:col.inactive").ptr());
    auto* const GROUPCOLACTIVELOCKED   = sc<Config::CGradientValueData*>(ScrollOverview::Config::valueRef<Config::IComplexConfigValue>("group:groupbar:col.locked_active").ptr());
    auto* const GROUPCOLINACTIVELOCKED = sc<Config::CGradientValueData*>(ScrollOverview::Config::valueRef<Config::IComplexConfigValue>("group:groupbar:col.locked_inactive").ptr());
    const auto* const COLACTIVE        = groupLocked ? GROUPCOLACTIVELOCKED : GROUPCOLACTIVE;
    const auto* const COLINACTIVE      = groupLocked ? GROUPCOLINACTIVELOCKED : GROUPCOLINACTIVE;

    float xoff = 0.F;
    float yoff = 0.F;

    for (size_t i = 0; i < groupWindows.size(); ++i) {
        const size_t windowIdx = PSTACKED ? groupWindows.size() - i - 1 : i;
        const auto   member    = groupWindows[windowIdx].lock();
        if (!member)
            continue;

        CHyprColor color = member == focusedWindow ? COLACTIVE->m_colors[0] : COLINACTIVE->m_colors[0];

        color.a *= alpha;
        if (color.a <= 0.F)
            continue;

        CBox box;
        if (PSTACKED) {
            box = {indicatorArea.x, indicatorArea.y - yoff - outerGap - indicatorH, indicatorArea.width, indicatorH};
            yoff += oneBarHeight;
        } else {
            const float barWidth = (indicatorArea.width - innerGap * (groupWindows.size() - 1)) / groupWindows.size();
            box                  = {indicatorArea.x + xoff, indicatorArea.y - outerGap - indicatorH, barWidth, indicatorH};
            xoff += innerGap + barWidth;
        }

        box.round();
        if (box.empty())
            continue;

        CRectPassElement::SRectData data;
        data.box           = box;
        data.color         = color;
        data.round         = rounding;
        data.roundingPower = PROUNDINGPOWER;
        Render::IHyprRenderer::addPassElement(ctx, makeUnique<CRectPassElement>(data));
    }
}

static void renderOverviewGroupTabTitles(Render::CRenderContext& ctx, PHLMONITOR monitor, const PHLWINDOW& window, const CBox& windowBox, const SOverviewWindowMetrics& metrics, float alpha) {
    if (!monitor || !window || !window->grouping().group() || window->grouping().group()->size() < 1 || alpha <= 0.F)
        return;

    const auto PRENDERTITLES            = ScrollOverview::Config::getValue<bool>("group:groupbar:render_titles");
    const auto PTITLEFONTSIZE           = ScrollOverview::Config::getValue<int>("group:groupbar:font_size");
    const auto PHEIGHT                  = ScrollOverview::Config::getValue<int>("group:groupbar:height");
    const auto PINDICATORGAP            = ScrollOverview::Config::getValue<int>("group:groupbar:indicator_gap");
    const auto PINDICATORHEIGHT         = ScrollOverview::Config::getValue<int>("group:groupbar:indicator_height");
    const auto PSTACKED                 = ScrollOverview::Config::getValue<bool>("group:groupbar:stacked");
    const auto POUTERGAP                = ScrollOverview::Config::getValue<int>("group:groupbar:gaps_out");
    const auto PINNERGAP                = ScrollOverview::Config::getValue<int>("group:groupbar:gaps_in");
    const auto PTEXTOFFSET              = ScrollOverview::Config::getValue<int>("group:groupbar:text_offset");
    const auto PTEXTPADDING             = ScrollOverview::Config::getValue<int>("group:groupbar:text_padding");
    const auto PTEXTCOLORACTIVE         = ScrollOverview::Config::getValue<int>("group:groupbar:text_color");
    const auto PTEXTCOLORINACTIVE       = ScrollOverview::Config::getValue<int>("group:groupbar:text_color_inactive");
    const auto PTEXTCOLORLOCKEDACTIVE   = ScrollOverview::Config::getValue<int>("group:groupbar:text_color_locked_active");
    const auto PTEXTCOLORLOCKEDINACTIVE = ScrollOverview::Config::getValue<int>("group:groupbar:text_color_locked_inactive");

    if (!PRENDERTITLES || PHEIGHT <= 0)
        return;

    const bool  groupLocked  = window->grouping().group()->locked() || Desktop::windowState()->groupsLocked();
    const auto  groupWindows = window->grouping().group()->windows();
    const auto  focusedWindow = Desktop::focusState()->window();
    const float outerGap     = sc<float>(POUTERGAP) * metrics.pxScale;
    const float innerGap     = sc<float>(PINNERGAP) * metrics.pxScale;
    const float oneBarHeight = sc<float>(POUTERGAP + PINDICATORHEIGHT + PINDICATORGAP + PHEIGHT) * metrics.pxScale;
    const float titleHeight  = sc<float>(PHEIGHT) * metrics.pxScale;
    const float textPadding  = sc<float>(PTEXTPADDING) * metrics.pxScale;
    const float textOffset   = sc<float>(PTEXTOFFSET) * metrics.pxScale;
    const int   fontSizePx   = std::max(1, sc<int>(std::round(PTITLEFONTSIZE * metrics.pxScale)));

    CBox        titleArea = windowBox.copy().expand(metrics.borderPxScaled);

    const CHyprColor COLORACTIVE         = CHyprColor(PTEXTCOLORACTIVE);
    const CHyprColor COLORINACTIVE       = PTEXTCOLORINACTIVE == -1 ? COLORACTIVE : CHyprColor(PTEXTCOLORINACTIVE);
    const CHyprColor COLORLOCKEDACTIVE   = PTEXTCOLORLOCKEDACTIVE == -1 ? COLORACTIVE : CHyprColor(PTEXTCOLORLOCKEDACTIVE);
    const CHyprColor COLORLOCKEDINACTIVE = PTEXTCOLORLOCKEDINACTIVE == -1 ? COLORINACTIVE : CHyprColor(PTEXTCOLORLOCKEDINACTIVE);
    const auto       FONTWEIGHTACTIVE    = sc<Config::CFontWeightConfigValueData*>(ScrollOverview::Config::valueRef<Config::IComplexConfigValue>("group:groupbar:font_weight_active").ptr());
    const auto       FONTWEIGHTINACTIVE  = sc<Config::CFontWeightConfigValueData*>(ScrollOverview::Config::valueRef<Config::IComplexConfigValue>("group:groupbar:font_weight_inactive").ptr());
    const auto       TITLEFONTFAMILY     = ScrollOverview::Config::getValue<std::string>("group:groupbar:font_family");
    const auto       FONTFAMILY          = TITLEFONTFAMILY != STRVAL_EMPTY ? TITLEFONTFAMILY : ScrollOverview::Config::getValue<std::string>("misc:font_family");

    float xoff = 0.F;
    float yoff = 0.F;

    for (size_t i = 0; i < groupWindows.size(); ++i) {
        const size_t windowIdx = PSTACKED ? groupWindows.size() - i - 1 : i;
        const auto   member    = groupWindows[windowIdx].lock();
        if (!member)
            continue;

        const float barWidth = PSTACKED ? titleArea.width : (titleArea.width - innerGap * (groupWindows.size() - 1)) / groupWindows.size();

        CBox box;
        if (PSTACKED) {
            box = {titleArea.x, titleArea.y - yoff - outerGap - titleHeight - sc<float>(PINDICATORHEIGHT) * metrics.pxScale - sc<float>(PINDICATORGAP) * metrics.pxScale,
                   titleArea.width, titleHeight};
            yoff += oneBarHeight;
        } else {
            box = {titleArea.x + xoff, titleArea.y - outerGap - titleHeight - sc<float>(PINDICATORHEIGHT) * metrics.pxScale - sc<float>(PINDICATORGAP) * metrics.pxScale,
                   barWidth, titleHeight};
            xoff += innerGap + barWidth;
        }

        box.round();
        if (box.empty())
            continue;

        CHyprColor textColor;
        const bool isCurrent = member == focusedWindow;
        if (groupLocked)
            textColor = isCurrent ? COLORLOCKEDACTIVE : COLORLOCKEDINACTIVE;
        else
            textColor = isCurrent ? COLORACTIVE : COLORINACTIVE;

        textColor.a *= alpha;
        if (textColor.a <= 0.F)
            continue;

        const int maxWidth = std::max(1, sc<int>(std::round(box.width - textPadding * 2.F)));
        auto      titleTex =
            g_pHyprRenderer->renderText(member->metadata().title(), textColor, fontSizePx, false, FONTFAMILY, maxWidth, isCurrent ? FONTWEIGHTACTIVE->m_value : FONTWEIGHTINACTIVE->m_value);
        if (!titleTex || !titleTex->ok())
            continue;

        CBox textBox = box;
        textBox.y += std::ceil((box.height - titleTex->m_size.y) / 2.0 - textOffset);
        textBox.height = titleTex->m_size.y;
        textBox.width  = titleTex->m_size.x;
        textBox.x += std::round((box.width + textPadding) / 2.0 - (titleTex->m_size.x + textPadding) / 2.0);
        textBox.round();

        CTexPassElement::SRenderData data;
        data.tex = titleTex;
        data.box = textBox;
        data.a   = 1.F;
        Render::IHyprRenderer::addPassElement(ctx, makeUnique<CTexPassElement>(data));
    }
}

static bool renderOverviewCustomDecorations(Render::CRenderContext& ctx, PHLMONITOR monitor, const PHLWINDOW& window, const CBox& workspaceBox, const CBox& windowBox,
                                                                           const SOverviewWindowMetrics& metrics, eDecorationLayer layer) {
    if (!monitor || !window)
        return false;

    window->presentation().updateDecorations();

    Render::SRenderModifData modif;
    modif.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_SCALE, metrics.renderScale);
    modif.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_TRANSLATE, workspaceBox.pos());
    bool        drewAny = false;

    for (const auto& deco : window->presentation().decorations()) {
        if (!deco || deco->getDecorationType() != DECORATION_CUSTOM || deco->getDecorationLayer() != layer)
            continue;

        if (!drewAny) {
            Render::IHyprRenderer::addPassElement(ctx, makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = modif}));
            drewAny = true;
        }

        deco->updateWindow(window);
        deco->draw(ctx, monitor, metrics.targetOpacity, window->presentation().renderPresentation());
    }

    if (drewAny)
        Render::IHyprRenderer::addPassElement(ctx, makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = Render::SRenderModifData{}}));

    return drewAny;
}

} // namespace

bool shouldBlurBackground(const PHLWINDOW& window) {
    return window && window->shouldBlur();
}

static bool overviewBoxContains(const CBox& outer, const CBox& inner) {
    constexpr float EPSILON = 0.5F;
    return inner.x >= outer.x - EPSILON && inner.y >= outer.y - EPSILON && inner.x + inner.width <= outer.x + outer.width + EPSILON &&
        inner.y + inner.height <= outer.y + outer.height + EPSILON;
}

static bool overviewWindowFitsWorkspaceBox(const CBox* workspaceBox, const CBox* windowBox) {
    return !workspaceBox || !windowBox || overviewBoxContains(*workspaceBox, *windowBox);
}

static bool overviewWorkspaceBoxReadyForPrecomputedBlur(PHLMONITOR monitor, const CBox* workspaceBox) {
    if (!monitor || !workspaceBox)
        return true;

    constexpr float EPSILON    = 0.5F;
    const auto      RENDERSIZE = monitor->m_size * monitor->m_scale;

    return workspaceBox->x >= -EPSILON && workspaceBox->y >= -EPSILON && workspaceBox->x + workspaceBox->width <= RENDERSIZE.x + EPSILON &&
        workspaceBox->y + workspaceBox->height <= RENDERSIZE.y + EPSILON;
}

bool shouldUsePrecomputedBlur(const PHLWINDOW& window, PHLMONITOR monitor, const CBox* workspaceBox, const CBox* windowBox, bool dragged) {
    return !dragged && ScrollOverview::Config::getValue<bool>("decoration:blur:new_optimizations") && shouldShowOverviewWindow(window) && !window->isFloating() && shouldBlurBackground(window) &&
        overviewWindowFitsWorkspaceBox(workspaceBox, windowBox) && overviewWorkspaceBoxReadyForPrecomputedBlur(monitor, workspaceBox);
}

bool shouldUseBlurFramebuffer(const PHLWINDOW& window, PHLMONITOR monitor, const CBox* workspaceBox, const CBox* windowBox) {
    return shouldUsePrecomputedBlur(window, monitor, workspaceBox, windowBox) ||
        (shouldShowOverviewWindow(window) && shouldBlurBackground(window) && window->m_ruleApplicator->xray().valueOr(false) &&
         overviewWindowFitsWorkspaceBox(workspaceBox, windowBox) && overviewWorkspaceBoxReadyForPrecomputedBlur(monitor, workspaceBox));
}

void forceDecoRecalc(const PHLWINDOW& window) {
    const auto WINDOW = getOverviewWindowToShow(window);
    if (!validMapped(WINDOW))
        return;

    g_pDecorationPositioner->forceRecalcFor(WINDOW);
    WINDOW->presentation().updateDecorations();
}

void renderOverviewWindow(Render::CRenderContext& ctx, const SRenderParams& previewParams) {
    if (!previewParams.window)
        return;

    // Surface, border, rounding and shadow all shrink with the preview.
    auto params = previewParams;
    params.renderScale *= std::clamp(params.previewScale, 0.1F, 1.F);

    const SOverviewPseudoFocusState pseudoFocusState{params.window, params.pseudoFocusWindow};
    const auto colors = getOverviewDecorationColors(params.window, isOverviewActiveWindow(params.window, params.pseudoFocusWindow));
    const bool                   fullscreen   = Fullscreen::controller()->isFullscreen(params.window);
    const SOverviewWindowMetrics metrics      = getOverviewWindowMetrics(params.monitor, params.window, params.renderScale);

    if (!fullscreen)
        renderOverviewWindowShadow(ctx, params.monitor, params.window, params.windowBox, metrics, colors, params.selected);

    const auto underDecos =
        renderOverviewCustomDecorations(ctx, params.monitor, params.window, params.workspaceBox ? *params.workspaceBox : CBox{}, params.windowBox, metrics, DECORATION_LAYER_UNDER);
    if (underDecos)
        OverviewRender::flushPass(ctx, params.monitor);

    const Vector2D previousWindowPos       = params.window->positionAnimation()->value();
    const Vector2D previousWindowSize      = params.window->sizeAnimation()->value();
    const bool     previousAnimatingIn     = params.window->presentation().animatingIn();
    const bool     previousSizeAnimating   = params.window->sizeAnimation()->isBeingAnimated();
    const auto     WORKSPACE               = params.window->m_workspace;
    const bool     OVERRIDEWORKSPACEOFFSET = WORKSPACE && !windowPinned(params.window);
    const Vector2D previousWorkspaceOffset = OVERRIDEWORKSPACEOFFSET ? WORKSPACE->m_renderOffset->value() : Vector2D{};
    const float    previousWorkspaceAlpha  = OVERRIDEWORKSPACEOFFSET && WORKSPACE->m_alpha ? WORKSPACE->m_alpha->value() : 1.F;
    std::optional<Render::SRenderModifData> cameraTransform;

    if (params.cameraTransform && params.monitor) {
        const auto SOURCEGEOMETRY = params.window->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        // Match renderWindow's surface origin, including Hyprland's temporary
        // floating correction during workspace slides. It is not camera motion.
        const CBox SOURCEBOX{
            (SOURCEGEOMETRY.pos() + params.window->presentation().floatingOffset() - params.monitor->m_position) * params.monitor->m_scale,
            SOURCEGEOMETRY.size() * params.monitor->m_scale,
        };

        Render::SRenderModifData transform;
        transform.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_SCALE, params.renderScale);
        transform.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_TRANSLATE,
                                      params.windowBox.pos() - SOURCEBOX.pos() * params.renderScale);
        cameraTransform = std::move(transform);
    }

    if (!cameraTransform) {
        params.window->positionAnimation()->value() = params.monitor->m_position + params.windowBox.pos() / params.monitor->m_scale - params.window->presentation().floatingOffset();
        params.window->sizeAnimation()->value()     = params.windowBox.size() / params.monitor->m_scale;
        params.window->presentation().setAnimatingIn(true);
        COverviewAnimatedVariableAccess::setBeingAnimated(params.window->sizeAnimation().get(), true);
    }
    if (OVERRIDEWORKSPACEOFFSET) {
        WORKSPACE->m_renderOffset->value() = {};
        // Persistent Canvas can outlive its workspace. Hidden workspaces may
        // legitimately carry alpha 0, but a live workspace transition must
        // retain Hyprland's animated alpha so Canvas clients fade with the
        // workspace instead of appearing instantly. Surface passes snapshot
        // this value, so override only the current value while queuing.
        if (WORKSPACE->m_alpha && !WORKSPACE->m_alpha->isBeingAnimated())
            WORKSPACE->m_alpha->value() = 1.F;
    }

    auto restoreWindowGeometry = Hyprutils::Utils::CScopeGuard([&] {
        if (!cameraTransform) {
            params.window->positionAnimation()->value() = previousWindowPos;
            params.window->sizeAnimation()->value()     = previousWindowSize;
            params.window->presentation().setAnimatingIn(previousAnimatingIn);
            COverviewAnimatedVariableAccess::setBeingAnimated(params.window->sizeAnimation().get(), previousSizeAnimating);
        }
        if (OVERRIDEWORKSPACEOFFSET && WORKSPACE) {
            WORKSPACE->m_renderOffset->value() = previousWorkspaceOffset;
            if (WORKSPACE->m_alpha)
                WORKSPACE->m_alpha->value() = previousWorkspaceAlpha;
        }
    });

    const bool usePrecomputedBlur = shouldUsePrecomputedBlur(params.window, params.monitor, params.workspaceBox, &params.windowBox, params.dragged);
    if (cameraTransform)
        Render::IHyprRenderer::addPassElement(ctx, makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = *cameraTransform}));

    Render::CRenderPass windowPass;
    {
        auto redirect = Render::IHyprRenderer::redirectPass(ctx, &windowPass);
        OverviewRender::queueWindow(ctx, params.window, params.monitor, params.now);
    }
    if (!cameraTransform) {
        const Vector2D targetWindowPosition = params.monitor->m_position + params.windowBox.pos() / params.monitor->m_scale;
        scaleOverviewChildSurfaceGeometry(params.window, targetWindowPosition, params.window->sizeAnimation()->value(), windowPass);
    }
    if (!usePrecomputedBlur)
        blockOverviewWindowBlurOptimization(params.window, windowPass);
    roundStandaloneWindowPassElements(params.window, params.monitor, params.renderScale, windowPass);
    auto popups = commitWindowPass(ctx, params.window, windowPass);

    if (cameraTransform)
        Render::IHyprRenderer::addPassElement(ctx, makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = Render::SRenderModifData{}}));

    renderOverviewCustomDecorations(ctx, params.monitor, params.window, params.workspaceBox ? *params.workspaceBox : CBox{}, params.windowBox, metrics, DECORATION_LAYER_OVER);
    renderOverviewCustomDecorations(ctx, params.monitor, params.window, params.workspaceBox ? *params.workspaceBox : CBox{}, params.windowBox, metrics,
                                    DECORATION_LAYER_OVERLAY);

    if (!fullscreen) {
        renderOverviewGroupTabIndicators(ctx, params.monitor, params.window, params.windowBox, metrics, metrics.targetOpacity);
        renderOverviewGroupTabTitles(ctx, params.monitor, params.window, params.windowBox, metrics, metrics.targetOpacity);
        renderOverviewWindowBorder(ctx, params.monitor, params.window, params.windowBox, metrics, colors, params.selected);
    }

    commitWindowPopups(ctx, popups, cameraTransform);
    const bool previousNoSimplify = ctx.m_data.noSimplify;
    if (cameraTransform)
        ctx.m_data.noSimplify = true;
    auto restoreNoSimplify = Hyprutils::Utils::CScopeGuard(
        [&ctx, previousNoSimplify] { ctx.m_data.noSimplify = previousNoSimplify; });
    OverviewRender::flushPass(ctx, params.monitor);
}

} // namespace OverviewWindow
