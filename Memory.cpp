#include "Memory.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace SpatialOverview::Memory {

    namespace {
        bool                                      g_loaded = false;
        std::unordered_map<std::string, Vector2D> g_cameras;

        // Hand-edited or damaged files must never feed NaN, infinity or
        // absurd offsets into the camera.
        bool sane(double value) {
            return std::isfinite(value) && std::abs(value) <= 1e6;
        }

        std::string clean(std::string value) {
            std::ranges::replace(value, '\t', ' ');
            std::ranges::replace(value, '\n', ' ');
            std::ranges::replace(value, '\r', ' ');
            return value;
        }

        // Lines other than "camera" (window boxes from older versions) are
        // ignored and dropped on the next save.
        void load() {
            if (g_loaded)
                return;
            g_loaded = true;

            std::ifstream in{path()};
            std::string   line;
            while (in && std::getline(in, line)) {
                std::vector<std::string> fields;
                std::string              field;
                std::istringstream       stream{line};
                while (std::getline(stream, field, '\t'))
                    fields.push_back(field);
                if (fields.size() != 4 || fields[0] != "camera")
                    continue;
                try {
                    const Vector2D CAMERA{std::stod(fields[2]), std::stod(fields[3])};
                    if (sane(CAMERA.x) && sane(CAMERA.y))
                        g_cameras[fields[1]] = CAMERA;
                } catch (...) {
                    // A damaged line only costs that one camera.
                }
            }
        }
    }

    std::string path() {
        std::filesystem::path base;
        if (const char* state = std::getenv("XDG_STATE_HOME"); state && *state)
            base = state;
        else
            base = std::filesystem::path{std::getenv("HOME") ? std::getenv("HOME") : "/tmp"} / ".local" / "state";
        return (base / "spatial-overview" / "canvas-memory.tsv").string();
    }

    std::optional<Vector2D> camera(const std::string& scope) {
        load();
        if (const auto IT = g_cameras.find(scope); IT != g_cameras.end())
            return IT->second;
        return std::nullopt;
    }

    void save(const std::unordered_map<std::string, Vector2D>& cameras) {
        load();
        for (const auto& [scope, offset] : cameras) {
            if (sane(offset.x) && sane(offset.y))
                g_cameras[scope] = offset;
        }

        std::error_code error;
        std::filesystem::create_directories(std::filesystem::path{path()}.parent_path(), error);
        const auto    TEMPORARY = path() + ".next";
        std::ofstream out{TEMPORARY, std::ios::trunc};
        if (!out)
            return;
        out << std::fixed << std::setprecision(1) << "# spatial-overview canvas memory v3 (cameras only)\n";
        for (const auto& [scope, offset] : g_cameras)
            out << "camera\t" << clean(scope) << '\t' << offset.x << '\t' << offset.y << '\n';
        out.close();
        if (out)
            std::filesystem::rename(TEMPORARY, path(), error);
    }
}
