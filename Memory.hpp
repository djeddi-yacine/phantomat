#pragma once

#include "globals.hpp"

#include <hyprland/src/helpers/math/Math.hpp>

#include <optional>
#include <string>
#include <unordered_map>

// Canvas cameras remembered across restarts, keyed by canvas scope. Window
// boxes are never remembered: they always come from Hyprland and
// canvas:client_size.
namespace SpatialOverview::Memory {
    std::optional<Vector2D> camera(const std::string& scope);
    void                    save(const std::unordered_map<std::string, Vector2D>& cameras);
    std::string             path();
}
