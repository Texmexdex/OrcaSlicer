#include "ColorImageTracer.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Tesselate.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"

#include <opencv2/opencv.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace Slic3r {

bool ColorImageTracer::process_image(
    const std::string& filepath,
    int k_clusters,
    double target_width_mm,
    int min_area_px,
    double smooth_tolerance_px,
    std::vector<ColorTraceLayer>& out_layers,
    std::vector<unsigned char>* out_preview_rgb,
    int* out_preview_width,
    int* out_preview_height)
{
    out_layers.clear();

    // 1. Load the image with unchanged channels to preserve alpha transparency
    cv::Mat img = cv::imread(filepath, cv::IMREAD_UNCHANGED);
    if (img.empty()) {
        return false;
    }

    // 2. Cap processing dimensions to 1280px for instantaneous clustering and responsiveness
    const int MAX_DIM = 1280;
    if (img.cols > MAX_DIM || img.rows > MAX_DIM) {
        double scale_img = static_cast<double>(MAX_DIM) / std::max(img.cols, img.rows);
        int new_w = std::max(1, static_cast<int>(std::round(img.cols * scale_img)));
        int new_h = std::max(1, static_cast<int>(std::round(img.rows * scale_img)));
        cv::resize(img, img, cv::Size(new_w, new_h), 0, 0, cv::INTER_AREA);
    }

    // 3. Separate BGR color channels and optional Alpha transparency channel
    bool has_alpha = (img.channels() == 4);
    cv::Mat bgr;
    cv::Mat alpha;
    if (has_alpha) {
        cv::Mat channels[4];
        cv::split(img, channels);
        alpha = channels[3];
        cv::merge(channels, 3, bgr);
    } else if (img.channels() == 3) {
        bgr = img;
    } else if (img.channels() == 1) {
        cv::cvtColor(img, bgr, cv::COLOR_GRAY2BGR);
    } else {
        return false;
    }

    // 4. Eliminate pixel noise while preserving sharp feature boundaries
    cv::Mat filtered;
    cv::bilateralFilter(bgr, filtered, 9, 75, 75);

    int total_pixels = filtered.rows * filtered.cols;
    if (total_pixels == 0 || target_width_mm <= 0.0) {
        return false;
    }

    // 5. Gather only opaque pixels for K-means clustering (ignoring transparent voids)
    std::vector<int> opaque_indices;
    if (has_alpha) {
        opaque_indices.reserve(total_pixels);
        for (int r = 0; r < filtered.rows; ++r) {
            const uchar* a_row = alpha.ptr<uchar>(r);
            for (int c = 0; c < filtered.cols; ++c) {
                if (a_row[c] >= 128) {
                    opaque_indices.push_back(r * filtered.cols + c);
                }
            }
        }
    }

    int num_train = has_alpha ? static_cast<int>(opaque_indices.size()) : total_pixels;
    if (num_train == 0) {
        return false;
    }

    k_clusters = std::clamp(k_clusters, 1, std::min(16, num_train));

    // Construct float32 matrix for k-means
    cv::Mat data(num_train, 3, CV_32F);
    if (has_alpha) {
        for (int i = 0; i < num_train; ++i) {
            int idx = opaque_indices[i];
            int r = idx / filtered.cols;
            int c = idx % filtered.cols;
            const cv::Vec3b& color = filtered.at<cv::Vec3b>(r, c);
            data.at<float>(i, 0) = static_cast<float>(color[0]);
            data.at<float>(i, 1) = static_cast<float>(color[1]);
            data.at<float>(i, 2) = static_cast<float>(color[2]);
        }
    } else {
        cv::Mat reshaped = filtered.reshape(1, total_pixels);
        reshaped.convertTo(data, CV_32F);
    }

    cv::Mat labels;
    cv::Mat centers;
    cv::kmeans(data, k_clusters, labels,
               cv::TermCriteria(cv::TermCriteria::EPS + cv::TermCriteria::COUNT, 20, 0.2),
               3, cv::KMEANS_PP_CENTERS, centers);

    // Build discrete 2D label map (CV_8U), where 255 denotes transparent/void
    cv::Mat label_mat(filtered.rows, filtered.cols, CV_8U, cv::Scalar(255));
    if (has_alpha) {
        for (int i = 0; i < num_train; ++i) {
            int idx = opaque_indices[i];
            int r = idx / filtered.cols;
            int c = idx % filtered.cols;
            label_mat.at<uchar>(r, c) = static_cast<uchar>(labels.at<int>(i));
        }
    } else {
        cv::Mat reshaped_labels = labels.reshape(1, filtered.rows);
        reshaped_labels.convertTo(label_mat, CV_8U);
    }

    // 6. Optional: generate quantized RGB preview buffer with checkerboard transparency
    if (out_preview_rgb != nullptr && out_preview_width != nullptr && out_preview_height != nullptr) {
        *out_preview_width = filtered.cols;
        *out_preview_height = filtered.rows;
        out_preview_rgb->resize(static_cast<size_t>(filtered.rows) * static_cast<size_t>(filtered.cols) * 3);
        unsigned char* dst = out_preview_rgb->data();

        for (int r = 0; r < filtered.rows; ++r) {
            for (int c = 0; c < filtered.cols; ++c) {
                int px_idx = (r * filtered.cols + c) * 3;
                uchar lbl = label_mat.at<uchar>(r, c);
                if (lbl == 255) {
                    bool check = ((r / 10) + (c / 10)) % 2 == 0;
                    unsigned char val = check ? 220 : 255;
                    dst[px_idx + 0] = val;
                    dst[px_idx + 1] = val;
                    dst[px_idx + 2] = val;
                } else {
                    float b = centers.at<float>(lbl, 0);
                    float g = centers.at<float>(lbl, 1);
                    float red = centers.at<float>(lbl, 2);
                    dst[px_idx + 0] = static_cast<unsigned char>(std::clamp(red, 0.0f, 255.0f));
                    dst[px_idx + 1] = static_cast<unsigned char>(std::clamp(g, 0.0f, 255.0f));
                    dst[px_idx + 2] = static_cast<unsigned char>(std::clamp(b, 0.0f, 255.0f));
                }
            }
        }
    }

    // 7. Calculate scale factor from pixels to Slic3r internal nanometer coordinates
    const double mm_per_px = target_width_mm / static_cast<double>(filtered.cols);
    const double scale = scale_(mm_per_px);

    out_layers.reserve(k_clusters);

    // 8. Vectorize each detected color cluster into clean topological polygons
    for (int k = 0; k < k_clusters; ++k) {
        ColorTraceLayer layer;
        float b = centers.at<float>(k, 0);
        float g = centers.at<float>(k, 1);
        float r = centers.at<float>(k, 2);
        layer.r = static_cast<unsigned char>(std::clamp(r, 0.0f, 255.0f));
        layer.g = static_cast<unsigned char>(std::clamp(g, 0.0f, 255.0f));
        layer.b = static_cast<unsigned char>(std::clamp(b, 0.0f, 255.0f));
        layer.extruder_id = (k % MAXIMUM_EXTRUDER_NUMBER) + 1;
        layer.height_mm = 2.0;
        layer.is_negative = false;
        layer.offset_mm = 0.0;
        layer.corner_style = CornerStyle::Sharp;
        layer.face_profile = FaceProfile::Flat;
        layer.face_height_mm = 0.8;

        cv::Mat mask = (label_mat == k);
        if (cv::countNonZero(mask) == 0) {
            continue;
        }

        // Discrete 3x3 median filter suppresses 1-pixel anti-aliasing edge noise while preserving fine lines
        cv::medianBlur(mask, mask, 3);
        if (cv::countNonZero(mask) == 0) {
            continue;
        }

        // Full topological tree extraction to correctly preserve nested islands & holes
        std::vector<std::vector<cv::Point>> contours;
        std::vector<cv::Vec4i> hierarchy;
        cv::findContours(mask, contours, hierarchy, cv::RETR_TREE, cv::CHAIN_APPROX_SIMPLE);

        if (contours.empty() || hierarchy.empty()) {
            continue;
        }

        Polygons layer_polygons;
        layer_polygons.reserve(contours.size());

        for (size_t i = 0; i < contours.size(); ++i) {
            double area = std::abs(cv::contourArea(contours[i]));
            if (area < min_area_px || contours[i].size() < 3) {
                continue;
            }

            std::vector<cv::Point> approx;
            if (smooth_tolerance_px > 0.0) {
                cv::approxPolyDP(contours[i], approx, smooth_tolerance_px, true);
            } else {
                approx = contours[i];
            }
            if (approx.size() < 3) {
                continue;
            }

            // Determine contour nesting depth: even = solid outer island, odd = hole
            int depth = 0;
            int p = hierarchy[i][3];
            while (p != -1) {
                depth++;
                p = hierarchy[p][3];
            }
            bool is_hole = (depth % 2 == 1);

            Polygon poly;
            poly.points.reserve(approx.size());
            for (const auto& pt : approx) {
                coord_t x = static_cast<coord_t>(std::round(pt.x * scale));
                coord_t y = static_cast<coord_t>(std::round((filtered.rows - pt.y) * scale));
                poly.points.emplace_back(x, y);
            }

            // Slic3r & Clipper NonZero convention in Cartesian space:
            // Outer contours must be CCW, holes must be CW
            if (!is_hole) {
                if (poly.is_clockwise()) {
                    poly.reverse();
                }
            } else {
                if (!poly.is_clockwise()) {
                    poly.reverse();
                }
            }

            layer_polygons.push_back(std::move(poly));
        }

        if (!layer_polygons.empty()) {
            // Clipper strictly-simple union resolves all nesting and eliminates pinched/bowtie vertices
            layer.expolygons = simplify_polygons_ex(layer_polygons);
            if (!layer.expolygons.empty()) {
                extrude_layer(layer);
                out_layers.push_back(std::move(layer));
            }
        }
    }

    return !out_layers.empty();
}

void ColorImageTracer::extrude_layer(ColorTraceLayer& layer)
{
    layer.mesh.clear();
    if (layer.expolygons.empty() || layer.height_mm <= 0.0) {
        return;
    }

    // 1. Determine Clipper join type based on CornerStyle
    ClipperLib::JoinType join_type = ClipperLib::jtMiter;
    if (layer.corner_style == CornerStyle::Round) {
        join_type = ClipperLib::jtRound;
    } else if (layer.corner_style == CornerStyle::Beveled) {
        join_type = ClipperLib::jtSquare;
    }

    // 2. Apply XY tolerance offset (clearance gap < 0, perimeter choke > 0)
    ExPolygons P0 = layer.expolygons;
    if (std::abs(layer.offset_mm) > 1e-4) {
        P0 = offset_ex(P0, static_cast<float>(scale_(layer.offset_mm)), join_type, 3.0);
        if (P0.empty()) {
            return;
        }
    }

    // Helper: Construct vertical wall quad strip between two Z heights
    auto create_wall_strip = [](const Polygon& poly, double lower_z, double upper_z) -> indexed_triangle_set {
        indexed_triangle_set ret;
        size_t offs = poly.points.size();
        if (offs < 3 || upper_z <= lower_z) {
            return ret;
        }

        ret.vertices.reserve(2 * offs);
        for (const Point& p : poly.points) {
            ret.vertices.emplace_back(to_3d(unscaled(p).cast<float>().eval(), static_cast<float>(lower_z)));
        }
        for (const Point& p : poly.points) {
            ret.vertices.emplace_back(to_3d(unscaled(p).cast<float>().eval(), static_cast<float>(upper_z)));
        }

        ret.indices.reserve(2 * offs);
        for (size_t i = 1; i < offs; ++i) {
            ret.indices.emplace_back(static_cast<int>(i - 1), static_cast<int>(i), static_cast<int>(i + offs - 1));
            ret.indices.emplace_back(static_cast<int>(i), static_cast<int>(i + offs), static_cast<int>(i + offs - 1));
        }
        ret.indices.emplace_back(static_cast<int>(offs - 1), 0, static_cast<int>(2 * offs - 1));
        ret.indices.emplace_back(0, static_cast<int>(offs), static_cast<int>(2 * offs - 1));

        return ret;
    };

    // Helper: Construct sloping transition band between two concentric polygon slices
    auto add_sloping_band = [&](indexed_triangle_set& dst_mesh, const ExPolygons& p_lower, const ExPolygons& p_upper, double z_lower, double z_upper) {
        if (p_lower.empty()) return;
        ExPolygons band_expolys = diff_ex(p_lower, p_upper);
        if (band_expolys.empty()) return;

        for (const ExPolygon& band_poly : band_expolys) {
            std::set<std::pair<coord_t, coord_t>> upper_pts;
            for (const Polygon& hole : band_poly.holes) {
                for (const Point& pt : hole.points) {
                    upper_pts.insert({pt.x(), pt.y()});
                }
            }

            std::vector<Vec3d> tris = triangulate_expolygon_3d(band_poly, 0.0, NORMALS_UP);
            if (tris.empty()) continue;

            size_t base_idx = dst_mesh.vertices.size();
            dst_mesh.vertices.reserve(dst_mesh.vertices.size() + tris.size());
            dst_mesh.indices.reserve(dst_mesh.indices.size() + tris.size() / 3);

            for (size_t t = 0; t + 2 < tris.size(); t += 3) {
                for (size_t v_idx = 0; v_idx < 3; ++v_idx) {
                    const Vec3d& pt3d = tris[t + v_idx];
                    coord_t cx = static_cast<coord_t>(std::round(scale_(pt3d.x())));
                    coord_t cy = static_cast<coord_t>(std::round(scale_(pt3d.y())));
                    float z_val = static_cast<float>((upper_pts.count({cx, cy}) > 0) ? z_upper : z_lower);
                    dst_mesh.vertices.emplace_back(static_cast<float>(pt3d.x()), static_cast<float>(pt3d.y()), z_val);
                }
                dst_mesh.indices.emplace_back(static_cast<int>(base_idx + t),
                                              static_cast<int>(base_idx + t + 1),
                                              static_cast<int>(base_idx + t + 2));
            }
        }
    };

    // Helper: Add vertical walls for an ExPolygons set
    auto add_vertical_walls = [&](const ExPolygons& polys, double z_lo, double z_hi) {
        if (z_hi <= z_lo) return;
        for (const ExPolygon& expoly : polys) {
            its_merge(layer.mesh, create_wall_strip(expoly.contour, z_lo, z_hi));
            for (const Polygon& hole : expoly.holes) {
                its_merge(layer.mesh, create_wall_strip(hole, z_lo, z_hi));
            }
        }
    };

    // 3. Bottom cap at Z = 0 with downward normal
    its_merge(layer.mesh, triangulate_expolygons_3d(P0, 0.0, NORMALS_DOWN));

    // 4. Construct 3D body based on requested FaceProfile
    if (layer.face_profile == FaceProfile::Flat || layer.face_height_mm <= 0.001) {
        // Standard flat extrusion
        add_vertical_walls(P0, 0.0, layer.height_mm);
        its_merge(layer.mesh, triangulate_expolygons_3d(P0, layer.height_mm, NORMALS_UP));
    }
    else if (layer.face_profile == FaceProfile::Chamfer) {
        // 45-degree angular beveled shoulder around top perimeter
        double h_f = std::clamp(layer.face_height_mm, 0.05, layer.height_mm * 0.95);
        double z_base = std::max(0.0, layer.height_mm - h_f);

        add_vertical_walls(P0, 0.0, z_base);

        ExPolygons P_top = offset_ex(P0, static_cast<float>(-scale_(h_f)), join_type, 3.0);
        add_sloping_band(layer.mesh, P0, P_top, z_base, layer.height_mm);

        if (!P_top.empty()) {
            its_merge(layer.mesh, triangulate_expolygons_3d(P_top, layer.height_mm, NORMALS_UP));
        }
    }
    else if (layer.face_profile == FaceProfile::Fillet) {
        // Quarter-circle rounded shoulder
        double h_f = std::clamp(layer.face_height_mm, 0.05, layer.height_mm * 0.95);
        double z_base = std::max(0.0, layer.height_mm - h_f);

        add_vertical_walls(P0, 0.0, z_base);

        const int NUM_STEPS = 4;
        ExPolygons P_curr = P0;
        double z_curr = z_base;

        for (int s = 1; s <= NUM_STEPS; ++s) {
            double angle = (3.14159265358979323846 / 2.0) * (static_cast<double>(s) / NUM_STEPS);
            double z_next = z_base + h_f * std::sin(angle);
            double inset_dist = h_f * (1.0 - std::cos(angle));

            ExPolygons P_next = offset_ex(P0, static_cast<float>(-scale_(inset_dist)), join_type, 3.0);
            add_sloping_band(layer.mesh, P_curr, P_next, z_curr, z_next);

            P_curr = std::move(P_next);
            z_curr = z_next;
            if (P_curr.empty()) break;
        }

        if (!P_curr.empty()) {
            its_merge(layer.mesh, triangulate_expolygons_3d(P_curr, layer.height_mm, NORMALS_UP));
        }
    }
    else if (layer.face_profile == FaceProfile::Peaked) {
        // Straight-skeleton hipped roof / pyramid rising to ridges and peaks
        double h_f = std::clamp(layer.face_height_mm, 0.05, layer.height_mm * 0.95);
        double z_base = std::max(0.0, layer.height_mm - h_f);

        add_vertical_walls(P0, 0.0, z_base);

        const int MAX_STEPS = 8;
        double step_delta = std::max(0.15, h_f / 5.0);

        std::vector<ExPolygons> slices;
        slices.push_back(P0);

        while (static_cast<int>(slices.size()) <= MAX_STEPS) {
            ExPolygons next_s = offset_ex(slices.back(), static_cast<float>(-scale_(step_delta)), join_type, 3.0);
            if (next_s.empty()) break;
            slices.push_back(std::move(next_s));
        }

        size_t M = slices.size() - 1;
        if (M == 0) {
            its_merge(layer.mesh, triangulate_expolygons_3d(P0, layer.height_mm, NORMALS_UP));
        } else {
            for (size_t s = 0; s < M; ++s) {
                double z_s = z_base + h_f * (static_cast<double>(s) / M);
                double z_s_next = z_base + h_f * (static_cast<double>(s + 1) / M);
                add_sloping_band(layer.mesh, slices[s], slices[s + 1], z_s, z_s_next);
            }
            if (!slices.back().empty()) {
                its_merge(layer.mesh, triangulate_expolygons_3d(slices.back(), layer.height_mm, NORMALS_UP));
            }
        }
    }
    else if (layer.face_profile == FaceProfile::Bubbled) {
        // Inflated spherical dome / pillow crown
        double h_f = std::clamp(layer.face_height_mm, 0.05, layer.height_mm * 0.95);
        double z_base = std::max(0.0, layer.height_mm - h_f);

        add_vertical_walls(P0, 0.0, z_base);

        const int MAX_STEPS = 8;
        double step_delta = std::max(0.15, h_f / 5.0);

        std::vector<ExPolygons> slices;
        slices.push_back(P0);

        while (static_cast<int>(slices.size()) <= MAX_STEPS) {
            ExPolygons next_s = offset_ex(slices.back(), static_cast<float>(-scale_(step_delta)), join_type, 3.0);
            if (next_s.empty()) break;
            slices.push_back(std::move(next_s));
        }

        size_t M = slices.size() - 1;
        if (M == 0) {
            its_merge(layer.mesh, triangulate_expolygons_3d(P0, layer.height_mm, NORMALS_UP));
        } else {
            for (size_t s = 0; s < M; ++s) {
                double z_s = z_base + h_f * std::sin((3.14159265358979323846 / 2.0) * (static_cast<double>(s) / M));
                double z_s_next = z_base + h_f * std::sin((3.14159265358979323846 / 2.0) * (static_cast<double>(s + 1) / M));
                add_sloping_band(layer.mesh, slices[s], slices[s + 1], z_s, z_s_next);
            }
            if (!slices.back().empty()) {
                its_merge(layer.mesh, triangulate_expolygons_3d(slices.back(), layer.height_mm, NORMALS_UP));
            }
        }
    }

    // 5. Weld matching float vertices and remove degenerate elements for 100% watertight manifold mesh
    its_merge_vertices(layer.mesh);
    its_remove_degenerate_faces(layer.mesh);
    its_compactify_vertices(layer.mesh);
}

} // namespace Slic3r
