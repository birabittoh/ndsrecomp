#include "gpu3d_interp.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

using melonDS::GPU3D;
using melonDS::Polygon;
using melonDS::Vertex;

namespace {

constexpr size_t kMaxPolygons = 2048;
constexpr size_t kMaxVertices = 10;
constexpr size_t kMatchWindow = 64;
// From a capture of real play: 16 px removes nearly all pairings of two
// similar polygons, at the cost of 0.3% of the true ones.
constexpr float kMatchDistance = 16.f;
constexpr int32_t kMaxVertexTravel = 32;
constexpr int32_t kMaxTexCoordTravel = 1024;
constexpr float kMaxShift = 8.f;
// Below this share of paired polygons the two images are different scenes.
constexpr size_t kMinPairedPercent = 60;
// Past this many DS frames apart, the game was not animating between them.
constexpr uint64_t kMaxImageGap = 4;

template <typename T>
T midpoint(T a, T b) {
    return static_cast<T>(a + (b - a) / 2);
}

bool same_surface(const NdsGxSnapshotPolygon& a,
                  const NdsGxSnapshotPolygon& b) {
    // Polygon alpha is left out: fades change it on everything at once.
    constexpr uint32_t kAlphaMask = 0x1F0000u;
    return (a.attr & ~kAlphaMask) == (b.attr & ~kAlphaMask) &&
           a.tex_param == b.tex_param && a.tex_palette == b.tex_palette;
}

bool can_pair(const NdsGxSnapshotPolygon& a, const NdsGxSnapshotPolygon& b) {
    return same_surface(a, b) && std::fabs(a.x - b.x) <= kMatchDistance &&
           std::fabs(a.y - b.y) <= kMatchDistance;
}

int64_t signed_area(const Vertex* const* vertices, uint32_t count) {
    int64_t area = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const Vertex& p = *vertices[i];
        const Vertex& q = *vertices[(i + 1) % count];
        area += int64_t(p.FinalPosition[0]) * q.FinalPosition[1] -
                int64_t(q.FinalPosition[0]) * p.FinalPosition[1];
    }
    return area;
}

// The bounds and normalised W that GPU3D::SubmitPolygon works out, for
// vertices that have since moved.
void finish_polygon(Polygon* polygon, int32_t render_width) {
    uint32_t top = 0, bottom = 0, wsize = 0;
    int32_t ytop = 192, ybottom = 0, xtop = render_width, xbottom = 0;
    polygon->Degenerate = false;
    for (uint32_t i = 0; i < polygon->NumVertices; ++i) {
        const Vertex& vertex = *polygon->Vertices[i];
        const int32_t x = vertex.FinalPosition[0];
        const int32_t y = vertex.FinalPosition[1];
        if (y < ytop) {
            xtop = x;
            ytop = y;
            top = i;
        }
        if (y > ybottom || (y == ybottom && x > xbottom)) {
            xbottom = x;
            ybottom = y;
            bottom = i;
        }
        const uint32_t w = static_cast<uint32_t>(vertex.Position[3]);
        if (w == 0) polygon->Degenerate = true;
        while ((w >> wsize) && wsize < 32) wsize += 4;
    }
    polygon->VTop = top;
    polygon->VBottom = bottom;
    polygon->YTop = ytop;
    polygon->YBottom = ybottom;
    polygon->XTop = xtop;
    polygon->XBottom = xbottom;
    if (ybottom > 192) polygon->Degenerate = true;
    polygon->SortKey = (static_cast<uint32_t>(ybottom) << 8) |
                       static_cast<uint32_t>(ytop);
    if (polygon->Translucent) polygon->SortKey |= 0x10000u;
    for (uint32_t i = 0; i < polygon->NumVertices; ++i) {
        const int32_t w = polygon->Vertices[i]->Position[3];
        polygon->FinalW[i] = wsize < 16 ? w << (16 - wsize)
                                        : w >> (wsize - 16);
    }
}

struct InterpState {
    std::vector<NdsGxSnapshotPolygon> last =
        std::vector<NdsGxSnapshotPolygon>(kMaxPolygons);
    std::vector<NdsGxSnapshotPolygon> current =
        std::vector<NdsGxSnapshotPolygon>(kMaxPolygons);
    std::vector<int32_t> pairs = std::vector<int32_t>(kMaxPolygons);
    std::vector<uint8_t> blended = std::vector<uint8_t>(kMaxPolygons);
    std::vector<Polygon> polygons = std::vector<Polygon>(kMaxPolygons);
    std::vector<Vertex> vertices =
        std::vector<Vertex>(kMaxPolygons * kMaxVertices);
    std::vector<Polygon*> real = std::vector<Polygon*>(kMaxPolygons);
    size_t last_count = 0;
    bool last_valid = false;
    bool showing_blend = false;
    uint64_t vblank = 0;
    uint64_t last_vblank = 0;
};

InterpState& state() {
    static InterpState instance;
    return instance;
}

// Replaces the render list with one halfway from the last image to the one
// just flushed. The real list's order stands: it is sorted for the real
// positions, and half a step does not reorder anything that shows.
bool show_blend(GPU3D& gpu3d, const Polygon* bank, size_t count,
                NdsGxInterpStats* stats) {
    InterpState& s = state();
    const size_t paired = nds_gx_match(s.last.data(), s.last_count,
                                       s.current.data(), count,
                                       s.pairs.data());
    if (paired * 100 < count * kMinPairedPercent) return false;

    const int32_t width = static_cast<int32_t>(gpu3d.GetRenderWidth());
    size_t blended = 0;
    for (size_t i = 0; i < count; ++i) {
        const int32_t from = s.pairs[i];
        s.blended[i] = from >= 0 &&
            nds_gx_blend_polygon(s.last[size_t(from)], bank[i], width,
                                 &s.polygons[i],
                                 &s.vertices[i * kMaxVertices]);
        if (!s.blended[i]) s.pairs[i] = -1;
        else ++blended;
    }
    // The rest take the motion of the paired polygon drawn just before them,
    // which is usually the same object.
    const auto half_step = [&s](size_t i, float* dx, float* dy) {
        const NdsGxSnapshotPolygon& last = s.last[size_t(s.pairs[i])];
        *dx = (last.x - s.current[i].x) / 2.f;
        *dy = (last.y - s.current[i].y) / 2.f;
    };
    float dx = 0.f, dy = 0.f;
    const auto first = std::find(s.blended.begin(), s.blended.begin() + count,
                                 uint8_t{1});
    if (first != s.blended.begin() + count)
        half_step(size_t(first - s.blended.begin()), &dx, &dy);
    for (size_t i = 0; i < count; ++i) {
        if (s.blended[i])
            half_step(i, &dx, &dy);
        else
            nds_gx_shift_polygon(bank[i], dx, dy, width, &s.polygons[i],
                                 &s.vertices[i * kMaxVertices]);
    }
    for (size_t i = 0; i < count; ++i) {
        s.real[i] = gpu3d.RenderPolygonRAM[i];
        gpu3d.RenderPolygonRAM[i] =
            &s.polygons[size_t(s.real[i] - bank)];
    }
    s.showing_blend = true;
    if (stats) {
        ++stats->images;
        stats->polygons += count;
        stats->blended += blended;
    }
    return true;
}

}  // namespace

void nds_gx_snapshot_polygon(const Polygon& polygon,
                             NdsGxSnapshotPolygon* out) {
    out->attr = polygon.Attr;
    out->tex_param = polygon.TexParam;
    out->tex_palette = polygon.TexPalette;
    out->num_vertices = std::min<uint32_t>(polygon.NumVertices, kMaxVertices);
    float x = 0.f, y = 0.f;
    for (uint32_t i = 0; i < out->num_vertices; ++i) {
        out->vertices[i] = *polygon.Vertices[i];
        out->final_z[i] = polygon.FinalZ[i];
        x += static_cast<float>(polygon.Vertices[i]->FinalPosition[0]);
        y += static_cast<float>(polygon.Vertices[i]->FinalPosition[1]);
    }
    const float count = out->num_vertices ? float(out->num_vertices) : 1.f;
    out->x = x / count;
    out->y = y / count;
}

size_t nds_gx_match(const NdsGxSnapshotPolygon* a, size_t a_count,
                    const NdsGxSnapshotPolygon* b, size_t b_count,
                    int32_t* b_to_a) {
    std::fill(b_to_a, b_to_a + b_count, -1);
    size_t paired = 0, i = 0, j = 0;
    while (i < a_count && j < b_count) {
        if (can_pair(a[i], b[j])) {
            b_to_a[j++] = static_cast<int32_t>(i++);
            ++paired;
            continue;
        }
        bool resumed = false;
        for (size_t d = 1; d < kMatchWindow && !resumed; ++d) {
            if (i + d < a_count && can_pair(a[i + d], b[j])) {
                i += d;
                resumed = true;
            } else if (j + d < b_count && can_pair(a[i], b[j + d])) {
                j += d;
                resumed = true;
            }
        }
        if (!resumed) {
            ++i;
            ++j;
        }
    }
    return paired;
}

bool nds_gx_blend_polygon(const NdsGxSnapshotPolygon& from, const Polygon& to,
                          int32_t render_width, Polygon* out,
                          Vertex* vertices) {
    if (from.num_vertices != to.NumVertices ||
        to.NumVertices > kMaxVertices) {
        return false;
    }
    *out = to;
    for (uint32_t i = 0; i < to.NumVertices; ++i) {
        const Vertex& a = from.vertices[i];
        const Vertex& b = *to.Vertices[i];
        if (std::abs(a.FinalPosition[0] - b.FinalPosition[0]) >
                kMaxVertexTravel ||
            std::abs(a.FinalPosition[1] - b.FinalPosition[1]) >
                kMaxVertexTravel) {
            return false;
        }
        Vertex& mid = vertices[i];
        mid = b;
        for (int c = 0; c < 2; ++c) {
            mid.FinalPosition[c] =
                midpoint(a.FinalPosition[c], b.FinalPosition[c]);
            mid.HiresPosition[c] =
                midpoint(a.HiresPosition[c], b.HiresPosition[c]);
            // A scrolling texture wraps, and half of a wrap is not a step.
            if (std::abs(a.TexCoords[c] - b.TexCoords[c]) <=
                kMaxTexCoordTravel)
                mid.TexCoords[c] = midpoint(a.TexCoords[c], b.TexCoords[c]);
        }
        for (int c = 0; c < 3; ++c)
            mid.FinalColor[c] = midpoint(a.FinalColor[c], b.FinalColor[c]);
        mid.Position[3] = midpoint(a.Position[3], b.Position[3]);
        out->FinalZ[i] = midpoint(from.final_z[i], to.FinalZ[i]);
        out->Vertices[i] = &mid;
    }
    // The rasteriser takes the winding from FacingView, so a polygon that
    // turns over between the images cannot be drawn halfway.
    const int64_t before = signed_area(to.Vertices, to.NumVertices);
    const int64_t after = signed_area(out->Vertices, to.NumVertices);
    if ((before > 0 && after < 0) || (before < 0 && after > 0)) return false;
    finish_polygon(out, render_width);
    return true;
}

void nds_gx_shift_polygon(const Polygon& polygon, float dx, float dy,
                          int32_t render_width, Polygon* out,
                          Vertex* vertices) {
    *out = polygon;
    const int32_t shift_x = static_cast<int32_t>(
        std::lround(std::clamp(dx, -kMaxShift, kMaxShift)));
    const int32_t shift_y = static_cast<int32_t>(
        std::lround(std::clamp(dy, -kMaxShift, kMaxShift)));
    const uint32_t count = std::min<uint32_t>(polygon.NumVertices,
                                              kMaxVertices);
    for (uint32_t i = 0; i < count; ++i) {
        Vertex& vertex = vertices[i];
        vertex = *polygon.Vertices[i];
        out->Vertices[i] = &vertex;
        const int32_t x = vertex.FinalPosition[0];
        const int32_t y = vertex.FinalPosition[1];
        if (x <= 0 || x >= render_width - 1 || y <= 0 || y >= 191) continue;
        vertex.FinalPosition[0] += shift_x;
        vertex.FinalPosition[1] += shift_y;
        vertex.HiresPosition[0] += shift_x * 16;
        vertex.HiresPosition[1] += shift_y * 16;
    }
    finish_polygon(out, render_width);
}

void nds_gx_interp_show_real(GPU3D& gpu3d) {
    InterpState& s = state();
    if (!s.showing_blend) return;
    std::copy_n(s.real.begin(), gpu3d.RenderNumPolygons,
                gpu3d.RenderPolygonRAM.begin());
    gpu3d.RenderFrameIdentical = false;
    s.showing_blend = false;
}

void nds_gx_interp_reset() {
    InterpState& s = state();
    s.last_valid = false;
    s.showing_blend = false;
}

void nds_gx_interp_after_vblank(GPU3D& gpu3d, bool active, bool swap,
                                bool new_image, NdsGxInterpStats* stats) {
    InterpState& s = state();
    ++s.vblank;
    if (!active) {
        nds_gx_interp_show_real(gpu3d);
        s.last_valid = false;
        return;
    }
    if (!swap) {
        nds_gx_interp_show_real(gpu3d);
        return;
    }
    // GPU3D::VBlank has just rebuilt the render list from the bank.
    s.showing_blend = false;
    const size_t count = gpu3d.RenderNumPolygons;
    if (count == 0) {
        s.last_valid = false;
        return;
    }
    if (!new_image) return;

    // VBlank flipped the banks, so the flushed image is in the other one.
    const Polygon* bank = &gpu3d.PolygonRAM[gpu3d.CurRAMBank ? 0 : 2048];
    for (size_t i = 0; i < count; ++i)
        nds_gx_snapshot_polygon(bank[i], &s.current[i]);
    if (s.last_valid && s.vblank - s.last_vblank <= kMaxImageGap)
        show_blend(gpu3d, bank, count, stats);
    std::swap(s.last, s.current);
    s.last_count = count;
    s.last_valid = true;
    s.last_vblank = s.vblank;
}
