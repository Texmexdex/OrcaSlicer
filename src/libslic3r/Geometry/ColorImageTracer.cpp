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

    const double z_bottom = 0.0;
    const double z_top = layer.height_mm;

    // 1. Bottom and top caps using Slic3r's native triangulate_expolygons_3d
    its_merge(layer.mesh, triangulate_expolygons_3d(layer.expolygons, z_bottom, NORMALS_DOWN));
    its_merge(layer.mesh, triangulate_expolygons_3d(layer.expolygons, z_top, NORMALS_UP));

    // 2. Vertical side walls connecting perimeter loops with bit-exact float vertices
    auto create_wall_strip = [](const Polygon& poly, double lower_z, double upper_z) -> indexed_triangle_set {
        indexed_triangle_set ret;
        size_t offs = poly.points.size();
        if (offs < 3) {
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

    for (const ExPolygon& expoly : layer.expolygons) {
        its_merge(layer.mesh, create_wall_strip(expoly.contour, z_bottom, z_top));
        for (const Polygon& hole : expoly.holes) {
            its_merge(layer.mesh, create_wall_strip(hole, z_bottom, z_top));
        }
    }

    // 3. Weld matching float vertices and remove degenerate elements to ensure 100% watertight manifold mesh
    its_merge_vertices(layer.mesh);
    its_remove_degenerate_faces(layer.mesh);
    its_compactify_vertices(layer.mesh);
}

} // namespace Slic3r
