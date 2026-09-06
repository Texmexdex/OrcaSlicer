#pragma once

#include "libslic3r/Model.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include <string>
#include <vector>

namespace Slic3r {

struct ColorTraceLayer {
    unsigned char r{0}, g{0}, b{0};
    double height_mm{2.0};
    int extruder_id{1};
    bool is_negative{false};
    ExPolygons expolygons;
    indexed_triangle_set mesh;
};

class ColorImageTracer {
public:
    static bool process_image(
        const std::string& filepath,
        int k_clusters,
        double target_width_mm,
        int min_area_px,
        double smooth_tolerance_px,
        std::vector<ColorTraceLayer>& out_layers,
        std::vector<unsigned char>* out_preview_rgb = nullptr,
        int* out_preview_width = nullptr,
        int* out_preview_height = nullptr
    );

    static void extrude_layer(ColorTraceLayer& layer);
};

} // namespace Slic3r
