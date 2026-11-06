#include "gpu3d_interp.h"

#include <cstdio>
#include <deque>
#include <initializer_list>
#include <vector>

using melonDS::Polygon;
using melonDS::Vertex;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (ok) return;
    std::fprintf(stderr, "FAILED: %s\n", what);
    ++failures;
}

struct Corner {
    int x, y;  // screen position
    int16_t id;  // texcoords: which vertex of the model this is
    bool clipped = false;
};

// One image. Corners at the same screen position are one clip-space vertex.
struct Image {
    std::deque<Vertex> vertices;
    std::vector<Polygon> polygons;
    int w = 0x1000;

    void add(std::initializer_list<Corner> corners, uint32_t tex_param = 1) {
        Polygon polygon;
        polygon.NumVertices = 0;
        polygon.TexParam = tex_param;
        polygon.Attr = 0;
        polygon.TexPalette = 0;
        polygon.Translucent = polygon.IsShadow = polygon.IsShadowMask = false;
        for (const Corner& corner : corners) {
            Vertex vertex{};
            vertex.Position[0] = corner.x;
            vertex.Position[1] = corner.y;
            vertex.Position[3] = w;
            vertex.TexCoords[0] = corner.id;
            vertex.FinalPosition[0] = corner.x;
            vertex.FinalPosition[1] = corner.y;
            vertex.HiresPosition[0] = corner.x * 16;
            vertex.HiresPosition[1] = corner.y * 16;
            vertex.Clipped = corner.clipped;
            vertices.push_back(vertex);
            polygon.FinalZ[polygon.NumVertices] = 1000;
            polygon.Vertices[polygon.NumVertices++] = &vertices.back();
        }
        polygons.push_back(polygon);
    }
    std::vector<NdsGxSnapshotPolygon> snapshot() const {
        std::vector<NdsGxSnapshotPolygon> out(polygons.size());
        for (size_t i = 0; i < out.size(); ++i)
            nds_gx_snapshot_polygon(polygons[i], &out[i]);
        return out;
    }
};

struct Blend {
    std::vector<Polygon> polygons = std::vector<Polygon>(2048);
    std::vector<Vertex> vertices = std::vector<Vertex>(2048 * 10);
    NdsGxBlendCounts counts;
    bool ok;

    Blend(const Image& from, const Image& to) {
        const auto a = from.snapshot();
        const auto b = to.snapshot();
        ok = nds_gx_blend_image(a.data(), a.size(), b.data(),
                                to.polygons.data(), b.size(), 256,
                                polygons.data(), vertices.data(), &counts);
    }
    int x(size_t polygon, uint32_t corner) const {
        return polygons[polygon].Vertices[corner]->FinalPosition[0];
    }
    int y(size_t polygon, uint32_t corner) const {
        return polygons[polygon].Vertices[corner]->FinalPosition[1];
    }
};

void test_blend_is_halfway() {
    Image from, to;
    from.w = 0x1000;
    to.w = 0x3000;
    from.add({{20, 40, 1}, {30, 40, 2}, {20, 50, 3}});
    to.add({{30, 60, 1}, {40, 60, 2}, {30, 70, 3}});
    const Blend blend(from, to);
    check(blend.ok && blend.counts.paired == 1, "the polygon pairs");
    const Vertex& v = *blend.polygons[0].Vertices[0];
    check(v.FinalPosition[0] == 25 && v.FinalPosition[1] == 50,
          "position is halfway");
    check(v.HiresPosition[0] == 25 * 16, "hires is halfway");
    check(v.Position[3] == 0x2000, "W is halfway");
    check(blend.polygons[0].YTop == 50 && blend.polygons[0].YBottom == 60,
          "bounds follow the vertices");
    check(to.polygons[0].Vertices[0]->FinalPosition[0] == 30,
          "source unchanged");
}

void test_match_goes_by_identity_not_position_in_the_list() {
    // A strip of look-alike polygons, the second gone from the next image.
    // Its successor is an acceptable neighbour by surface and distance alone.
    Image before, after;
    for (int16_t i = 0; i < 5; ++i) {
        const int x = 20 + i * 6;
        const int16_t id = int16_t(i * 3);
        before.add({{x, 50, id}, {x + 6, 50, int16_t(id + 1)},
                    {x, 56, int16_t(id + 2)}});
        if (i == 1) continue;
        after.add({{x + 1, 50, id}, {x + 7, 50, int16_t(id + 1)},
                   {x + 1, 56, int16_t(id + 2)}});
    }
    const auto a = before.snapshot();
    const auto b = after.snapshot();
    std::vector<int32_t> pairs(b.size());
    check(nds_gx_match(a.data(), a.size(), b.data(), b.size(),
                       pairs.data()) == 4, "the survivors pair");
    check(pairs[0] == 0 && pairs[1] == 2 && pairs[2] == 3 && pairs[3] == 4,
          "pairs skip the polygon that left");
}

void test_match_picks_the_repeat_that_fits_the_motion() {
    // Identical tiles in a row, everything 5 px along: the nearest look-alike
    // is the wrong one for the tile that the first tile tells us about.
    Image before, after;
    before.add({{10, 10, 1}, {14, 10, 2}, {10, 14, 3}}, 2);
    before.add({{50, 50, 1}, {58, 50, 2}, {50, 58, 3}});
    before.add({{58, 50, 1}, {66, 50, 2}, {58, 58, 3}});
    after.add({{15, 10, 1}, {19, 10, 2}, {15, 14, 3}}, 2);
    after.add({{55, 50, 1}, {63, 50, 2}, {55, 58, 3}});
    after.add({{63, 50, 1}, {71, 50, 2}, {63, 58, 3}});
    const auto a = before.snapshot();
    const auto b = after.snapshot();
    std::vector<int32_t> pairs(b.size());
    nds_gx_match(a.data(), a.size(), b.data(), b.size(), pairs.data());
    check(pairs[1] == 1 && pairs[2] == 2, "tiles pair with themselves");
}

void test_match_needs_proximity() {
    Image from, to;
    from.add({{20, 40, 1}, {30, 40, 2}, {20, 50, 3}});
    to.add({{120, 40, 1}, {130, 40, 2}, {120, 50, 3}});
    const auto a = from.snapshot();
    const auto b = to.snapshot();
    int32_t pair = 0;
    check(nds_gx_match(a.data(), 1, b.data(), 1, &pair) == 0 && pair == -1,
          "distant look-alikes do not pair");
}

void test_order_alone_does_not_pair() {
    // Same surface, same place, nothing in common: not the same polygon.
    Image from, to;
    from.add({{20, 40, 1}, {30, 40, 2}, {20, 50, 3}});
    to.add({{21, 40, 4}, {40, 40, 5}, {21, 70, 6}});
    const auto a = from.snapshot();
    const auto b = to.snapshot();
    int32_t pair = 0;
    check(nds_gx_match(a.data(), 1, b.data(), 1, &pair) == 0,
          "a different outline does not pair");
}

void test_scrolling_texture_pairs_by_outline() {
    Image from, to;
    from.add({{20, 40, 1}, {30, 40, 2}, {20, 50, 3}});
    to.add({{22, 40, 11}, {32, 40, 12}, {22, 50, 13}});
    const auto a = from.snapshot();
    const auto b = to.snapshot();
    int32_t pair = -1;
    check(nds_gx_match(a.data(), 1, b.data(), 1, &pair) == 1 && pair == 0,
          "same outline, moved texture");
}

void test_shared_corner_stays_shared() {
    // The second polygon has no pair, the first does: the corner they share
    // must not open up, and the rest of the mesh follows it.
    Image from, to;
    from.add({{20, 40, 1}, {30, 40, 2}, {20, 50, 3}});
    from.add({{100, 100, 20}, {110, 100, 21}, {100, 110, 22}}, 2);
    to.add({{30, 40, 1}, {40, 40, 2}, {30, 50, 3}});
    to.add({{40, 40, 7}, {50, 40, 8}, {40, 50, 9}});
    to.add({{100, 100, 20}, {110, 100, 21}, {100, 110, 22}}, 2);
    const Blend blend(from, to);
    check(blend.ok, "blends");
    check(blend.x(0, 1) == blend.x(1, 0) && blend.y(0, 1) == blend.y(1, 0),
          "the shared corner is one point");
    check(blend.x(1, 1) == 45, "the unpaired polygon moves with the mesh");
}

void test_majority_decides_a_vertex() {
    // Three polygons meet at (40, 40). Two say it came from (30, 40), and the
    // third, whose model moves separately, says otherwise.
    Image from, to;
    from.add({{30, 40, 1}, {20, 40, 2}, {30, 30, 3}});
    from.add({{30, 40, 4}, {30, 50, 5}, {20, 50, 6}});
    from.add({{36, 40, 7}, {46, 30, 8}, {46, 50, 9}});
    to.add({{40, 40, 1}, {30, 40, 2}, {40, 30, 3}});
    to.add({{40, 40, 4}, {40, 50, 5}, {30, 50, 6}});
    to.add({{40, 40, 7}, {50, 30, 8}, {50, 50, 9}});
    const Blend blend(from, to);
    check(blend.x(0, 0) == 35 && blend.x(1, 0) == 35 && blend.x(2, 0) == 35,
          "every polygon takes the majority");
}

void test_polygon_turning_over_keeps_its_winding() {
    Image from, to;
    from.add({{20, 40, 1}, {30, 40, 2}, {20, 80, 3}});
    to.add({{20, 40, 1}, {30, 40, 2}, {20, 50, 3}});
    from.add({{60, 40, 4}, {70, 40, 5}, {60, 20, 6}});
    to.add({{60, 40, 4}, {70, 40, 5}, {60, 80, 6}});
    const Blend blend(from, to);
    const auto area = [&blend](size_t i) {
        return (blend.x(i, 1) - blend.x(i, 0)) *
                   (blend.y(i, 2) - blend.y(i, 0)) -
               (blend.x(i, 2) - blend.x(i, 0)) *
                   (blend.y(i, 1) - blend.y(i, 0));
    };
    check(area(0) > 0, "a polygon that keeps its side is left alone");
    check(area(1) > 0, "the corners follow the winding of the real polygon");
    check(blend.y(1, 0) == 50 || blend.y(1, 2) == 50, "still halfway");
}

void test_departing_polygon_stays_with_the_mesh() {
    // The second polygon of the mesh is gone from the next image, turned away.
    Image from, to;
    from.add({{20, 40, 1}, {30, 40, 2}, {20, 50, 3}});
    from.add({{30, 40, 2}, {40, 50, 4}, {20, 50, 3}});
    to.add({{30, 40, 1}, {40, 40, 2}, {30, 50, 3}});
    const Blend blend(from, to);
    check(blend.counts.departing == 1, "the departing polygon is kept");
    check(blend.x(1, 0) == blend.x(0, 1) && blend.x(1, 2) == blend.x(0, 2),
          "it stays attached to what remains");
    check(blend.x(1, 1) == 45, "its free corner moves with the mesh");
}

void test_departing_polygon_past_edge_on_is_culled() {
    Image from, to;
    from.add({{20, 40, 1}, {30, 40, 2}, {25, 30, 3}});     // keeps two corners
    from.add({{25, 60, 4}, {35, 60, 5}, {25, 70, 6}});     // keeps the third
    from.add({{20, 40, 1}, {25, 60, 4}, {30, 40, 2}});     // leaves
    to.add({{20, 40, 1}, {30, 40, 2}, {25, 30, 3}});
    to.add({{25, 0, 4}, {35, 0, 5}, {25, 10, 6}});
    const Blend blend(from, to);
    check(blend.counts.departing == 0, "turned away halfway: not drawn");
}

void test_object_that_left_whole_is_gone() {
    Image from, to;
    from.add({{20, 40, 1}, {30, 40, 2}, {20, 50, 3}});
    from.add({{100, 100, 4}, {110, 100, 5}, {100, 110, 6}});
    to.add({{22, 40, 1}, {32, 40, 2}, {22, 50, 3}});
    const Blend blend(from, to);
    check(blend.counts.departing == 0, "nothing ties it to the image");
}

void test_clipped_vertex_stays_on_the_screen_edge() {
    // No pair for the clipped corner, but the mesh around it moves by 8 px.
    Image from, to;
    from.add({{20, 40, 1}, {30, 40, 2}, {20, 50, 3}});
    from.add({{60, 40, 4}, {70, 40, 5}, {60, 50, 6}}, 2);
    to.add({{28, 48, 1}, {38, 48, 2}, {28, 58, 3}});
    to.add({{28, 48, 1}, {0, 60, 90, true}, {28, 58, 3}});
    to.add({{68, 48, 4}, {78, 48, 5}, {68, 58, 6}}, 2);
    const Blend blend(from, to);
    check(blend.x(1, 1) == 0, "it stays on the edge");
    check(blend.y(1, 1) == 56, "and slides along it");
}

void test_different_scenes_do_not_blend() {
    Image from, to;
    from.add({{20, 40, 1}, {30, 40, 2}, {20, 50, 3}});
    to.add({{20, 40, 4}, {90, 40, 5}, {20, 150, 6}});
    check(!Blend(from, to).ok, "nothing in common");
}

}  // namespace

int main() {
    test_blend_is_halfway();
    test_match_goes_by_identity_not_position_in_the_list();
    test_match_picks_the_repeat_that_fits_the_motion();
    test_match_needs_proximity();
    test_order_alone_does_not_pair();
    test_scrolling_texture_pairs_by_outline();
    test_shared_corner_stays_shared();
    test_majority_decides_a_vertex();
    test_polygon_turning_over_keeps_its_winding();
    test_departing_polygon_stays_with_the_mesh();
    test_departing_polygon_past_edge_on_is_culled();
    test_object_that_left_whole_is_gone();
    test_clipped_vertex_stays_on_the_screen_edge();
    test_different_scenes_do_not_blend();
    if (failures == 0) std::puts("gpu3d_interp_test: ok");
    return failures != 0;
}
