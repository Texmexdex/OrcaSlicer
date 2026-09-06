#pragma once

#include "libslic3r/Model.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include <string>
#include <vector>

namespace Slic3r {

enum class CornerStyle {
    Sharp = 0,     // ClipperLib::jtMiter
    Round = 1,     // ClipperLib::jtRound
    Beveled = 2    // ClipperLib::jtSquare
};

enum class FaceProfile {
    Flat = 0,
    Chamfer = 1,
    Fillet = 2,
    Peaked = 3,
    Bubbled = 4
};

struct ColorTraceLayer {
    unsigned char r{0}, g{0}, b{0};
    double height_mm{2.0};
    int extruder_id{1};
    bool is_negative{false};
    double offset_mm{0.0};                      // XY tolerance offset (clearance gap < 0, perimeter choke > 0)
    CornerStyle corner_style{CornerStyle::Sharp};
    FaceProfile face_profile{FaceProfile::Flat};
    double face_height_mm{0.8};                 // Height/depth of chamfer, fillet, peak, or dome

    ExPolygons expolygons;                      // Base 2D vectorized contours
    indexed_triangle_set mesh;                  // Extruded 3D watertight manifold mesh
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
