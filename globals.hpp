#pragma once

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowBackend.hpp>
#include <hyprland/src/desktop/view/window/WindowGroupMembership.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include <hyprland/src/desktop/view/window/X11Backend.hpp>
#include <hyprland/src/desktop/view/window/WindowFullscreenPolicy.hpp>

#include <functional>

inline HANDLE SCROLLOVERVIEW_HANDLE = nullptr;

bool ensureScrollOverviewHooks();
void disableScrollOverviewHooks();

// Hyprland 0.57 workspace IDs are a numbered/special variant. The plugin keys
// per-workspace state by a stable integer: the number, or a negative hash of
// the special workspace's address.
using WORKSPACEID                       = int64_t;
constexpr WORKSPACEID WORKSPACE_INVALID = -1;

inline WORKSPACEID workspaceID(const PHLWORKSPACE& workspace) {
    if (!workspace)
        return WORKSPACE_INVALID;
    if (const auto NUMBER = workspace->numberedID())
        return *NUMBER;
    return -2 - sc<WORKSPACEID>(std::hash<std::string>{}(workspace->addressableName()) & 0x3fffffffffffffffULL);
}

inline bool windowPinned(const PHLWINDOW& window) {
    return window && (window->m_state & Desktop::View::WINDOW_STATE_PINNED);
}

inline bool isSpecialWorkspace(const PHLWORKSPACE& workspace) {
    return workspace && workspace->type() == Workspace::eWorkspaceType::SPECIAL;
}

// Names a private Hyprland member without changing access or guessing layout:
// explicit template instantiation may name it, and the compiler checks the
// member still exists with this type. Declare a tag with a friend
// privateMember(Tag), instantiate SPrivateMember<Tag, &Class::member>, then use
// object.*privateMember(Tag{}).
template <typename TTag, auto MEMBER>
struct SPrivateMember {
    friend constexpr auto privateMember(TTag) {
        return MEMBER;
    }
};

// An X11 window's XWayland surface. CX11Backend keeps it private in 0.57.
struct SX11SurfaceTag {
    friend constexpr auto privateMember(SX11SurfaceTag);
};
template struct SPrivateMember<SX11SurfaceTag, &Desktop::View::CX11Backend::m_xwaylandSurface>;

inline SP<CXWaylandSurface> windowX11Surface(const PHLWINDOW& window) {
    if (!window || !window->backend().isX11())
        return nullptr;
    const auto* BACKEND = dc<const Desktop::View::CX11Backend*>(&window->backend());
    return BACKEND ? (BACKEND->*privateMember(SX11SurfaceTag{})).lock() : nullptr;
}
