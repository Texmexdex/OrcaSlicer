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

namespace Slic3r {

bool ColorImageTracer::process_image(
    const std::string& filepath,
    int k_clusters,
    double target_width_mm,
    int min_area_px,
    std::vector<ColorTraceLayer>& out_layers,
    std::vector<unsigned char>* out_preview_rgb,
    int* out_preview_width,
    int* out_preview_height)
{
    out_layers.clear();

    // 1. Load the image
    cv::Mat img = cv::imread(filepath, cv::IMREAD_COLOR);
    if (img.empty()) {
        return false;
    }

    // 2. Eliminate pixel noise while preserving vector edges
    cv::Mat filtered;
    cv::bilateralFilter(img, filtered, 9, 75, 75);

    int num_pixels = filtered.rows * filtered.cols;
    if (num_pixels == 0 || target_width_mm <= 0.0) {
        return false;
    }

    k_clusters = std::clamp(k_clusters, 1, std::min(16, num_pixels));

    // 3. Quantize colors via cv::kmeans on float32 reshaped data using cv::KMEANS_PP_CENTERS
    cv::Mat data = filtered.reshape(1, num_pixels);
    data.convertTo(data, CV_32F);

    cv::Mat labels;
    cv::Mat centers;
    cv::kmeans(data, k_clusters, labels,
               cv::TermCriteria(cv::TermCriteria::EPS + cv::TermCriteria::COUNT, 20, 0.2),
               3, cv::KMEANS_PP_CENTERS, centers);

    cv::Mat label_mat = labels.reshape(1, filtered.rows);

    // Optional: generate quantized RGB preview buffer
    if (out_preview_rgb != nullptr && out_preview_width != nullptr && out_preview_height != nullptr) {
        *out_preview_width = filtered.cols;
        *out_preview_height = filtered.rows;
        out_preview_rgb->resize(static_cast<size_t>(filtered.rows) * static_cast<size_t>(filtered.cols) * 3);
        unsigned char* dst = out_preview_rgb->data();

        for (int r = 0; r < filtered.rows; ++r) {
            for (int c = 0; c < filtered.cols; ++c) {
                int cluster_idx = label_mat.at<int>(r, c);
                float b = centers.at<float>(cluster_idx, 0);
                float g = centers.at<float>(cluster_idx, 1);
                float red = centers.at<float>(cluster_idx, 2);

                int px_idx = (r * filtered.cols + c) * 3;
                dst[px_idx + 0] = static_cast<unsigned char>(std::clamp(red, 0.0f, 255.0f));
                dst[px_idx + 1] = static_cast<unsigned char>(std::clamp(g, 0.0f, 255.0f));
                dst[px_idx + 2] = static_cast<unsigned char>(std::clamp(b, 0.0f, 255.0f));
            }
        }
    }

    // 4. Calculate scale factor to convert pixel dimensions to internal nanometer Slic3r coordinate units
    const double mm_per_px = target_width_mm / static_cast<double>(filtered.cols);
    const double scale = scale_(mm_per_px);

    out_layers.reserve(k_clusters);

    // 5. For each cluster index k in [0, K-1]
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

        // Generate binary mask
        cv::Mat mask = (label_mat == k);

        // Apply morphological open/close using a 3x3 rectangle kernel
        cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
        cv::morphologyEx(mask, mask, cv::MORPH_OPEN, kernel);
        cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, kernel);

        // Run cv::findContours with RETR_CCOMP and CHAIN_APPROX_TC89_KCOS
        std::vector<std::vector<cv::Point>> contours;
        std::vector<cv::Vec4i> hierarchy;
        cv::findContours(mask, contours, hierarchy, cv::RETR_CCOMP, cv::CHAIN_APPROX_TC89_KCOS);

        if (contours.empty() || hierarchy.empty()) {
            continue;
        }

        ExPolygons layer_expolygons;

        for (int i = 0; i < static_cast<int>(contours.size()); ++i) {
            // Outer contours have hierarchy[i][3] == -1
            if (hierarchy[i][3] != -1) {
                continue;
            }

            double area = std::abs(cv::contourArea(contours[i]));
            if (area < min_area_px || contours[i].size() < 3) {
                continue;
            }

            ExPolygon expoly;
            expoly.contour.points.reserve(contours[i].size());
            for (const auto& pt : contours[i]) {
                coord_t x = static_cast<coord_t>(std::round(pt.x * scale));
                coord_t y = static_cast<coord_t>(std::round((filtered.rows - pt.y) * scale));
                expoly.contour.points.emplace_back(x, y);
            }

            if (expoly.contour.is_clockwise()) {
                expoly.contour.reverse();
            }

            // Gather child contours (hierarchy[i][2]) as holes
            int child_idx = hierarchy[i][2];
            while (child_idx != -1) {
                double hole_area = std::abs(cv::contourArea(contours[child_idx]));
                if (hole_area >= min_area_px && contours[child_idx].size() >= 3) {
                    Polygon hole;
                    hole.points.reserve(contours[child_idx].size());
                    for (const auto& pt : contours[child_idx]) {
                        coord_t x = static_cast<coord_t>(std::round(pt.x * scale));
                        coord_t y = static_cast<coord_t>(std::round((filtered.rows - pt.y) * scale));
                        hole.points.emplace_back(x, y);
                    }
                    if (!hole.is_clockwise()) {
                        hole.reverse();
                    }
                    expoly.holes.push_back(std::move(hole));
                }
                child_idx = hierarchy[child_idx][0]; // next sibling hole
            }

            layer_expolygons.push_back(std::move(expoly));
        }

        // Clean and repair polygons using Clipper union_ex
        layer.expolygons = union_ex(layer_expolygons);
        if (!layer.expolygons.empty()) {
            extrude_layer(layer);
            out_layers.push_back(std::move(layer));
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

    const float z_bottom = 0.0f;
    const float z_top = static_cast<float>(layer.height_mm);

    for (const ExPolygon& expoly : layer.expolygons) {
        // 1. Tessellate 2D polygons using Slic3r::triangulate_expolygon_2d
        std::vector<Vec2d> triangles_2d = triangulate_expolygon_2d(expoly, NORMALS_UP);
        if (triangles_2d.size() < 3) {
            continue;
        }

        // Bottom cap vertices at Z = 0 with reversed winding (normals oriented downward)
        for (size_t t = 0; t + 2 < triangles_2d.size(); t += 3) {
            Vec3f v0(static_cast<float>(triangles_2d[t].x()),     static_cast<float>(triangles_2d[t].y()),     z_bottom);
            Vec3f v1(static_cast<float>(triangles_2d[t + 2].x()), static_cast<float>(triangles_2d[t + 2].y()), z_bottom);
            Vec3f v2(static_cast<float>(triangles_2d[t + 1].x()), static_cast<float>(triangles_2d[t + 1].y()), z_bottom);

            int base_idx = static_cast<int>(layer.mesh.vertices.size());
            layer.mesh.vertices.push_back(v0);
            layer.mesh.vertices.push_back(v1);
            layer.mesh.vertices.push_back(v2);
            layer.mesh.indices.emplace_back(base_idx, base_idx + 1, base_idx + 2);
        }

        // Top cap vertices at Z = layer.height_mm with CCW winding (normals oriented upward)
        for (size_t t = 0; t + 2 < triangles_2d.size(); t += 3) {
            Vec3f v0(static_cast<float>(triangles_2d[t].x()),     static_cast<float>(triangles_2d[t].y()),     z_top);
            Vec3f v1(static_cast<float>(triangles_2d[t + 1].x()), static_cast<float>(triangles_2d[t + 1].y()), z_top);
            Vec3f v2(static_cast<float>(triangles_2d[t + 2].x()), static_cast<float>(triangles_2d[t + 2].y()), z_top);

            int base_idx = static_cast<int>(layer.mesh.vertices.size());
            layer.mesh.vertices.push_back(v0);
            layer.mesh.vertices.push_back(v1);
            layer.mesh.vertices.push_back(v2);
            layer.mesh.indices.emplace_back(base_idx, base_idx + 1, base_idx + 2);
        }

        // 2. Side walls connecting perimeter loop vertices from Z = 0 to Z = height_mm
        auto extrude_loop = [&](const Polygon& loop) {
            size_t n = loop.points.size();
            if (n < 3) return;

            int base_idx = static_cast<int>(layer.mesh.vertices.size());
            layer.mesh.vertices.reserve(layer.mesh.vertices.size() + 2 * n);
            layer.mesh.indices.reserve(layer.mesh.indices.size() + 2 * n);

            // Add bottom ring [0..n-1]
            for (size_t i = 0; i < n; ++i) {
                Vec2d pt = unscaled(loop.points[i]);
                layer.mesh.vertices.emplace_back(static_cast<float>(pt.x()), static_cast<float>(pt.y()), z_bottom);
            }
            // Add top ring [n..2n-1]
            for (size_t i = 0; i < n; ++i) {
                Vec2d pt = unscaled(loop.points[i]);
                layer.mesh.vertices.emplace_back(static_cast<float>(pt.x()), static_cast<float>(pt.y()), z_top);
            }

            // Generate side-wall quads (split into two triangles)
            // Outer contour is CCW -> normals point outward
            // Hole contour is CW -> normals point into the hole void
            for (size_t i = 0; i < n; ++i) {
                size_t next = (i + 1) % n;
                int b_i = base_idx + static_cast<int>(i);
                int b_next = base_idx + static_cast<int>(next);
                int t_i = base_idx + static_cast<int>(n + i);
                int t_next = base_idx + static_cast<int>(n + next);

                layer.mesh.indices.emplace_back(b_i, b_next, t_next);
                layer.mesh.indices.emplace_back(b_i, t_next, t_i);
            }
        };

        extrude_loop(expoly.contour);
        for (const Polygon& hole : expoly.holes) {
            extrude_loop(hole);
        }
    }

    // Merge vertices and remove degenerate faces to ensure watertight manifold mesh
    its_merge_vertices(layer.mesh);
    its_remove_degenerate_faces(layer.mesh);
    its_compactify_vertices(layer.mesh);
}

} // namespace Slic3r
