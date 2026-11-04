#pragma once

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowBackend.hpp>
#include <hyprland/src/desktop/view/window/WindowGroupMembership.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>

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
