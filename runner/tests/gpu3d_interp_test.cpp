#include "gpu3d_interp.h"

#include <cstdio>
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

// A triangle with its top-left corner at (x, y).
struct Triangle {
    Vertex vertices[3];
    Polygon polygon;
    Triangle(int x, int y, uint32_t tex_param = 1, int w = 0x1000) {
        const int corners[3][2] = {{x, y}, {x + 10, y}, {x, y + 10}};
        for (int i = 0; i < 3; ++i) {
            Vertex& v = vertices[i];
            v.FinalPosition[0] = corners[i][0];
            v.FinalPosition[1] = corners[i][1];
            v.HiresPosition[0] = corners[i][0] * 16;
            v.HiresPosition[1] = corners[i][1] * 16;
            v.Position[3] = w;
            polygon.Vertices[i] = &v;
            polygon.FinalZ[i] = 1000;
        }
        polygon.NumVertices = 3;
        polygon.TexParam = tex_param;
    }
    NdsGxSnapshotPolygon snapshot() const {
        NdsGxSnapshotPolygon out;
        nds_gx_snapshot_polygon(polygon, &out);
        return out;
    }
};

void test_blend_is_halfway() {
    const Triangle from(20, 40, 1, 0x1000), to(30, 60, 1, 0x3000);
    Polygon out;
    Vertex vertices[10];
    check(nds_gx_blend_polygon(from.snapshot(), to.polygon, 256, &out,
                               vertices), "close polygons blend");
    check(out.Vertices[0]->FinalPosition[0] == 25 &&
          out.Vertices[0]->FinalPosition[1] == 50, "position is halfway");
    check(out.Vertices[0]->HiresPosition[0] == 25 * 16, "hires is halfway");
    check(out.Vertices[0]->Position[3] == 0x2000, "W is halfway");
    check(out.YTop == 50 && out.YBottom == 60, "bounds follow the vertices");
    // Bounds of `to` itself are untouched.
    check(to.polygon.Vertices[0]->FinalPosition[0] == 30, "source unchanged");
}

void test_blend_refuses() {
    Polygon out;
    Vertex vertices[10];
    const Triangle near(20, 40), far(120, 40);
    check(!nds_gx_blend_polygon(far.snapshot(), near.polygon, 256, &out,
                                vertices), "a far polygon does not blend");
    Triangle quad(20, 40);
    quad.polygon.NumVertices = 4;
    quad.polygon.Vertices[3] = &quad.vertices[0];
    check(!nds_gx_blend_polygon(near.snapshot(), quad.polygon, 256, &out,
                                vertices), "vertex counts must agree");
}

void test_match_survives_polygons_leaving() {
    // Five polygons, the second gone from the next image, all moved 2 px.
    std::vector<Triangle> before, after;
    before.reserve(5);  // a Triangle points into itself
    after.reserve(4);
    for (int i = 0; i < 5; ++i) before.emplace_back(i * 40, 50, i + 1);
    for (int i : {0, 2, 3, 4}) after.emplace_back(i * 40 + 2, 50, i + 1);
    std::vector<NdsGxSnapshotPolygon> a, b;
    for (const Triangle& t : before) a.push_back(t.snapshot());
    for (const Triangle& t : after) b.push_back(t.snapshot());
    std::vector<int32_t> pairs(b.size());
    check(nds_gx_match(a.data(), a.size(), b.data(), b.size(),
                       pairs.data()) == 4, "the survivors pair");
    check(pairs[0] == 0 && pairs[1] == 2 && pairs[3] == 4,
          "pairs skip the polygon that left");
}

void test_match_needs_proximity() {
    // Same texture, too far apart: a different tile, not the same one moved.
    const auto a = Triangle(20, 40).snapshot();
    const auto b = Triangle(100, 40).snapshot();
    int32_t pair = 0;
    check(nds_gx_match(&a, 1, &b, 1, &pair) == 0 && pair == -1,
          "distant look-alikes do not pair");
}

void test_shift_keeps_screen_edges() {
    Triangle edge(0, 40);
    Polygon out;
    Vertex vertices[10];
    nds_gx_shift_polygon(edge.polygon, 4.f, 2.f, 256, &out, vertices);
    check(out.Vertices[0]->FinalPosition[0] == 0, "edge vertex stays");
    check(out.Vertices[1]->FinalPosition[0] == 14 &&
          out.Vertices[1]->FinalPosition[1] == 42, "inner vertex moves");
}

}  // namespace

int main() {
    test_blend_is_halfway();
    test_blend_refuses();
    test_match_survives_polygons_leaving();
    test_match_needs_proximity();
    test_shift_keeps_screen_edges();
    if (failures == 0) std::puts("gpu3d_interp_test: ok");
    return failures != 0;
}
