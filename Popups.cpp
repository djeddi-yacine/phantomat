#define WLR_USE_UNSTABLE

// Popup placement for canvas windows. Hyprland keeps a popup inside the
// monitor that holds its window's real position; on the canvas the window is
// drawn somewhere else (wherever the camera shows it), so menus flipped or
// slid away from their window. Here the popup is kept inside the screen as
// the canvas shows it (see canvasPopupConstraint in scrollOverview.cpp).
//
// This file only reaches into CPopup, whose positioner is private; standard
// headers come first so the access trick below never touches them.
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

#include <hyprland/src/desktop/view/Popup.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/protocols/XDGShell.hpp>

std::optional<CBox> canvasPopupConstraint(const PHLWINDOW& window);

// CPopup::reposition() positions against the monitor; a canvas window's popups
// use the visible canvas box instead. Both members are private in 0.57.
struct SPopupBackendTag {
    friend constexpr auto privateMember(SPopupBackendTag);
};
struct SPopupParentCoordsTag {
    friend constexpr auto privateMember(SPopupParentCoordsTag);
};
template struct SPrivateMember<SPopupBackendTag, &Desktop::View::CPopup::m_backend>;
template struct SPrivateMember<SPopupParentCoordsTag, &Desktop::View::CPopup::t1ParentCoords>;

bool canvasRepositionPopup(Desktop::View::CPopup* popup) {
    const auto& BACKEND = popup ? popup->*privateMember(SPopupBackendTag{}) : SP<Desktop::View::IPopupBackend>{};
    if (!BACKEND || !popup->windowOwner())
        return false;
    const auto CONSTRAINT = canvasPopupConstraint(popup->windowOwner());
    if (!CONSTRAINT)
        return false;
    BACKEND->applyPositioning(*CONSTRAINT, (popup->*privateMember(SPopupParentCoordsTag{}))());
    return true;
}

// Whether any popup of the window is fading in or out.
bool popupTreeFading(const PHLWINDOW& window) {
    if (!window || window->backend().isX11() || !window->popupHead())
        return false;
    bool fading = false;
    window->popupHead()->breadthfirst(
        [&fading](SP<Desktop::View::CPopup> popup, void*) {
            if (popup && popup->alpha().get(Desktop::View::POPUP_ALPHA_FADE)->isBeingAnimated())
                fading = true;
        },
        nullptr);
    return fading;
}
