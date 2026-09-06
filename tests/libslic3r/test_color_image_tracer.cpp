#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include "libslic3r/Geometry/ColorImageTracer.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/ExPolygon.hpp"

using namespace Slic3r;

TEST_CASE("ColorImageTracer 3D extrusion watertightness and offsets", "[ColorImageTracer]") {
    ColorTraceLayer layer;
    layer.r = 255;
    layer.g = 0;
    layer.b = 0;
    layer.height_mm = 4.0;

    // Create a 20mm x 20mm square polygon
    ExPolygon square;
    square.contour.points = {
        Point(scale_(0.0), scale_(0.0)),
        Point(scale_(20.0), scale_(0.0)),
        Point(scale_(20.0), scale_(20.0)),
        Point(scale_(0.0), scale_(20.0))
    };
    layer.expolygons.push_back(square);

    SECTION("Flat profile with zero offset produces watertight mesh") {
        layer.offset_mm = 0.0;
        layer.corner_style = CornerStyle::Sharp;
        layer.face_profile = FaceProfile::Flat;

        ColorImageTracer::extrude_layer(layer);

        REQUIRE(!layer.mesh.empty());
        REQUIRE(its_num_open_edges(layer.mesh) == 0);

        BoundingBoxf3 bb = bounding_box(layer.mesh);
        REQUIRE_THAT(bb.size().x(), Catch::Matchers::WithinRel(20.0f, 0.01f));
        REQUIRE_THAT(bb.size().y(), Catch::Matchers::WithinRel(20.0f, 0.01f));
        REQUIRE_THAT(bb.size().z(), Catch::Matchers::WithinRel(4.0f, 0.01f));
    }

    SECTION("Tolerance offset inset reduces bounding footprint") {
        layer.offset_mm = -1.0;
        layer.corner_style = CornerStyle::Sharp;
        layer.face_profile = FaceProfile::Flat;

        ColorImageTracer::extrude_layer(layer);

        REQUIRE(!layer.mesh.empty());
        REQUIRE(its_num_open_edges(layer.mesh) == 0);

        BoundingBoxf3 bb = bounding_box(layer.mesh);
        REQUIRE_THAT(bb.size().x(), Catch::Matchers::WithinRel(18.0f, 0.01f));
        REQUIRE_THAT(bb.size().y(), Catch::Matchers::WithinRel(18.0f, 0.01f));
    }

    SECTION("Tolerance offset outset expands bounding footprint") {
        layer.offset_mm = 1.0;
        layer.corner_style = CornerStyle::Sharp;
        layer.face_profile = FaceProfile::Flat;

        ColorImageTracer::extrude_layer(layer);

        REQUIRE(!layer.mesh.empty());
        REQUIRE(its_num_open_edges(layer.mesh) == 0);

        BoundingBoxf3 bb = bounding_box(layer.mesh);
        REQUIRE_THAT(bb.size().x(), Catch::Matchers::WithinRel(22.0f, 0.01f));
        REQUIRE_THAT(bb.size().y(), Catch::Matchers::WithinRel(22.0f, 0.01f));
    }

    SECTION("Round corners create higher vertex count than sharp corners") {
        layer.offset_mm = 1.0;
        layer.corner_style = CornerStyle::Sharp;
        ColorImageTracer::extrude_layer(layer);
        size_t sharp_vert_count = layer.mesh.vertices.size();

        layer.corner_style = CornerStyle::Round;
        ColorImageTracer::extrude_layer(layer);
        size_t round_vert_count = layer.mesh.vertices.size();

        REQUIRE(round_vert_count > sharp_vert_count);
        REQUIRE(its_num_open_edges(layer.mesh) == 0);
    }

    SECTION("Peaked pyramid roof produces watertight manifold") {
        layer.offset_mm = 0.0;
        layer.corner_style = CornerStyle::Sharp;
        layer.face_profile = FaceProfile::Peaked;
        layer.face_height_mm = 1.5;

        ColorImageTracer::extrude_layer(layer);

        REQUIRE(!layer.mesh.empty());
        REQUIRE(its_num_open_edges(layer.mesh) == 0);
    }

    SECTION("Bubbled dome crown produces watertight manifold") {
        layer.offset_mm = 0.0;
        layer.corner_style = CornerStyle::Sharp;
        layer.face_profile = FaceProfile::Bubbled;
        layer.face_height_mm = 1.5;

        ColorImageTracer::extrude_layer(layer);

        REQUIRE(!layer.mesh.empty());
        REQUIRE(its_num_open_edges(layer.mesh) == 0);
    }

    SECTION("Chamfer profile produces watertight manifold") {
        layer.offset_mm = 0.0;
        layer.corner_style = CornerStyle::Sharp;
        layer.face_profile = FaceProfile::Chamfer;
        layer.face_height_mm = 1.0;

        ColorImageTracer::extrude_layer(layer);

        REQUIRE(!layer.mesh.empty());
        REQUIRE(its_num_open_edges(layer.mesh) == 0);
    }

    SECTION("Fillet profile produces watertight manifold") {
        layer.offset_mm = 0.0;
        layer.corner_style = CornerStyle::Sharp;
        layer.face_profile = FaceProfile::Fillet;
        layer.face_height_mm = 1.0;

        ColorImageTracer::extrude_layer(layer);

        REQUIRE(!layer.mesh.empty());
        REQUIRE(its_num_open_edges(layer.mesh) == 0);
    }

    SECTION("Excessive negative offset collapses safely without crashing") {
        layer.offset_mm = -15.0; // Exceeds 20mm half-width
        ColorImageTracer::extrude_layer(layer);

        REQUIRE(layer.mesh.empty());
    }
}
