#define WLR_USE_UNSTABLE

#include <unistd.h>
#include <sstream>
#include <hyprutils/utils/ScopeGuard.hpp>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/xwayland/XSurface.hpp>
#include <hyprland/src/xwayland/XWayland.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/keybinds/Manager.hpp>
#include <hyprland/src/keybinds/Resolver.hpp>
#include <hyprland/src/ipc/s1/S1.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/ElementRenderer.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/managers/input/trackpad/GestureTypes.hpp>
#include <hyprland/src/managers/input/trackpad/TrackpadGestures.hpp>

using namespace Hyprutils::String;

#include "globals.hpp"
#include "Config.hpp"
#include "PluginVersion.hpp"
#include "scrollOverview.hpp"
#include "BarrelShader.hpp"
#include "Experiments.hpp"
#include "Hud.hpp"
#include "Navigator.hpp"
#include "OverviewRender.hpp"
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include "OverviewGesture.hpp"

namespace Desktop::View {
    class CPopup;
}
bool canvasRepositionPopup(Desktop::View::CPopup* popup);
bool canvasTakeClientWindowGesture(const PHLWINDOW& window, std::optional<Layout::eRectCorner> resizeEdge);
CBox canvasX11Configure(void* surface, const CBox& box);
CBox canvasX11Request(void* window, CBox box);
void canvasReleaseX11Windows();

// Every Hyprland function the plugin hooks. SCROLL_OVERVIEW_HOOKS (below the
// handlers) says how each one is found and whether it is only needed while an
// overview exists.
enum eHook : uint8_t {
    HOOK_RENDER_WORKSPACE,
    HOOK_RENDER_WINDOW,
    HOOK_WORKSPACE_WINDOWS,
    HOOK_WORKSPACE_FULLSCREEN,
    HOOK_SCHEDULE_FRAME,
    HOOK_DAMAGE_SURFACE,
    HOOK_SEND_FRAME_EVENTS,
    HOOK_SURFACE_FRAME,
    HOOK_ADD_DAMAGE_BOX,
    HOOK_ADD_DAMAGE_REGION,
    HOOK_DRAW_TEX,
    HOOK_ELEMENT_DRAW_TEX,
    HOOK_DRAW_ELEMENT,
    HOOK_POPUP_REPOSITION,
    HOOK_BEGIN_DRAG_TARGET,
    HOOK_X11_CONFIGURE,
    HOOK_X11_CONFIGURE_REQUEST,
    HOOK_X11_CLIENT_MESSAGE,
    HOOK_COUNT,
};

static std::array<CFunctionHook*, HOOK_COUNT> g_hooks = {};

template <typename TFn>
static TFn original(eHook hook) {
    return rc<TFn>(g_hooks[hook]->m_original);
}

using origRenderWorkspace  = void (*)(void*, Render::CRenderContext&, PHLMONITOR, PHLWORKSPACE, const Time::steady_tp&, const CBox&);
using origRenderWindow     = void (*)(void*, Render::CRenderContext&, PHLWINDOW, PHLMONITOR, const Render::SWindowRenderPresentation&, const Time::steady_tp&, bool,
                                      Render::eRenderPassMode, bool, bool);
using origWorkspaceWindows = void (*)(void*, Render::CRenderContext&, PHLMONITOR, PHLWORKSPACE, const Time::steady_tp&, Render::eSceneMode);
using origAddDamageBox     = void (*)(void*, const CBox&);
using origAddDamageRegion  = void (*)(void*, const pixman_region32_t*);
using origDamageSurface    = void (*)(void*, SP<CWLSurfaceResource>, double, double, double);
using origScheduleFrame    = void (*)(void*, Aquamarine::IOutput::scheduleFrameReason);
using origSendFrameEvents  = void (*)(void*, PHLMONITOR, PHLWORKSPACE, const Time::steady_tp&);
using origSurfaceFrame     = void (*)(void*, const Time::steady_tp&);
using origDrawTex          = void (*)(void*, Render::CRenderContext&, WP<CTexPassElement>, const CRegion&);
using origDrawElement      = void (*)(void*, Render::CRenderContext&, WP<IPassElement>, const CRegion&);
using origBeginDragTarget  = void (*)(void*, SP<Layout::ITarget>, eMouseBindMode, std::optional<Layout::eRectCorner>, bool);
using origX11Configure     = void (*)(void*, const CBox&);
using origConfigureRequest = void (*)(void*, const CBox&);
using origX11ClientMessage = void (*)(void*, xcb_client_message_event_t*);
using origPopupReposition  = void (*)(void*);

static bool g_unloading = false;

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

static bool          renderingOverview = false;
static bool          damageFromSurface = false;
static PHLMONITORREF renderingOverviewMonitor;
struct SWorkspaceCanvasRenderContext {
    PHLMONITOR                       monitor;
    PHLWORKSPACE                     workspace;
    std::vector<SP<CScrollOverview>> canvases;
};
// Only valid while the native workspace pass is executing synchronously.
static SWorkspaceCanvasRenderContext* g_workspaceCanvasRenderContext   = nullptr;
static bool                           g_scrollOverviewHooksActive      = false;
static wl_event_source*               g_workspaceCanvasSyncIdle        = nullptr;
static wl_event_source*               g_workspaceCanvasTransitionTimer = nullptr;

static void                           failNotif(const std::string& reason);

static int                            workspaceCanvasTransitionTimerCallback(void*) {
    if (g_unloading) {
        clearWorkspaceTransitionOverviews();
        g_workspaceCanvasTransitionTimer = nullptr;
        return 0;
    }

    const auto transitions = workspaceTransitionOverviews();
    for (const auto& overview : transitions) {
        const auto* canvas    = dynamic_cast<const CScrollOverview*>(overview.get());
        const auto  workspace = canvas ? canvas->workspaceCanvas() : PHLWORKSPACE{};
        const bool  animating = valid(workspace) && (workspace->m_alpha->isBeingAnimated() || workspace->m_renderOffset->isBeingAnimated());
        if (!animating) {
            releaseWorkspaceTransitionOverview(overview.get());
            // Retiring a camera plane erases its output-space footprint once.
            // Native source-window damage cannot describe that old footprint.
            if (const auto monitor = overview->pMonitor.lock())
                g_pHyprRenderer->damageMonitor(monitor);
        }
    }

    if (workspaceTransitionOverviews().empty()) {
        if (g_workspaceCanvasTransitionTimer)
            wl_event_source_remove(g_workspaceCanvasTransitionTimer);
        g_workspaceCanvasTransitionTimer = nullptr;
        if (scrollOverviews().empty())
            disableScrollOverviewHooks();
        return 0;
    }

    if (g_workspaceCanvasTransitionTimer)
        wl_event_source_timer_update(g_workspaceCanvasTransitionTimer, 16);
    return 0;
}

static void scheduleWorkspaceCanvasTransitionCleanup() {
    if (g_workspaceCanvasTransitionTimer || !g_pCompositor || g_unloading)
        return;

    g_workspaceCanvasTransitionTimer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, workspaceCanvasTransitionTimerCallback, nullptr);
    if (g_workspaceCanvasTransitionTimer)
        wl_event_source_timer_update(g_workspaceCanvasTransitionTimer, 16);
}

static SP<IOverview> workspaceTransitionOverviewFor(PHLMONITOR monitor, PHLWORKSPACE workspace) {
    if (!monitor || !workspace)
        return {};

    const auto it = std::ranges::find_if(workspaceTransitionOverviews(), [&](const auto& overview) {
        const auto* canvas = overview ? dynamic_cast<const CScrollOverview*>(overview.get()) : nullptr;
        return canvas && canvas->pMonitor.lock() == monitor && canvas->workspaceCanvas() == workspace;
    });
    return it == workspaceTransitionOverviews().end() ? SP<IOverview>{} : *it;
}

static void hkScheduleFrame(void* thisptr, Aquamarine::IOutput::scheduleFrameReason reason) {
    const auto OVERVIEW = scrollOverviewForMonitor(sc<Monitor::CMonitor*>(thisptr)->m_self.lock());
    if (OVERVIEW) {
        using enum Aquamarine::IOutput::scheduleFrameReason;

        const bool THROTTLEDREASON = reason == AQ_SCHEDULE_UNKNOWN || reason == AQ_SCHEDULE_CLIENT_UNKNOWN || reason == AQ_SCHEDULE_NEEDS_FRAME ||
            reason == AQ_SCHEDULE_RENDER_MONITOR || reason == AQ_SCHEDULE_DAMAGE;

        if (THROTTLEDREASON && !OVERVIEW->blockDamageReporting && !OVERVIEW->shouldAllowRealtimePreviewSchedule())
            return;
    }

    original<origScheduleFrame>(HOOK_SCHEDULE_FRAME)(thisptr, reason);
}

static void renderWorkspaceCanvasPlane(Render::CRenderContext& ctx, const SP<CScrollOverview>& overview) {
    const auto previousOverview  = g_pScrollOverview;
    const auto previousMonitor   = renderingOverviewMonitor;
    const bool previousRendering = renderingOverview;
    g_pScrollOverview            = overview;
    renderingOverviewMonitor     = overview->pMonitor;
    renderingOverview            = true;
    auto restore                 = Hyprutils::Utils::CScopeGuard([previousOverview, previousMonitor, previousRendering] {
        g_pScrollOverview        = previousOverview;
        renderingOverviewMonitor = previousMonitor;
        renderingOverview        = previousRendering;
    });
    overview->renderWorkspaceTransition(ctx);
}

static void renderWorkspaceWindowPlanes(eHook hook, void* thisptr, Render::CRenderContext& ctx, PHLMONITOR monitor, PHLWORKSPACE workspace, const Time::steady_tp& now,
                                        Render::eSceneMode mode) {
    const auto  ORIGINAL = original<origWorkspaceWindows>(hook);
    const auto* context  = g_workspaceCanvasRenderContext;
    if (!context || renderingOverview || context->monitor != monitor || context->workspace != workspace) {
        ORIGINAL(thisptr, ctx, monitor, workspace, now, mode);
        return;
    }

    // Native rendering includes every animating workspace in this one pass.
    // Keep outgoing Canvas below the incoming workspace, before special
    // workspaces, pinned clients, and layer surfaces are drawn natively.
    for (const auto& overview : context->canvases) {
        if (overview->workspaceCanvas() != workspace)
            renderWorkspaceCanvasPlane(ctx, overview);
    }
    ORIGINAL(thisptr, ctx, monitor, workspace, now, mode);
    for (const auto& overview : context->canvases) {
        if (overview->workspaceCanvas() == workspace)
            renderWorkspaceCanvasPlane(ctx, overview);
    }
}

static void hkWorkspaceWindows(void* thisptr, Render::CRenderContext& ctx, PHLMONITOR monitor, PHLWORKSPACE workspace, const Time::steady_tp& now, Render::eSceneMode mode) {
    renderWorkspaceWindowPlanes(HOOK_WORKSPACE_WINDOWS, thisptr, ctx, monitor, workspace, now, mode);
}

static void hkWorkspaceFullscreen(void* thisptr, Render::CRenderContext& ctx, PHLMONITOR monitor, PHLWORKSPACE workspace, const Time::steady_tp& now, Render::eSceneMode mode) {
    renderWorkspaceWindowPlanes(HOOK_WORKSPACE_FULLSCREEN, thisptr, ctx, monitor, workspace, now, mode);
}

static void hkRenderWindow(void* thisptr, Render::CRenderContext& ctx, PHLWINDOW window, PHLMONITOR monitor, const Render::SWindowRenderPresentation& presentation,
                           const Time::steady_tp& now, bool decorate, Render::eRenderPassMode mode, bool ignorePosition, bool standalone) {
    const auto* context = g_workspaceCanvasRenderContext;
    if (context && !renderingOverview && !standalone && context->monitor == monitor &&
        std::ranges::any_of(context->canvases, [&window](const auto& overview) { return overview->ownsWorkspaceTransitionWindow(window); }))
        return; // drawn once through its Canvas camera, including off-screen source clients

    original<origRenderWindow>(HOOK_RENDER_WINDOW)(thisptr, ctx, window, monitor, presentation, now, decorate, mode, ignorePosition, standalone);
}

static void hkRenderWorkspace(void* thisptr, Render::CRenderContext& ctx, PHLMONITOR pMonitor, PHLWORKSPACE pWorkspace, const Time::steady_tp& now, const CBox& geometry) {
    const auto  ORIGINAL = original<origRenderWorkspace>(HOOK_RENDER_WORKSPACE);
    auto        overview = scrollOverviewForMonitor(pMonitor);
    const auto* canvas   = overview ? dynamic_cast<const CScrollOverview*>(overview.get()) : nullptr;
    if (!renderingOverview && (!overview || (canvas && canvas->isWorkspaceBoundCanvas()))) {
        SWorkspaceCanvasRenderContext context{pMonitor, pWorkspace, {}};
        const auto                    append = [&](const SP<IOverview>& candidate) {
            const auto scene     = Hyprutils::Memory::dynamicPointerCast<CScrollOverview>(candidate);
            const auto workspace = scene ? scene->workspaceCanvas() : PHLWORKSPACE{};
            if (!scene || !scene->isWorkspaceBoundCanvas() || candidate->pMonitor.lock() != pMonitor || !workspace ||
                (workspace != pWorkspace && !workspace->m_alpha->isBeingAnimated() && !workspace->m_renderOffset->isBeingAnimated()))
                return;
            if (std::ranges::none_of(context.canvases, [&](const auto& existing) { return existing->workspaceCanvas() == workspace; }))
                context.canvases.push_back(scene);
        };
        append(overview);
        for (const auto& outgoing : workspaceTransitionOverviews())
            append(outgoing);

        const bool animating = std::ranges::any_of(context.canvases, [](const auto& scene) {
            const auto workspace = scene->workspaceCanvas();
            return workspace->m_alpha->isBeingAnimated() || workspace->m_renderOffset->isBeingAnimated();
        });
        if (animating) {
            const auto previousContext     = g_workspaceCanvasRenderContext;
            g_workspaceCanvasRenderContext = &context;
            auto restore                   = Hyprutils::Utils::CScopeGuard([previousContext] { g_workspaceCanvasRenderContext = previousContext; });
            ORIGINAL(thisptr, ctx, pMonitor, pWorkspace, now, geometry);
            return;
        }
    }
    if (canvas && canvas->isWorkspaceBoundCanvas() && canvas->workspaceCanvas() != pWorkspace)
        overview = workspaceTransitionOverviewFor(pMonitor, pWorkspace);

    if (!overview)
        overview = workspaceTransitionOverviewFor(pMonitor, pWorkspace);

    if (!overview || renderingOverview) {
        ORIGINAL(thisptr, ctx, pMonitor, pWorkspace, now, geometry);
        return;
    }

    const bool PREVRENDERINGOVERVIEW = renderingOverview;
    const auto PREVRENDERINGMONITOR  = renderingOverviewMonitor;
    const auto PREVOVERVIEW          = g_pScrollOverview;
    renderingOverview                = true;
    renderingOverviewMonitor         = pMonitor;
    g_pScrollOverview                = overview;
    overview->render(ctx);
    g_pScrollOverview        = PREVOVERVIEW;
    renderingOverviewMonitor = PREVRENDERINGMONITOR;
    renderingOverview        = PREVRENDERINGOVERVIEW;
}

static void hkDamageSurface(void* thisptr, SP<CWLSurfaceResource> surface, double x, double y, double scale) {
    const auto allHandleSurface = [&surface](const auto& overviews) {
        return std::ranges::all_of(overviews, [&surface](const auto& overview) { return !overview || overview->shouldHandleSurfaceDamage(surface); });
    };
    const auto anyBlocksDamage = [](const auto& overviews) {
        return std::ranges::any_of(overviews, [](const auto& overview) { return overview && overview->blockDamageReporting; });
    };
    const bool HASOVERVIEWS = !scrollOverviews().empty() || !workspaceTransitionOverviews().empty();
    const bool HANDLED      = !HASOVERVIEWS || anyBlocksDamage(scrollOverviews()) || anyBlocksDamage(workspaceTransitionOverviews()) ||
        (allHandleSurface(scrollOverviews()) && allHandleSurface(workspaceTransitionOverviews()));
    if (HANDLED) {
        const bool PREVDAMAGEFROMSURFACE = damageFromSurface;
        damageFromSurface                = HASOVERVIEWS;
        original<origDamageSurface>(HOOK_DAMAGE_SURFACE)(thisptr, surface, x, y, scale);
        damageFromSurface = PREVDAMAGEFROMSURFACE;
    }
}

static void hkSendFrameEventsToWorkspace(void* thisptr, PHLMONITOR monitor, PHLWORKSPACE workspace, const Time::steady_tp& now) {
    if (const auto OVERVIEW = scrollOverviewForMonitor(monitor)) {
        const auto* CANVAS = dynamic_cast<const CScrollOverview*>(OVERVIEW.get());
        if (!CANVAS || !CANVAS->isWorkspaceBoundCanvas() || CANVAS->workspaceCanvas() == workspace)
            return;
    }
    if (workspaceTransitionOverviewFor(monitor, workspace))
        return;

    original<origSendFrameEvents>(HOOK_SEND_FRAME_EVENTS)(thisptr, monitor, workspace, now);
}

static void hkSurfaceFrame(void* thisptr, const Time::steady_tp& now) {
    const auto SURFACE       = sc<CWLSurfaceResource*>(thisptr)->m_self.lock();
    const auto disallowFrame = [&SURFACE, &now](const auto& overviews) {
        return std::ranges::any_of(overviews, [&SURFACE, &now](const auto& overview) { return overview && !overview->shouldAllowSurfaceFrame(SURFACE, now); });
    };

    if (disallowFrame(scrollOverviews()) || disallowFrame(workspaceTransitionOverviews()))
        return;

    original<origSurfaceFrame>(HOOK_SURFACE_FRAME)(thisptr, now);
}

// An offscreen buffer that is not the monitor's size (a transformed window's
// canvas, a work buffer) has its own coordinates; canvas viewport clipping and
// camera re-projection only make sense on monitor-sized targets.
static bool drawsOffscreenCanvas(const Render::CRenderContext& ctx) {
    const auto MONITOR = ctx.m_data.pMonitor.lock();
    return ctx.m_data.renderingTransformedSource || (MONITOR && ctx.m_data.fbSize.x > 0 && ctx.m_data.fbSize != MONITOR->m_transformedSize);
}

static void hkDrawElement(void* thisptr, Render::CRenderContext& ctx, WP<IPassElement> element, const CRegion& damage) {
    const auto  ORIGINAL = original<origDrawElement>(HOOK_DRAW_ELEMENT);
    const auto* clip     = OverviewRender::canvasViewportClip;
    // Inside a transformed (e.g. wobbling) window's offscreen canvas the
    // coordinates are that canvas's, not the screen's: leave it to Hyprland.
    // Empty damage means "the whole target" to Hyprland (e.g. clearing a
    // transformed window's canvas); clipping it would skip the draw.
    if (!renderingOverview || !clip || !element || damage.empty() || drawsOffscreenCanvas(ctx)) {
        ORIGINAL(thisptr, ctx, element, damage);
        return;
    }

    // Live blur can expand a pass's damage. Bound every primitive again at
    // draw time, so surfaces, borders and shadows share one finite viewport.
    const auto  clippedDamage = damage.copy().intersect(*clip);
    const auto  texture       = element->type() == EK_TEXTURE ? Hyprutils::Memory::dynamicPointerCast<CTexPassElement>(element) : WP<CTexPassElement>{};
    const auto& modif         = ctx.m_data.renderModif;
    const bool  cameraSurface = texture && texture->m_data.surface && ctx.m_data.currentWindow && modif.enabled && !modif.modifs.empty();
    // Surface damage is still in source coordinates here. The texture hook
    // projects it through the camera before deciding whether it is visible.
    if (clippedDamage.empty() && element->type() != EK_HINTS && !cameraSurface)
        return;
    const auto previousDamage = ctx.m_data.damage.copy();
    ctx.m_data.damage         = previousDamage.copy().intersect(*clip);
    auto restore              = Hyprutils::Utils::CScopeGuard([&ctx, &previousDamage] { ctx.m_data.damage = previousDamage; });
    ORIGINAL(thisptr, ctx, element, clippedDamage);
}

static void hkDrawTex(void* thisptr, Render::CRenderContext& ctx, WP<CTexPassElement> element, const CRegion& damage) {
    const auto ORIGINAL    = original<origDrawTex>(HOOK_DRAW_TEX);
    auto&      renderModif = ctx.m_data.renderModif;
    if (!renderingOverview || !element || !renderModif.enabled || renderModif.modifs.empty() || drawsOffscreenCanvas(ctx)) {
        ORIGINAL(thisptr, ctx, element, damage);
        return;
    }

    // Hyprland applies renderModif to the texture box, but its texture path
    // consumes clipRegion and clipBox as already-transformed framebuffer
    // coordinates. Keep those masks attached to camera-transformed surfaces.
    const auto previousElementClipRegion = element->m_data.clipRegion.copy();
    const auto previousElementClipBox    = element->m_data.clipBox;
    const auto previousRendererClipBox   = ctx.m_data.clipBox;

    auto       transformedDamage = damage.copy();
    if (ctx.m_data.currentWindow && element->m_data.surface) {
        // The backing workspace's visibility calculation may expose only the
        // part of a window that intersected its original monitor. Canvas mode
        // deliberately renders that window elsewhere, so use the complete
        // transformed surface as its authoritative clip and damage region.
        auto transformedSurfaceBox = element->m_data.box;
        renderModif.applyToBox(transformedSurfaceBox);
        transformedSurfaceBox.round();
        element->m_data.clipRegion = CRegion{transformedSurfaceBox};
        element->m_data.clipBox    = {};
        ctx.m_data.clipBox         = {};
        transformedDamage          = CRegion{transformedSurfaceBox};
    } else {
        if (!element->m_data.clipRegion.empty())
            renderModif.applyToRegion(element->m_data.clipRegion);
        if (!element->m_data.clipBox.empty())
            renderModif.applyToBox(element->m_data.clipBox);
        else if (!ctx.m_data.clipBox.empty())
            renderModif.applyToBox(ctx.m_data.clipBox);
    }

    if (const auto* clip = OverviewRender::canvasViewportClip) {
        transformedDamage.intersect(*clip);
        element->m_data.clipRegion.intersect(*clip);
        ctx.m_data.clipBox = *clip;
    }

    ORIGINAL(thisptr, ctx, element, transformedDamage);

    element->m_data.clipRegion = previousElementClipRegion;
    element->m_data.clipBox    = previousElementClipBox;
    ctx.m_data.clipBox         = previousRendererClipBox;
}

static void hkElementDrawTex(void* thisptr, Render::CRenderContext& ctx, WP<CTexPassElement> element, const CRegion& damage) {
    const auto ORIGINAL    = original<origDrawTex>(HOOK_ELEMENT_DRAW_TEX);
    auto&      renderModif = ctx.m_data.renderModif;
    if (!renderingOverview || !element || !renderModif.enabled || renderModif.modifs.empty() || !ctx.m_data.currentWindow || !element->m_data.surface ||
        drawsOffscreenCanvas(ctx)) {
        ORIGINAL(thisptr, ctx, element, damage);
        return;
    }

    // SurfacePassElement and the blur path both inspect the texture box before
    // CGLElementRenderer::draw (and therefore hkDrawTex) is reached. A window
    // shown through another monitor's canvas camera can otherwise be rejected
    // at its source coordinates, leaving only plugin-rendered decorations.
    // Materialize the camera transform for this texture while it passes through
    // that early stage, then suppress the later transform to avoid applying it
    // twice. All queued element state is restored synchronously afterwards.
    const auto previousRenderModif       = renderModif;
    const auto previousElementBox        = element->m_data.box;
    const auto previousElementClipRegion = element->m_data.clipRegion.copy();
    const auto previousElementClipBox    = element->m_data.clipBox;
    const auto previousRendererClipBox   = ctx.m_data.clipBox;

    renderModif.applyToBox(element->m_data.box);
    element->m_data.box.round();
    if (!element->m_data.clipRegion.empty())
        renderModif.applyToRegion(element->m_data.clipRegion);
    if (!element->m_data.clipBox.empty())
        renderModif.applyToBox(element->m_data.clipBox);

    auto transformedDamage = CRegion{element->m_data.box};
    if (const auto* clip = OverviewRender::canvasViewportClip) {
        transformedDamage.intersect(*clip);
        element->m_data.clipRegion = transformedDamage;
        element->m_data.clipBox    = *clip;
    }
    ctx.m_data.clipBox = {};
    renderModif        = {};

    ORIGINAL(thisptr, ctx, element, transformedDamage);

    renderModif                = previousRenderModif;
    element->m_data.box        = previousElementBox;
    element->m_data.clipRegion = previousElementClipRegion;
    element->m_data.clipBox    = previousElementClipBox;
    ctx.m_data.clipBox         = previousRendererClipBox;
}

// Title-bar drags of apps that draw their own (xdg move/resize requests) run
// as canvas drags; see CScrollOverview::beginClientWindowGesture. Hyprland
// inlines the request handlers, so the drag start is where to catch them.
static void hkBeginDragTarget(void* thisptr, SP<Layout::ITarget> target, eMouseBindMode mode, std::optional<Layout::eRectCorner> edge, bool exclusiveGrab) {
    if (target && target->window()) {
        const bool MOVE   = mode == MBIND_MOVE;
        const bool RESIZE = mode == MBIND_RESIZE && edge && *edge != Layout::CORNER_NONE;
        if ((MOVE || RESIZE) && canvasTakeClientWindowGesture(target->window(), RESIZE ? edge : std::nullopt))
            return;
    }
    original<origBeginDragTarget>(HOOK_BEGIN_DRAG_TARGET)(thisptr, target, mode, edge, exclusiveGrab);
}

// X11 windows report where the canvas draws them; see canvasX11Configure.
static void hkX11Configure(void* thisptr, const CBox& box) {
    original<origX11Configure>(HOOK_X11_CONFIGURE)(thisptr, canvasX11Configure(thisptr, box));
}

// An X11 app asks to move. 0.57 hands CWindow the request in logical
// coordinates; canvasX11Request keeps its offsets in the X11 client space the
// configure hook reports in, so convert there and back.
static void hkConfigureRequest(void* thisptr, const CBox& logicalBox) {
    const auto ORIGINAL = original<origConfigureRequest>(HOOK_X11_CONFIGURE_REQUEST);
    auto*      window   = sc<Desktop::View::CWindow*>(thisptr);
    if (!window || !window->backend().isX11()) {
        ORIGINAL(thisptr, logicalBox);
        return;
    }

    const auto MONITOR   = window->m_monitor.lock();
    const auto CLIENTBOX = window->backend().logicalToClient(logicalBox, MONITOR);
    const auto CANVASBOX = canvasX11Request(thisptr, CLIENTBOX);
    ORIGINAL(thisptr, CANVASBOX == CLIENTBOX ? logicalBox : window->backend().clientToLogical(CANVASBOX, MONITOR));
}

// Hyprland re-applies an X11 app's last fullscreen request on every
// _NET_WM_STATE message, including ones about something else: a game
// flashing for attention when combat starts would drop out of the fullscreen
// the user chose with SUPER + F. Only messages about fullscreen may change it.
// (Not tied to the canvas: always hooked.)
static void hkX11ClientMessage(void* thisptr, xcb_client_message_event_t* e) {
    const auto CALL       = [&] { original<origX11ClientMessage>(HOOK_X11_CLIENT_MESSAGE)(thisptr, e); };
    const auto NETWMSTATE = HYPRATOMS.find("_NET_WM_STATE");
    const auto FULLSCREEN = HYPRATOMS.find("_NET_WM_STATE_FULLSCREEN");
    if (!e || e->format != 32 || NETWMSTATE == HYPRATOMS.end() || FULLSCREEN == HYPRATOMS.end() || !FULLSCREEN->second || e->type != NETWMSTATE->second ||
        e->data.data32[1] == FULLSCREEN->second || e->data.data32[2] == FULLSCREEN->second)
        return CALL();

    SP<CXWaylandSurface> surface;
    for (const auto& window : Desktop::windowState()->windows()) {
        if (const auto X11SURFACE = windowX11Surface(window); X11SURFACE && X11SURFACE->m_xID == e->window) {
            surface = X11SURFACE;
            break;
        }
    }
    if (!surface)
        return CALL();

    const auto REQUESTED = surface->m_state.requestsFullscreen;
    surface->m_state.requestsFullscreen.reset();
    CALL();
    surface->m_state.requestsFullscreen = REQUESTED;
}

static void hkPopupReposition(void* thisptr) {
    if (!canvasRepositionPopup(rc<Desktop::View::CPopup*>(thisptr)))
        original<origPopupReposition>(HOOK_POPUP_REPOSITION)(thisptr);
}

static bool shouldSuppressMonitorDamage(Monitor::CMonitor* monitor, const SP<IOverview>& overview) {
    return overview && renderingOverviewMonitor.lock() == monitor->m_self.lock() && !damageFromSurface && overview->shouldSuppressRenderDamage();
}

static void hkAddDamageBox(void* thisptr, const CBox& box) {
    const auto PMONITOR = sc<Monitor::CMonitor*>(thisptr);
    const auto OVERVIEW = scrollOverviewForMonitor(PMONITOR->m_self.lock());
    if (shouldSuppressMonitorDamage(PMONITOR, OVERVIEW))
        return;
    if (OVERVIEW && !OVERVIEW->blockDamageReporting && !damageFromSurface)
        OVERVIEW->onDamageReported();
    original<origAddDamageBox>(HOOK_ADD_DAMAGE_BOX)(thisptr, box);
}

static void hkAddDamageRegion(void* thisptr, const pixman_region32_t* rg) {
    const auto PMONITOR = sc<Monitor::CMonitor*>(thisptr);
    const auto OVERVIEW = scrollOverviewForMonitor(PMONITOR->m_self.lock());
    if (shouldSuppressMonitorDamage(PMONITOR, OVERVIEW))
        return;
    if (OVERVIEW && !OVERVIEW->blockDamageReporting && !damageFromSurface)
        OVERVIEW->onDamageReported();
    original<origAddDamageRegion>(HOOK_ADD_DAMAGE_REGION)(thisptr, rg);
}

// How each hook finds its target: the owning class and method name, tokens
// that must appear in the demangled parameter list, and the parameter count,
// which separates overloads such as renderWorkspaceWindows(..., eSceneMode).
struct SFunctionQuery {
    std::string_view              owner;
    std::string_view              name;
    std::vector<std::string_view> params;
    size_t                        paramCount = 0;
};

struct SHookSpec {
    SFunctionQuery query;
    void*          handler      = nullptr;
    bool           overviewOnly = true; // enabled only while an overview exists
};

static const std::array<SHookSpec, HOOK_COUNT> SCROLL_OVERVIEW_HOOKS = {{
    /* HOOK_RENDER_WORKSPACE */ {{"IHyprRenderer", "renderWorkspace", {"CRenderContext", "CMonitor", "CHLWorkspace", "CBox"}, 5}, rc<void*>(hkRenderWorkspace)},
    /* HOOK_RENDER_WINDOW */ {{"IHyprRenderer", "renderWindow", {"CRenderContext", "CWindow", "SWindowRenderPresentation"}, 9}, rc<void*>(hkRenderWindow)},
    /* HOOK_WORKSPACE_WINDOWS */ {{"IHyprRenderer", "renderWorkspaceWindows", {"CRenderContext", "eSceneMode"}, 5}, rc<void*>(hkWorkspaceWindows)},
    /* HOOK_WORKSPACE_FULLSCREEN */ {{"IHyprRenderer", "renderWorkspaceWindowsFullscreen", {"CRenderContext", "eSceneMode"}, 5}, rc<void*>(hkWorkspaceFullscreen)},
    /* HOOK_SCHEDULE_FRAME */ {{"CMonitor", "scheduleFrame", {"scheduleFrameReason"}, 1}, rc<void*>(hkScheduleFrame)},
    /* HOOK_DAMAGE_SURFACE */ {{"IHyprRenderer", "damageSurface", {"CWLSurfaceResource"}, 4}, rc<void*>(hkDamageSurface)},
    /* HOOK_SEND_FRAME_EVENTS */ {{"IHyprRenderer", "sendFrameEventsToWorkspace", {"CMonitor", "CHLWorkspace"}, 3}, rc<void*>(hkSendFrameEventsToWorkspace)},
    /* HOOK_SURFACE_FRAME */ {{"CWLSurfaceResource", "frame", {"time_point"}, 1}, rc<void*>(hkSurfaceFrame)},
    /* HOOK_ADD_DAMAGE_BOX */ {{"CMonitor", "addDamage", {"CBox"}, 1}, rc<void*>(hkAddDamageBox)},
    /* HOOK_ADD_DAMAGE_REGION */ {{"CMonitor", "addDamage", {"pixman_region32"}, 1}, rc<void*>(hkAddDamageRegion)},
    /* HOOK_DRAW_TEX */ {{"CGLElementRenderer", "draw", {"CRenderContext", "CTexPassElement"}, 3}, rc<void*>(hkDrawTex)},
    /* HOOK_ELEMENT_DRAW_TEX */ {{"IElementRenderer", "drawTex", {"CRenderContext", "CTexPassElement"}, 3}, rc<void*>(hkElementDrawTex)},
    /* HOOK_DRAW_ELEMENT */ {{"IElementRenderer", "drawElement", {"CRenderContext", "IPassElement"}, 3}, rc<void*>(hkDrawElement)},
    /* HOOK_POPUP_REPOSITION */ {{"CPopup", "reposition", {}, 0}, rc<void*>(hkPopupReposition)},
    /* HOOK_BEGIN_DRAG_TARGET */ {{"CLayoutManager", "beginDragTarget", {"ITarget"}, 4}, rc<void*>(hkBeginDragTarget)},
    /* HOOK_X11_CONFIGURE */ {{"CXWaylandSurface", "configure", {"CBox"}, 1}, rc<void*>(hkX11Configure)},
    /* HOOK_X11_CONFIGURE_REQUEST */ {{"CWindow", "onConfigureRequest", {"CBox"}, 1}, rc<void*>(hkConfigureRequest)},
    /* HOOK_X11_CLIENT_MESSAGE */ {{"CXWM", "handleClientMessage", {"xcb_client_message_event_t"}, 1}, rc<void*>(hkX11ClientMessage), false},
}};

// Top-level parameters of a demangled signature, e.g. "A::f(int, B<C, D>)".
static std::optional<std::vector<std::string_view>> demangledParams(std::string_view demangled, std::string_view qualifiedName) {
    const auto START = demangled.find(qualifiedName);
    if (START == std::string_view::npos)
        return std::nullopt;

    std::vector<std::string_view> params;
    size_t                        depth = 0, from = START + qualifiedName.size();
    for (size_t i = from; i < demangled.size(); ++i) {
        const char C = demangled[i];
        if (C == '<' || C == '(')
            ++depth;
        else if ((C == '>' || C == ')') && depth > 0)
            --depth;
        else if (C == ')' || (C == ',' && depth == 0)) {
            const auto PARAM = demangled.substr(from, i - from);
            if (PARAM.find_first_not_of(' ') != std::string_view::npos)
                params.push_back(PARAM);
            if (C == ')')
                return params;
            from = i + 1;
        }
    }
    return std::nullopt;
}

static void* findFunction(const SFunctionQuery& query) {
    const auto QUALIFIED = std::format("{}::{}(", query.owner, query.name);
    void*      found     = nullptr;
    size_t     matches   = 0;
    for (const auto& fn : HyprlandAPI::findFunctionsByName(SCROLLOVERVIEW_HANDLE, std::string{query.name})) {
        const auto PARAMS = demangledParams(fn.demangled, QUALIFIED);
        if (!PARAMS || PARAMS->size() != query.paramCount)
            continue;
        const auto HASTOKEN = [&PARAMS](std::string_view token) {
            return std::ranges::any_of(*PARAMS, [token](std::string_view param) { return param.find(token) != std::string_view::npos; });
        };
        if (!std::ranges::all_of(query.params, HASTOKEN))
            continue;
        found = fn.address;
        ++matches;
    }

    if (matches != 1) {
        const auto REASON = std::format("{} {} for hook {}", matches, matches ? "matches" : "no match", QUALIFIED);
        failNotif(REASON);
        throw std::runtime_error(std::format("[spatialoverview] {}", REASON));
    }
    return found;
}

bool ensureScrollOverviewHooks() {
    if (g_scrollOverviewHooksActive)
        return true;

    for (size_t i = 0; i < HOOK_COUNT; ++i) {
        if (!SCROLL_OVERVIEW_HOOKS[i].overviewOnly || (g_hooks[i] && g_hooks[i]->hook()))
            continue;

        const auto& QUERY = SCROLL_OVERVIEW_HOOKS[i].query;
        Log::logger->log(Log::ERR, "spatialoverview", "failed to enable overview hook: {}::{}", QUERY.owner, QUERY.name);
        disableScrollOverviewHooks();
        failNotif(std::format("Failed enabling overview hook: {}::{}", QUERY.owner, QUERY.name));
        return false;
    }

    g_scrollOverviewHooksActive = true;
    return true;
}

void disableScrollOverviewHooks() {
    for (size_t i = 0; i < HOOK_COUNT; ++i) {
        if (SCROLL_OVERVIEW_HOOKS[i].overviewOnly && g_hooks[i])
            g_hooks[i]->unhook();
    }

    g_scrollOverviewHooksActive = false;
}

// Resolves every hook target. Overview hooks are enabled lazily by
// ensureScrollOverviewHooks(); the rest are enabled here.
static void createScrollOverviewHooks() {
    for (size_t i = 0; i < HOOK_COUNT; ++i) {
        const auto& SPEC = SCROLL_OVERVIEW_HOOKS[i];
        g_hooks[i]       = HyprlandAPI::createFunctionHook(SCROLLOVERVIEW_HANDLE, findFunction(SPEC.query), SPEC.handler);
        if (!SPEC.overviewOnly && (!g_hooks[i] || !g_hooks[i]->hook()))
            Log::logger->log(Log::WARN, "spatialoverview", "could not enable hook {}::{}", SPEC.query.owner, SPEC.query.name);
    }
}

static std::pair<std::string, std::string> splitOverviewArg(const std::string& arg) {
    const auto FIRST = arg.find_first_not_of(" \t");
    if (FIRST == std::string::npos)
        return {"on", ""};

    const auto SEPARATOR = arg.find_first_of(" \t", FIRST);
    if (SEPARATOR == std::string::npos)
        return {arg.substr(FIRST), ""};

    const auto TARGET = arg.find_first_not_of(" \t", SEPARATOR);
    if (TARGET == std::string::npos)
        return {arg.substr(FIRST, SEPARATOR - FIRST), ""};

    const auto LAST = arg.find_last_not_of(" \t");
    return {arg.substr(FIRST, SEPARATOR - FIRST), arg.substr(TARGET, LAST - TARGET + 1)};
}

static std::vector<PHLMONITOR> overviewTargetMonitors(const std::string& target) {
    if (target == "all")
        return State::monitorState()->monitors();

    if (!target.empty()) {
        const auto& MONITORS = State::monitorState()->monitors();
        const auto  IT       = std::ranges::find_if(MONITORS, [&target](const auto& monitor) { return monitor && monitor->m_name == target; });
        return IT == MONITORS.end() ? std::vector<PHLMONITOR>{} : std::vector<PHLMONITOR>{*IT};
    }

    const auto MONITOR = Desktop::focusState()->monitor();
    return MONITOR ? std::vector<PHLMONITOR>{MONITOR} : std::vector<PHLMONITOR>{};
}

// 0.57 does not say which bind is running, but it does keep the inputs held
// down: a mouse button held means a mouse bind, which acts on the overview under
// the pointer. Keyboard binds act on the focused screen's overview.
// ponytail: wheel binds are momentary and count as keyboard binds here.
static bool mouseBindRunning() {
    if (!Keybinds::mgr())
        return false;
    return std::ranges::any_of(Keybinds::mgr()->inputState().pressed(),
                               [](const Keybinds::SPressedInput& input) { return input.key.event && input.key.event->starts_with("mouse"); });
}

static SP<IOverview> dispatcherOverview() {
    if (mouseBindRunning() && g_pInputManager)
        return scrollOverviewAt(g_pInputManager->getMouseCoordsInternal());

    return activeScrollOverview();
}

void        canvasReclaimScreen(const PHLMONITOR& monitor); // scrollOverview.cpp

static bool openOverview(PHLMONITOR monitor, bool workspaceBoundCanvas = false) {
    if (!monitor || scrollOverviewForMonitor(monitor))
        return true;

    if (!ensureScrollOverviewHooks())
        return false;

    const bool PREVRENDERINGOVERVIEW = renderingOverview;
    renderingOverview                = true;
    if (const auto retained = workspaceBoundCanvas ? workspaceTransitionOverviewFor(monitor, monitor->m_activeWorkspace) : SP<IOverview>{}) {
        registerScrollOverview(retained);
        static_cast<CScrollOverview*>(retained.get())->resumeAfterWorkspaceSwitch();
        Log::logger->log(Log::DEBUG, "spatialoverview", "resumed workspace Canvas {}", workspaceID(monitor->m_activeWorkspace));
    } else {
        auto overview = makeShared<CScrollOverview>(monitor->m_activeWorkspace, false, monitor, workspaceBoundCanvas);
        registerScrollOverview(overview);
        if (workspaceBoundCanvas)
            overview->restoreWorkspaceOverview();
        if (workspaceBoundCanvas)
            Log::logger->log(Log::DEBUG, "spatialoverview", "created workspace Canvas {}", workspaceID(monitor->m_activeWorkspace));
    }
    renderingOverview = PREVRENDERINGOVERVIEW;
    canvasReclaimScreen(monitor);
    return true;
}

bool openCanvasOverview(PHLMONITOR monitor) {
    const auto WORKSPACE = monitor ? monitor->m_activeWorkspace : PHLWORKSPACE{};
    if (ScrollOverview::Config::getCanvasWorkspaceBound())
        return WORKSPACE && SpatialOverview::workspaceCanvasEnabled(WORKSPACE) && openOverview(monitor, true);
    return openOverview(monitor, false);
}

static SDispatchResult openWorkspaceCanvas(PHLMONITOR monitor, PHLWORKSPACE workspace) {
    if (!monitor || !workspace || monitor->m_activeWorkspace != workspace)
        return {.success = false, .error = "Workspace Canvas requires the monitor's active workspace"};
    const auto FULLSCREEN = Fullscreen::controller()->getFullscreenWindow(workspace);
    if (validMapped(FULLSCREEN) && Fullscreen::controller()->isFullscreen(FULLSCREEN))
        return {}; // Fullscreen suspends the renderer, not the workspace's requested Canvas mode.

    if (const auto existing = scrollOverviewForMonitor(monitor)) {
        auto* canvas = dynamic_cast<CScrollOverview*>(existing.get());
        if (!canvas || !canvas->isWorkspaceBoundCanvas() || canvas->workspaceCanvas() != workspace)
            return {.success = false, .error = "Another overview already owns this workspace's monitor"};
        if (canvas->isClosing()) {
            canvas->reopen();
            if (canvas->isCanvasNavigationActive())
                canvas->toggleCanvasNavigation();
        }
        return {};
    }

    if (!openOverview(monitor, true))
        return {.success = false, .error = "Workspace Canvas failed to enable overview hooks"};
    return {};
}

static void syncWorkspaceCanvasSessions() {
    g_workspaceCanvasSyncIdle = nullptr;
    if (g_unloading)
        return;

    for (const auto& monitor : State::monitorState()->monitors()) {
        if (!monitor || !monitor->m_activeWorkspace)
            continue;

        const auto existing = scrollOverviewForMonitor(monitor);
        auto*      canvas   = existing ? dynamic_cast<CScrollOverview*>(existing.get()) : nullptr;
        if (canvas && canvas->isWorkspaceBoundCanvas() && canvas->workspaceCanvas() != monitor->m_activeWorkspace) {
            retainWorkspaceTransitionOverview(existing);
            canvas->closeForWorkspaceSwitch();
            scheduleWorkspaceCanvasTransitionCleanup();
        } else if (existing) {
            continue; // never replace a regular overview or another plugin's screen
        }

        const auto FULLSCREEN = Fullscreen::controller()->getFullscreenWindow(monitor->m_activeWorkspace);
        if (validMapped(FULLSCREEN) && Fullscreen::controller()->isFullscreen(FULLSCREEN))
            continue; // a fullscreen client temporarily owns this workspace's output

        if (SpatialOverview::workspaceCanvasEnabled(monitor->m_activeWorkspace))
            (void)openWorkspaceCanvas(monitor, monitor->m_activeWorkspace);
    }
}

static void scheduleWorkspaceCanvasSync() {
    if (g_workspaceCanvasSyncIdle || !g_pCompositor || g_unloading)
        return;
    g_workspaceCanvasSyncIdle = wl_event_loop_add_idle(g_pCompositor->m_wlEventLoop, [](void*) { syncWorkspaceCanvasSessions(); }, nullptr);
}

void                              canvasFullscreenEvent(PHLWINDOW window); // scrollOverview.cpp
bool                              canvasPointerMoved();
std::string                       canvasStateJson();
static SP<IPC::Socket1::SCommand> g_stateCommand;
void                              canvasFullscreenReset();
bool                              canvasToggleFill(PHLWINDOW window);
bool                              canvasTogglePin(PHLWINDOW window);

static SDispatchResult            onOverviewDispatcher(std::string arg) {
    const auto [ACTION, TARGET] = splitOverviewArg(arg);

    if (ACTION == "select") {
        const auto OVERVIEW = scrollOverviewAt(g_pInputManager->getMouseCoordsInternal());
        if (OVERVIEW && OVERVIEW->m_isSwiping)
            return {.success = false, .error = "already swiping"};
        if (OVERVIEW)
            OVERVIEW->selectHoveredWorkspace();
        return {};
    }

    const auto ACTIVE = dispatcherOverview();

    if (ACTIVE && ACTIVE->m_isSwiping)
        return {.success = false, .error = "already swiping"};

    if (ACTION == "off" || ACTION == "close" || ACTION == "disable") {
        if (TARGET.empty() || TARGET == "all") {
            closeAll();
            return {};
        }

        const auto MONITORS = overviewTargetMonitors(TARGET);
        if (MONITORS.empty())
            return {.success = false, .error = "monitor not found: " + TARGET};
        if (const auto overview = scrollOverviewForMonitor(MONITORS.front()))
            overview->close();
        return {};
    }

    if (ACTION != "toggle" && ACTION != "on" && ACTION != "open" && ACTION != "enable")
        return {.success = false, .error = "invalid arg. expected toggle|open|close|select [monitor|all]"};

    const auto MONITORS = overviewTargetMonitors(TARGET);
    if (MONITORS.empty())
        return {.success = false, .error = TARGET.empty() ? "no active monitor" : "monitor not found: " + TARGET};

    const bool ALL_OPEN = std::ranges::all_of(MONITORS, [](const auto& monitor) { return !!scrollOverviewForMonitor(monitor); });
    if (ACTION == "toggle" && ALL_OPEN) {
        for (const auto& monitor : MONITORS) {
            if (const auto overview = scrollOverviewForMonitor(monitor)) {
                if (overview->isClosing())
                    overview->reopen();
                else if (auto* canvas = dynamic_cast<CScrollOverview*>(overview.get()); canvas && canvas->isPersistentCanvas())
                    canvas->toggleCanvasNavigation(true);
                else
                    overview->close();
            }
        }
        return {};
    }

    for (const auto& monitor : MONITORS) {
        if (!openOverview(monitor))
            return {.success = false, .error = "failed enabling overview hooks (is other overview plugin enabled?)"};
    }

    // A persistent canvas opens at 100%. When the toggle key created it, the
    // user asked to see the map, so continue straight into navigation.
    if (ACTION == "toggle") {
        for (const auto& monitor : MONITORS) {
            auto* canvas = dynamic_cast<CScrollOverview*>(scrollOverviewForMonitor(monitor).get());
            if (canvas && canvas->isPersistentCanvas() && !canvas->isCanvasNavigationActive() && !canvas->isClosing())
                canvas->toggleCanvasNavigation(true);
        }
    }
    return {};
}

static SDispatchResult onNavigateDispatcher(std::string arg) {
    // Window ownership is only a surface-storage detail in the shared canvas.
    // Route arrows through the last camera the user actually interacted with,
    // and keep that output locked for the duration of the Super key hold.
    const auto OVERVIEW = canvasNavigationOverview();

    if (!OVERVIEW)
        return {};

    if (arg != "left" && arg != "right" && arg != "up" && arg != "down")
        return {.success = false, .error = "invalid arg. expected left|right|up|down"};

    OVERVIEW->moveSelection(arg);
    return {};
}

static SDispatchResult onWindowDispatcher(std::string arg) {
    const auto OVERVIEW = scrollOverviewAt(g_pInputManager->getMouseCoordsInternal());
    if (!OVERVIEW)
        return {};

    if (arg != "select" && arg != "close")
        return {.success = false, .error = "invalid arg. expected select|close"};

    OVERVIEW->windowDispatcherAction(arg);
    return {};
}

static PHLWINDOWREF                g_flightDeckNativeWindow;
static Fullscreen::SFullscreenMode g_flightDeckPreviousModes;
static bool                        g_flightDeckNativePending = false;
struct SFlightDeckRequest {
    PHLWINDOWREF                window;
    Fullscreen::eFullscreenMode mode;
};
static wl_event_source*                    g_flightDeckIdle = nullptr;
static std::unique_ptr<SFlightDeckRequest> g_flightDeckRequest;

void                                       requestFlightDeckNative(PHLWINDOW window, Fullscreen::eFullscreenMode mode) {
    if (g_flightDeckNativePending)
        return;
    g_flightDeckNativePending = true;
    g_flightDeckRequest       = std::make_unique<SFlightDeckRequest>(window, mode);
    g_flightDeckIdle          = wl_event_loop_add_idle(
        g_pCompositor->m_wlEventLoop,
        [](void*) {
            g_flightDeckIdle   = nullptr;
            const auto request = std::move(g_flightDeckRequest);
            if (g_workspaceCanvasTransitionTimer)
                wl_event_source_remove(g_workspaceCanvasTransitionTimer);
            g_workspaceCanvasTransitionTimer = nullptr;
            clearScrollOverviews();
            clearWorkspaceTransitionOverviews();
            disableScrollOverviewHooks();
            const auto w = request->window.lock();
            if (validMapped(w)) {
                g_flightDeckNativeWindow  = w;
                g_flightDeckPreviousModes = Fullscreen::controller()->getFullscreenModes(w);
                Desktop::focusState()->fullWindowFocus(w, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
                if (request->mode != Fullscreen::FSMODE_NONE)
                    Fullscreen::controller()->setFullscreenMode(w, request->mode, request->mode);
            }
            g_flightDeckNativePending = false;
        },
        nullptr);
    if (!g_flightDeckIdle) {
        g_flightDeckRequest.reset();
        g_flightDeckNativePending = false;
    }
}

static SDispatchResult setWorkspaceCanvasMode(const PHLMONITOR& monitor, const PHLWORKSPACE& workspace, bool enabled) {
    if (enabled && (!ScrollOverview::Config::getCanvasEnabled() || !ScrollOverview::Config::getCanvasDesktopMode() || !ScrollOverview::Config::getCanvasWorkspaceBound()))
        return {.success = false, .error = "Enable canvas.enabled, canvas.desktop_mode, and canvas.workspace_bound first"};
    if (enabled && (ScrollOverview::Config::getCanvasLinkedScreens() || ScrollOverview::Config::getCanvasPlaces()))
        return {.success = false, .error = "Workspace-bound Canvas requires linked_screens=false and places=false"};

    const auto EXISTING = scrollOverviewForMonitor(monitor);
    if (enabled && EXISTING) {
        const auto* CANVAS = dynamic_cast<const CScrollOverview*>(EXISTING.get());
        if (!CANVAS || !CANVAS->isWorkspaceBoundCanvas() || CANVAS->workspaceCanvas() != workspace)
            return {.success = false, .error = "Close the current overview before enabling workspace Canvas"};
    }

    SpatialOverview::setWorkspaceCanvasEnabled(workspace, enabled);
    if (enabled) {
        const auto RESULT = openWorkspaceCanvas(monitor, workspace);
        if (RESULT.success) {
            if (const auto OVERVIEW = scrollOverviewForMonitor(monitor)) {
                auto* CANVAS = dynamic_cast<CScrollOverview*>(OVERVIEW.get());
                if (CANVAS && CANVAS->isWorkspaceBoundCanvas() && CANVAS->workspaceCanvas() == workspace)
                    CANVAS->followCanvasWindow(Desktop::focusState()->window(), true, true);
            }
            return RESULT;
        }
        SpatialOverview::setWorkspaceCanvasEnabled(workspace, false);
        return RESULT;
    }

    auto* CANVAS = EXISTING ? dynamic_cast<CScrollOverview*>(EXISTING.get()) : nullptr;
    if (CANVAS && CANVAS->isWorkspaceBoundCanvas() && CANVAS->workspaceCanvas() == workspace)
        CANVAS->close();
    else
        SpatialOverview::restoreWorkspaceCanvasNative(workspace);
    return {};
}

static SDispatchResult toggleWorkspaceCanvasHud(const PHLMONITOR& monitor, const PHLWORKSPACE& workspace) {
    const auto OVERVIEW = monitor ? scrollOverviewForMonitor(monitor) : SP<IOverview>{};
    auto*      CANVAS   = OVERVIEW ? dynamic_cast<CScrollOverview*>(OVERVIEW.get()) : nullptr;
    if (!CANVAS || !workspace || CANVAS->isClosing() || !CANVAS->isWorkspaceBoundCanvas() || CANVAS->workspaceCanvas() != workspace ||
        !SpatialOverview::workspaceCanvasEnabled(workspace))
        return {};
    CANVAS->toggleCanvasNavigation(true);
    return {};
}

static SDispatchResult onWorkspaceCanvasDispatcher(const std::string& arg) {
    const auto MONITOR = Desktop::focusState()->monitor();
    if (MONITOR && MONITOR->m_activeSpecialWorkspace)
        return {}; // A special workspace owns input; do not toggle the workspace underneath it.
    const auto WORKSPACE = MONITOR ? MONITOR->m_activeWorkspace : PHLWORKSPACE{};
    if (arg == "hud toggle")
        return toggleWorkspaceCanvasHud(MONITOR, WORKSPACE);
    if (!WORKSPACE)
        return {.success = false, .error = "No active workspace"};

    const bool ENABLED = SpatialOverview::workspaceCanvasEnabled(WORKSPACE);
    const bool WANT    = arg == "workspace toggle" ? !ENABLED : arg == "workspace open";
    return setWorkspaceCanvasMode(MONITOR, WORKSPACE, WANT);
}

static SDispatchResult onCanvasDispatcher(std::string arg) {
    if (arg == "workspace toggle" || arg == "workspace open" || arg == "workspace close" || arg == "hud toggle")
        return onWorkspaceCanvasDispatcher(arg);

    if (arg.starts_with("experiment ")) {
        const auto PREVIOUS = SpatialOverview::Experiments::id();
        const auto NAME     = arg.substr(11);
        if (!SpatialOverview::Experiments::setByName(NAME))
            return {.success = false, .error = "unknown experiment"};
        onCanvasExperimentChanged(PREVIOUS);
        HyprlandAPI::addNotification(SCROLLOVERVIEW_HANDLE, SpatialOverview::Experiments::title() + ". " + SpatialOverview::Experiments::help(), CHyprColor{0.55F, 0.9F, 1.F, 1.F},
                                     7000);
        return {};
    }
    if (arg == "switch next" || arg == "switch prev") {
        auto* CANVAS = dynamic_cast<CScrollOverview*>(dispatcherOverview().get());
        if (!CANVAS)
            return {.success = false, .error = "Open the canvas before using this action"};
        return CANVAS->switchWindow(arg == "switch next" ? 1 : -1) ? SDispatchResult{} : SDispatchResult{.success = false, .error = "No window to switch to"};
    }
    if (arg == "alttab next" || arg == "alttab prev") {
        if (!SpatialOverview::Experiments::on(ECanvasExperiment::AltTab))
            return {.success = false, .error = "Alt-Tab experiment is not selected"};
        const auto OVERVIEW = dispatcherOverview();
        auto*      CANVAS   = OVERVIEW ? dynamic_cast<CScrollOverview*>(OVERVIEW.get()) : nullptr;
        if (!CANVAS)
            return {.success = false, .error = "Open the canvas before using this action"};
        return CANVAS->cycleAltTab(arg == "alttab next" ? 1 : -1) ? SDispatchResult{} : SDispatchResult{.success = false, .error = "No window to cycle"};
    }

    const auto OVERVIEW = dispatcherOverview();
    auto*      CANVAS   = OVERVIEW ? dynamic_cast<CScrollOverview*>(OVERVIEW.get()) : nullptr;
    if (arg == "restore") {
        if (const auto w = g_flightDeckNativeWindow.lock(); validMapped(w))
            Fullscreen::controller()->setFullscreenMode(w, g_flightDeckPreviousModes.internal, g_flightDeckPreviousModes.client);
        return {};
    }
    if (arg == "native" || arg == "maximize" || arg == "fullscreen") {
        requestFlightDeckNative(Desktop::focusState()->window(),
                                arg == "fullscreen"   ? Fullscreen::FSMODE_FULLSCREEN :
                                    arg == "maximize" ? Fullscreen::FSMODE_MAXIMIZED :
                                                        Fullscreen::FSMODE_NONE);
        return {};
    }
    if (arg == "fill")
        return canvasToggleFill(Desktop::focusState()->window()) ? SDispatchResult{} : SDispatchResult{.success = false, .error = "Open the canvas before using this action"};
    if (arg == "pin")
        return canvasTogglePin(Desktop::focusState()->window()) ? SDispatchResult{} : SDispatchResult{.success = false, .error = "Open the canvas before using this action"};
    // For keys that have nothing to do on the canvas (tiling): succeeds, so
    // canvas_or() bindings skip their fallback, only while a canvas is open.
    if (arg == "noop")
        return std::ranges::any_of(scrollOverviews(), [](const auto& overview) { return overview && !overview->isClosing(); }) ?
            SDispatchResult{} :
            SDispatchResult{.success = false, .error = "Open the canvas before using this action"};
    if (!CANVAS)
        return arg == "refresh" ? SDispatchResult{} : SDispatchResult{.success = false, .error = "Open the canvas before using this action"};
    if (arg == "back" || arg == "land" || arg == "frame" || arg == "undo" || arg == "redo" || arg == "fit" || arg == "summon" || arg == "search" || arg == "tune" ||
        arg.starts_with("search ") || arg.starts_with("zoom ") || arg.starts_with("pan ") || arg.starts_with("nudge ") || arg.starts_with("area ") || arg.starts_with("go ") ||
        arg.starts_with("send "))
        return CANVAS->flightDeckAction(arg) ? SDispatchResult{} : SDispatchResult{.success = false, .error = "No matching window, area or undo state"};

    std::istringstream stream{arg};
    std::string        action;
    stream >> action;

    if (action == "refresh") {
        CANVAS->refreshCanvasSettings();
        return {};
    }

    if (action == "arrange") {
        if (!CANVAS->arrangeCanvasWindows())
            return {.success = false, .error = CANVAS->canvasPlacementError().empty() ? "canvas arrangement failed" : CANVAS->canvasPlacementError()};
        return {};
    }

    int column = 0;
    int row    = 0;
    stream >> column >> row;
    if ((action != "place" && action != "viewport") || stream.fail())
        return {.success = false, .error = "expected: place <column> <row> | viewport <x> <y> | refresh | arrange"};

    const bool SUCCESS = action == "place" ? CANVAS->placeWindowOnCanvasCell(column, row) : CANVAS->setCanvasViewport(column, row);
    if (!SUCCESS)
        return {.success = false, .error = CANVAS->canvasPlacementError().empty() ? "canvas action failed" : CANVAS->canvasPlacementError()};

    return {};
}

static void failNotif(const std::string& reason) {
    HyprlandAPI::addNotification(SCROLLOVERVIEW_HANDLE, "[spatialoverview] Failure in initialization: " + reason, CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
}

// shared core: register / unregister the overview trackpad gesture. used by both the hyprlang
// keyword and the Lua `spatialoverview.gesture` function so both configure the same gesture system.
static std::expected<void, std::string> applyOverviewGesture(size_t fingerCount, eTrackpadGestureDirection direction, const std::string& action, Input::ModifierMask modMask,
                                                             float deltaScale, bool disableInhibit) {
    if (fingerCount <= 1 || fingerCount >= 10)
        return std::unexpected(std::format("Invalid value {} for finger count", fingerCount));

    if (direction == TRACKPAD_GESTURE_DIR_NONE)
        return std::unexpected("Invalid direction");

    if (action == "overview")
        return g_pTrackpadGestures->addGesture(makeUnique<COverviewGesture>(), fingerCount, direction, modMask, deltaScale, disableInhibit);

    if (action == "unset")
        return g_pTrackpadGestures->removeGesture(fingerCount, direction, modMask, deltaScale, disableInhibit);

    return std::unexpected(std::format("Invalid gesture: {}", action));
}

// Lua-facing registrar (spatialoverview.gesture). takes the direction/mods as strings and resolves
// them the same way the keyword does, then defers to applyOverviewGesture.
static SDispatchResult onRegisterOverviewGesture(size_t fingerCount, const std::string& directionStr, const std::string& action, const std::string& mods, float deltaScale,
                                                 bool disableInhibit) {
    if (g_unloading)
        return {};

    const auto direction = g_pTrackpadGestures->dirForString(directionStr);
    if (direction == TRACKPAD_GESTURE_DIR_NONE)
        return {.success = false, .error = std::format("Invalid direction: {}", directionStr)};

    Input::ModifierMask modMask = Input::HL_MODIFIER_NONE;
    if (!mods.empty())
        modMask = Keybinds::modMaskFromString(mods);

    const auto res = applyOverviewGesture(fingerCount, direction, action, modMask, std::clamp(deltaScale, 0.1F, 10.F), disableInhibit);
    if (!res)
        return {.success = false, .error = res.error()};

    return {};
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    SCROLLOVERVIEW_HANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        // The usual cause: Hyprland was updated after this build.
        failNotif("this build is for a different Hyprland version (Hyprland was probably updated). Rebuild it: run scripts/install.sh in the "
                  "Phantomat source folder");
        throw std::runtime_error("[he] Version mismatch");
    }

    createScrollOverviewHooks();

    static auto P                   = Event::bus()->m_events.render.pre.listen([](PHLMONITOR monitor) {
        const auto overview = scrollOverviewForMonitor(monitor);
        if (overview)
            overview->onPreRender();
        const auto* canvas = overview ? dynamic_cast<const CScrollOverview*>(overview.get()) : nullptr;
        // This stage precedes beginRender's damage snapshot. A camera view
        // owns the output-sized footprint, not its clients' source rectangles.
        // Damage the current frame only; do not start an idle repaint loop.
        if ((canvas && canvas->isWorkspaceBoundCanvas()) ||
            std::ranges::any_of(workspaceTransitionOverviews(), [&](const auto& outgoing) { return outgoing->pMonitor.lock() == monitor; }))
            monitor->m_damage.damageEntire();
    });
    static auto HUDSTAGE            = Event::bus()->m_events.render.stage.listen([](const Event::SRenderStageEvent& event) {
        if (event.stage == RENDER_BEGIN)
            SpatialOverview::BarrelShader::updateUniforms(0.F);
        if (event.stage == RENDER_LAST_MOMENT && event.context)
            SpatialOverview::BarrelShader::applyPendingHud(event.context->get());
    });
    const auto  dropMonitorOverview = [](PHLMONITOR monitor) {
        g_overviewMonitorTeardown = true;
        unregisterScrollOverviewForMonitor(monitor);
        g_overviewMonitorTeardown = false;
    };
    static auto MONITORREMOVED   = Event::bus()->m_events.monitor.removed.listen(dropMonitorOverview);
    static auto MONITORDESTROYED = Event::bus()->m_events.monitor.destroyMon.listen(dropMonitorOverview);

    SpatialOverview::Experiments::load();

    // Fullscreen on the canvas: only the screen showing the window steps
    // aside, and comes back after (scrollOverview.cpp).
    static auto CANVASFULLSCREEN          = Event::bus()->m_events.window.fullscreen.listen([](PHLWINDOW window) {
        if (!g_unloading) {
            canvasFullscreenEvent(window);
            scheduleWorkspaceCanvasSync();
        }
    });
    static auto CANVASFULLSCREENCLOSE     = Event::bus()->m_events.window.close.listen([](PHLWINDOW) {
        if (!g_unloading) {
            canvasFullscreenEvent(nullptr);
            scheduleWorkspaceCanvasSync();
        }
    });
    static auto CANVASFULLSCREENMOVE      = Event::bus()->m_events.window.moveToWorkspace.listen([](PHLWINDOW, PHLWORKSPACE) {
        if (!g_unloading) {
            canvasFullscreenEvent(nullptr);
            scheduleWorkspaceCanvasSync();
        }
    });
    static auto CANVASFULLSCREENWORKSPACE = Event::bus()->m_events.workspace.active.listen([](PHLWORKSPACE) {
        if (!g_unloading) {
            canvasFullscreenEvent(nullptr);
            scheduleWorkspaceCanvasSync();
        }
    });
    static auto WORKSPACECANVASREMOVED    = Event::bus()->m_events.workspace.removed.listen([](auto workspaceRef) {
        SpatialOverview::forgetWorkspaceCanvas(workspaceRef.lock());
        scheduleWorkspaceCanvasSync();
    });
    // Screens given to a fullscreen window: a game hiding its cursor keeps
    // the pointer on its screen, and the pointer moving onto one focuses its
    // window. Registered before any canvas, so the canvases see the pointer
    // where it stays.
    static auto CANVASFULLSCREENPOINTER = Event::bus()->m_events.input.mouse.move.listen([](Vector2D, Event::SCallbackInfo& info) {
        if (!g_unloading && canvasPointerMoved())
            info.cancelled = true;
    });

    // Recency for the navigator is tracked for the whole session, not only
    // while the canvas is open, so "recent first" means what it says.
    static auto NAVIGATORFOCUS = Event::bus()->m_events.window.active.listen([](PHLWINDOW window, Desktop::eFocusReason) { SpatialOverview::Navigator::noteFocus(window); });
    static auto NAVIGATORCLOSE = Event::bus()->m_events.window.close.listen([](PHLWINDOW window) { SpatialOverview::Navigator::forget(window); });
    SpatialOverview::Navigator::noteFocus(Desktop::focusState()->window());
    SpatialOverview::Navigator::setDamageCallback([] {
        if (const auto overview = activeScrollOverview())
            overview->damage();
    });

    // hyprctl spatialoverview: the canvases' state as JSON, for scripts and tests.
    g_stateCommand = HyprlandAPI::registerHyprCtlCommand(
        SCROLLOVERVIEW_HANDLE,
        IPC::Socket1::SCommand{.name = "spatialoverview", .match = IPC::Socket1::COMMAND_MATCH_EXACT, .handler = [](const IPC::Socket1::SRequest&) -> IPC::Socket1::SResponse {
                                   return canvasStateJson();
                               }});

    ScrollOverview::Config::registerDispatcher("overview", ::onOverviewDispatcher);
    ScrollOverview::Config::registerDispatcher("navigate", ::onNavigateDispatcher);
    ScrollOverview::Config::registerDispatcher("window", ::onWindowDispatcher);
    ScrollOverview::Config::registerDispatcher("canvas", ::onCanvasDispatcher);
    ScrollOverview::Config::registerGesture(::onRegisterOverviewGesture);
    ScrollOverview::Config::registerConfig();

    return {"spatialoverview", "Phantomat: a zoomable, infinite-canvas window manager for Hyprland", "Kaolti, yayuuu, Vaxry", SCROLLOVERVIEW_VERSION};
}

APICALL EXPORT void PLUGIN_EXIT() {
    if (g_flightDeckIdle)
        wl_event_source_remove(g_flightDeckIdle);
    g_flightDeckIdle = nullptr;
    g_flightDeckRequest.reset();
    g_flightDeckNativePending = false;
    if (g_workspaceCanvasSyncIdle)
        wl_event_source_remove(g_workspaceCanvasSyncIdle);
    g_workspaceCanvasSyncIdle = nullptr;
    if (g_workspaceCanvasTransitionTimer)
        wl_event_source_remove(g_workspaceCanvasTransitionTimer);
    g_workspaceCanvasTransitionTimer = nullptr;
    if (g_pHyprRenderer)
        g_pHyprRenderer->context().m_pass.removeAllOfType("CScrollOverviewPassElement");

    g_unloading = true;
    if (g_stateCommand)
        HyprlandAPI::unregisterHyprCtlCommand(SCROLLOVERVIEW_HANDLE, g_stateCommand);
    g_stateCommand.reset();
    clearScrollOverviews();
    clearWorkspaceTransitionOverviews();
    SpatialOverview::restoreWorkspaceCanvasNative();
    canvasFullscreenReset();
    SpatialOverview::clearWorkspaceCanvasModes();
    disableScrollOverviewHooks();
    canvasReleaseX11Windows();

    disarmCanvasTimers();
    SpatialOverview::Navigator::shutdown();
    if (const auto OPENGL = g_pHyprRenderer ? g_pHyprRenderer->glBackend() : WP<Render::GL::CHyprOpenGLImpl>{})
        OPENGL->makeEGLCurrent();
    SpatialOverview::BarrelShader::discardPendingHud();
    SpatialOverview::Hud::shutdown();

    if (g_pTrackpadGestures)
        g_pTrackpadGestures->clearGestures();

    HyprlandAPI::reloadConfig(); // re-adds built-in gestures cleared above
}
