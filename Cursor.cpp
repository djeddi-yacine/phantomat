#define WLR_USE_UNSTABLE

// Whether the app under the pointer has hidden its cursor. Hyprland keeps
// that in its private input state; this file only reads it. Standard headers
// come first so the access trick below never touches them.
#include <algorithm>
#include <any>
#include <array>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "globals.hpp"
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/math/Math.hpp>

#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>

struct SCursorSurfaceInfoTag {
    friend constexpr auto privateMember(SCursorSurfaceInfoTag);
};
template struct SPrivateMember<SCursorSurfaceInfoTag, &CInputManager::m_cursorSurfaceInfo>;

// No cursor at all, or Wine's way of hiding it: an empty 1x1 image.
bool canvasCursorHidden() {
    const auto& CURSOR = (*g_pInputManager).*privateMember(SCursorSurfaceInfoTag{});
    if (CURSOR.hidden)
        return true;
    const auto SURFACE = CURSOR.wlSurface ? CURSOR.wlSurface->resource() : nullptr;
    return SURFACE && SURFACE->m_current.size.x <= 1.0 && SURFACE->m_current.size.y <= 1.0;
}
