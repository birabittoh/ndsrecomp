#include "gpu3d_interp.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <numeric>
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
// Two polygons of any model usually share a texture and a neighbourhood, so
// only their outline can tell them apart once texcoords do not.
constexpr int32_t kMatchShape = 2;
// A polygon alone with its identity needs no such care, only a sanity limit.
constexpr float kIdentityDistance = 48.f;
constexpr int32_t kMaxVertexTravel = 64;
constexpr int32_t kMaxTexCoordTravel = 1024;
constexpr int32_t kMaxShift = 8 * 16;  // hires units
// Below this share of paired polygons the two images are different scenes.
constexpr size_t kMinPairedPercent = 60;
// Past this many DS frames apart, the game was not animating between them.
constexpr uint64_t kMaxImageGap = 4;
// Larger than any image's corners, so that the table cannot fill.
constexpr size_t kVertexTableSize = 1u << 15;
constexpr size_t kMaxVotes = 4;

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

bool near(const NdsGxSnapshotPolygon& a, const NdsGxSnapshotPolygon& b) {
    return std::fabs(a.x - b.x) <= kMatchDistance &&
           std::fabs(a.y - b.y) <= kMatchDistance;
}

bool same_texcoords(const Vertex& a, const Vertex& b) {
    return a.TexCoords[0] == b.TexCoords[0] &&
           a.TexCoords[1] == b.TexCoords[1];
}

bool same_shape(const NdsGxSnapshotPolygon& a,
                const NdsGxSnapshotPolygon& b) {
    if (a.num_vertices != b.num_vertices) return false;
    for (uint32_t v = 1; v < a.num_vertices; ++v) {
        for (int c = 0; c < 2; ++c) {
            const int32_t edge_a = a.vertices[v].FinalPosition[c] -
                                   a.vertices[0].FinalPosition[c];
            const int32_t edge_b = b.vertices[v].FinalPosition[c] -
                                   b.vertices[0].FinalPosition[c];
            if (std::abs(edge_a - edge_b) > kMatchShape) return false;
        }
    }
    return true;
}

// Corners of `b` that are also corners of `a`. Texcoords belong to the model,
// so they survive any motion, but clipping invents new ones.
uint32_t shared_corners(const NdsGxSnapshotPolygon& a,
                        const NdsGxSnapshotPolygon& b) {
    uint32_t shared = 0;
    for (uint32_t v = 0; v < b.num_vertices; ++v) {
        if (b.vertices[v].Clipped) continue;
        for (uint32_t u = 0; u < a.num_vertices; ++u) {
            if (!a.vertices[u].Clipped &&
                same_texcoords(a.vertices[u], b.vertices[v])) {
                ++shared;
                break;
            }
        }
    }
    return shared;
}

bool any_clipped(const NdsGxSnapshotPolygon& polygon) {
    for (uint32_t v = 0; v < polygon.num_vertices; ++v)
        if (polygon.vertices[v].Clipped) return true;
    return false;
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

// A vertex of the image being blended, shared by every polygon with a corner
// at that clip-space position. Each polygon that reaches it says where it was
// in the last image; the majority decides, once for all of them, so a mesh
// stays closed whatever happens to its polygons.
struct VertexSlot {
    struct Vote {
        int32_t polygon;
        int32_t corner;
        int32_t count;
    };
    int32_t position[4] = {};
    Vote votes[kMaxVotes] = {};
    uint32_t vote_count = 0;
    bool placed = false;
    bool clipped = false;
    int32_t hires_shift[2] = {};  // from the real position, when not voted
    const Vertex* to = nullptr;
    int32_t to_z = 0;
    const Vertex* from = nullptr;
    int32_t from_z = 0;
    int32_t final_position[2] = {};  // the answer, for every polygon here
    int32_t hires_position[2] = {};
    int32_t w = 0;
};

// A vertex of the last image: the slot it became, or for vertices of polygons
// that have left, how far their neighbours say it moved.
struct LastVertex {
    int32_t position[4] = {};
    bool clipped = false;
    int32_t slot = -1;
    bool placed = false;
    int32_t hires_shift[2] = {};
};

// Finds a vertex in a table of records carrying its clip-space position, adding
// it through `create` if it is new. Kept to the size of one image: `table` is
// larger than any image's corners, so it cannot fill.
template <typename Record, typename Create>
int32_t find_vertex(std::vector<uint16_t>& table, std::vector<Record>& records,
                    size_t& record_count, const Vertex& vertex,
                    Create create) {
    uint64_t hash = 1469598103934665603ull;
    for (int c = 0; c < 4; ++c) {
        hash = (hash ^ uint32_t(vertex.Position[c])) * 1099511628211ull;
        hash ^= hash >> 29;
    }
    hash = (hash << 1) | (vertex.Clipped ? 1 : 0);
    for (size_t i = hash & (kVertexTableSize - 1);;
         i = (i + 1) & (kVertexTableSize - 1)) {
        if (!table[i]) {
            Record& record = records[record_count];
            std::copy_n(vertex.Position, 4, record.position);
            record.clipped = vertex.Clipped;
            create(record);
            table[i] = static_cast<uint16_t>(++record_count);
            return static_cast<int32_t>(record_count - 1);
        }
        const int32_t index = table[i] - 1;
        const Record& record = records[size_t(index)];
        if (record.clipped == vertex.Clipped &&
            std::equal(record.position, record.position + 4,
                       vertex.Position)) {
            return index;
        }
    }
}

struct BlendScratch {
    std::vector<VertexSlot> slots =
        std::vector<VertexSlot>(kMaxPolygons * kMaxVertices);
    std::vector<uint16_t> table = std::vector<uint16_t>(kVertexTableSize);
    size_t slot_count = 0;
    std::vector<LastVertex> last_vertices =
        std::vector<LastVertex>(kMaxPolygons * kMaxVertices);
    std::vector<uint16_t> last_table = std::vector<uint16_t>(kVertexTableSize);
    size_t last_vertex_count = 0;
    std::vector<int32_t> corner_slot =
        std::vector<int32_t>(kMaxPolygons * kMaxVertices);
    std::vector<int8_t> corner_from =
        std::vector<int8_t>(kMaxPolygons * kMaxVertices);
    std::vector<int32_t> pairs = std::vector<int32_t>(kMaxPolygons);
    std::vector<uint8_t> last_paired = std::vector<uint8_t>(kMaxPolygons);
    std::vector<int32_t> departing;
    std::vector<int32_t> departing_corners =
        std::vector<int32_t>(kMaxPolygons * kMaxVertices);
};

BlendScratch& scratch() {
    static BlendScratch instance;
    return instance;
}

int32_t find_slot(BlendScratch& s, const Vertex& vertex, int32_t z) {
    return find_vertex(s.table, s.slots, s.slot_count, vertex,
                       [&vertex, z](VertexSlot& slot) {
        slot.vote_count = 0;
        slot.placed = false;
        slot.hires_shift[0] = slot.hires_shift[1] = 0;
        slot.to = &vertex;
        slot.to_z = z;
        slot.from = nullptr;
    });
}

int32_t find_last_vertex(BlendScratch& s, const Vertex& vertex) {
    return find_vertex(s.last_table, s.last_vertices, s.last_vertex_count,
                       vertex, [](LastVertex& record) {
        record.slot = -1;
        record.placed = false;
        record.hires_shift[0] = record.hires_shift[1] = 0;
    });
}

// For each corner of `to`, the corner of `from` that is the same model vertex,
// or -1. Clipped corners have no texcoords of their own to go by, so they
// follow the outline from a neighbour that does.
void map_corners(const NdsGxSnapshotPolygon& from,
                 const NdsGxSnapshotPolygon& to, int8_t* corners) {
    const uint32_t n = to.num_vertices, m = from.num_vertices;
    bool any = false;
    for (uint32_t v = 0; v < n; ++v) {
        corners[v] = -1;
        if (to.vertices[v].Clipped) continue;
        const uint32_t first = v < m ? v : 0;
        for (uint32_t k = 0; k < m; ++k) {
            const uint32_t u = (first + k) % m;
            if (!from.vertices[u].Clipped &&
                same_texcoords(from.vertices[u], to.vertices[v])) {
                corners[v] = int8_t(u);
                any = true;
                break;
            }
        }
    }
    if (!any) {
        // A scrolling texture, or nothing left unclipped: the pair stands on
        // its outline, corner for corner.
        if (n == m) {
            for (uint32_t v = 0; v < n; ++v) corners[v] = int8_t(v);
        }
        return;
    }
    for (uint32_t pass = 0; pass < n; ++pass) {
        for (uint32_t v = 0; v < n; ++v) {
            if (corners[v] >= 0 || !to.vertices[v].Clipped) continue;
            const int8_t before = corners[(v + n - 1) % n];
            const int8_t after = corners[(v + 1) % n];
            int32_t u = -1;
            if (before >= 0) u = (before + 1) % int32_t(m);
            else if (after >= 0) u = (after + int32_t(m) - 1) % int32_t(m);
            if (u >= 0 && from.vertices[u].Clipped) corners[v] = int8_t(u);
        }
    }
}

void add_vote(VertexSlot& slot, const Vertex& to,
              const NdsGxSnapshotPolygon* last, int32_t polygon,
              int32_t corner) {
    const Vertex& from = last[polygon].vertices[corner];
    if (std::abs(from.FinalPosition[0] - to.FinalPosition[0]) >
            kMaxVertexTravel ||
        std::abs(from.FinalPosition[1] - to.FinalPosition[1]) >
            kMaxVertexTravel) {
        return;
    }
    for (uint32_t i = 0; i < slot.vote_count; ++i) {
        VertexSlot::Vote& vote = slot.votes[i];
        const Vertex& other = last[vote.polygon].vertices[vote.corner];
        if (other.FinalPosition[0] == from.FinalPosition[0] &&
            other.FinalPosition[1] == from.FinalPosition[1]) {
            ++vote.count;
            return;
        }
    }
    if (slot.vote_count < kMaxVotes)
        slot.votes[slot.vote_count++] = {polygon, corner, 1};
}

struct InterpState {
    std::vector<NdsGxSnapshotPolygon> last =
        std::vector<NdsGxSnapshotPolygon>(kMaxPolygons);
    std::vector<NdsGxSnapshotPolygon> current =
        std::vector<NdsGxSnapshotPolygon>(kMaxPolygons);
    std::vector<Polygon> polygons = std::vector<Polygon>(kMaxPolygons);
    std::vector<Vertex> vertices =
        std::vector<Vertex>(kMaxPolygons * kMaxVertices);
    std::vector<Polygon*> real = std::vector<Polygon*>(kMaxPolygons);
    size_t last_count = 0;
    size_t real_count = 0;
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
// positions, and half a step does not reorder anything that shows. Departing
// polygons are all opaque and join the end of the opaque ones.
bool show_blend(GPU3D& gpu3d, const Polygon* bank, size_t count,
                NdsGxInterpStats* stats) {
    InterpState& s = state();
    NdsGxBlendCounts counts;
    if (!nds_gx_blend_image(s.last.data(), s.last_count, s.current.data(),
                            bank, count,
                            static_cast<int32_t>(gpu3d.GetRenderWidth()),
                            s.polygons.data(), s.vertices.data(), &counts)) {
        return false;
    }
    std::copy_n(gpu3d.RenderPolygonRAM.begin(), count, s.real.begin());
    size_t opaque = 0;
    while (opaque < count && !s.real[opaque]->Translucent) ++opaque;
    for (size_t i = 0; i < count; ++i) {
        gpu3d.RenderPolygonRAM[i < opaque ? i : i + counts.departing] =
            &s.polygons[size_t(s.real[i] - bank)];
    }
    for (size_t i = 0; i < counts.departing; ++i)
        gpu3d.RenderPolygonRAM[opaque + i] = &s.polygons[count + i];
    gpu3d.RenderNumPolygons = static_cast<uint32_t>(count + counts.departing);
    s.real_count = count;
    s.showing_blend = true;
    if (stats) {
        ++stats->images;
        stats->polygons += count;
        stats->blended += counts.paired;
    }
    return true;
}

}  // namespace

void nds_gx_snapshot_polygon(const Polygon& polygon,
                             NdsGxSnapshotPolygon* out) {
    out->source = polygon;
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

    constexpr uint32_t kAlphaMask = 0x1F0000u;
    uint64_t identity = 1469598103934665603ull;
    const auto mix = [&identity](uint32_t value) {
        identity = (identity ^ value) * 1099511628211ull;
    };
    mix(out->attr & ~kAlphaMask);
    mix(out->tex_param);
    mix(out->tex_palette);
    mix(out->num_vertices);
    for (uint32_t i = 0; i < out->num_vertices; ++i) {
        mix(uint16_t(out->vertices[i].TexCoords[0]));
        mix(uint16_t(out->vertices[i].TexCoords[1]));
    }
    out->identity = identity;
}

size_t nds_gx_match(const NdsGxSnapshotPolygon* a, size_t a_count,
                    const NdsGxSnapshotPolygon* b, size_t b_count,
                    int32_t* b_to_a) {
    std::fill(b_to_a, b_to_a + b_count, -1);
    std::vector<uint32_t> a_order(a_count), b_order(b_count);
    std::iota(a_order.begin(), a_order.end(), 0u);
    std::iota(b_order.begin(), b_order.end(), 0u);
    std::sort(a_order.begin(), a_order.end(), [a](uint32_t l, uint32_t r) {
        return a[l].identity != a[r].identity
            ? a[l].identity < a[r].identity : l < r;
    });
    std::sort(b_order.begin(), b_order.end(), [b](uint32_t l, uint32_t r) {
        return b[l].identity != b[r].identity
            ? b[l].identity < b[r].identity : l < r;
    });
    std::vector<uint8_t> a_used(a_count);
    size_t paired = 0;
    const auto pair = [&](size_t i, size_t j) {
        b_to_a[j] = static_cast<int32_t>(i);
        a_used[i] = 1;
        ++paired;
    };

    // A polygon alone with its identity in both images is itself.
    struct Group {
        uint32_t a_begin, a_end, b_begin, b_end;
    };
    std::vector<Group> repeats;
    for (size_t i = 0, j = 0; i < a_count && j < b_count;) {
        const uint64_t id_a = a[a_order[i]].identity;
        const uint64_t id_b = b[b_order[j]].identity;
        if (id_a < id_b) {
            ++i;
        } else if (id_b < id_a) {
            ++j;
        } else {
            size_t i_end = i + 1, j_end = j + 1;
            while (i_end < a_count && a[a_order[i_end]].identity == id_a)
                ++i_end;
            while (j_end < b_count && b[b_order[j_end]].identity == id_b)
                ++j_end;
            if (i_end - i == 1 && j_end - j == 1) {
                const NdsGxSnapshotPolygon& from = a[a_order[i]];
                const NdsGxSnapshotPolygon& to = b[b_order[j]];
                if (std::fabs(from.x - to.x) <= kIdentityDistance &&
                    std::fabs(from.y - to.y) <= kIdentityDistance) {
                    pair(a_order[i], b_order[j]);
                }
            } else {
                repeats.push_back({uint32_t(i), uint32_t(i_end), uint32_t(j),
                                   uint32_t(j_end)});
            }
            i = i_end;
            j = j_end;
        }
    }

    // What the pairs so far say about each polygon's motion: the closest one
    // submitted before it, which is usually the same object.
    std::vector<float> motion_x(b_count), motion_y(b_count);
    const auto update_motion = [&] {
        float x = 0.f, y = 0.f;
        size_t first = 0;
        while (first < b_count && b_to_a[first] < 0) ++first;
        if (first < b_count) {
            x = a[b_to_a[first]].x - b[first].x;
            y = a[b_to_a[first]].y - b[first].y;
        }
        for (size_t j = 0; j < b_count; ++j) {
            if (b_to_a[j] >= 0) {
                x = a[b_to_a[j]].x - b[j].x;
                y = a[b_to_a[j]].y - b[j].y;
            }
            motion_x[j] = x;
            motion_y[j] = y;
        }
    };

    // Repeated identities, such as tiles: the look-alike that fits that motion.
    update_motion();
    for (const Group& group : repeats) {
        for (uint32_t q = group.b_begin; q < group.b_end; ++q) {
            const uint32_t j = b_order[q];
            int32_t best = -1;
            float best_distance = 0.f;
            for (uint32_t p = group.a_begin; p < group.a_end; ++p) {
                const uint32_t i = a_order[p];
                if (a_used[i] || !near(a[i], b[j])) continue;
                const float distance =
                    std::fabs(a[i].x - b[j].x - motion_x[j]) +
                    std::fabs(a[i].y - b[j].y - motion_y[j]);
                if (best < 0 || distance < best_distance) {
                    best = static_cast<int32_t>(i);
                    best_distance = distance;
                }
            }
            if (best >= 0) pair(size_t(best), j);
        }
    }

    // The rest changed identity: clipped differently, or a moving texture.
    // Submission order narrows them down, and they must still be the same
    // surface with corners or an outline in common.
    update_motion();
    size_t next = 0;
    for (size_t j = 0; j < b_count; ++j) {
        if (b_to_a[j] >= 0) {
            next = size_t(b_to_a[j]) + 1;
            continue;
        }
        const size_t begin = next > kMatchWindow ? next - kMatchWindow : 0;
        const size_t end = std::min(a_count, next + kMatchWindow);
        int32_t best = -1;
        uint32_t best_shared = 0;
        float best_distance = 0.f;
        for (size_t i = begin; i < end; ++i) {
            if (a_used[i] || !same_surface(a[i], b[j]) || !near(a[i], b[j]))
                continue;
            const bool clipped = any_clipped(a[i]) || any_clipped(b[j]);
            const uint32_t shared = shared_corners(a[i], b[j]);
            if (!(clipped && shared > 0) && !same_shape(a[i], b[j])) continue;
            const float distance = std::fabs(a[i].x - b[j].x - motion_x[j]) +
                                   std::fabs(a[i].y - b[j].y - motion_y[j]);
            if (best < 0 || shared > best_shared ||
                (shared == best_shared && distance < best_distance)) {
                best = static_cast<int32_t>(i);
                best_shared = shared;
                best_distance = distance;
            }
        }
        if (best >= 0) {
            pair(size_t(best), j);
            next = size_t(best) + 1;
        }
    }
    return paired;
}

namespace {

void vote_on_vertices(BlendScratch& s, const NdsGxSnapshotPolygon* last,
                      const NdsGxSnapshotPolygon* current, size_t count) {
    std::fill(s.table.begin(), s.table.end(), uint16_t{0});
    s.slot_count = 0;
    for (size_t j = 0; j < count; ++j) {
        const uint32_t n = current[j].num_vertices;
        int32_t* corner_slot = &s.corner_slot[j * kMaxVertices];
        int8_t* corner_from = &s.corner_from[j * kMaxVertices];
        for (uint32_t v = 0; v < n; ++v) {
            corner_slot[v] = find_slot(s, current[j].vertices[v],
                                       current[j].final_z[v]);
            corner_from[v] = -1;
        }
        if (s.pairs[j] < 0) continue;
        map_corners(last[s.pairs[j]], current[j], corner_from);
        for (uint32_t v = 0; v < n; ++v) {
            if (corner_from[v] >= 0) {
                add_vote(s.slots[corner_slot[v]], current[j].vertices[v],
                         last, s.pairs[j], corner_from[v]);
            }
        }
    }
    for (size_t index = 0; index < s.slot_count; ++index) {
        VertexSlot& slot = s.slots[index];
        if (!slot.vote_count) continue;
        const VertexSlot::Vote* best = &slot.votes[0];
        for (uint32_t i = 1; i < slot.vote_count; ++i)
            if (slot.votes[i].count > best->count) best = &slot.votes[i];
        slot.from = &last[best->polygon].vertices[best->corner];
        slot.from_z = last[best->polygon].final_z[best->corner];
        slot.placed = true;
    }
}

// The mean of what is known about a polygon's corners. `step` writes one
// corner's motion and returns false if there is none yet.
template <typename Step>
bool mean_motion(uint32_t corners, Step step, int32_t* shift) {
    int32_t sum[2] = {}, known = 0;
    for (uint32_t v = 0; v < corners; ++v) {
        int32_t corner[2];
        if (!step(v, corner)) continue;
        sum[0] += corner[0];
        sum[1] += corner[1];
        ++known;
    }
    if (!known) return false;
    shift[0] = sum[0] / known;
    shift[1] = sum[1] / known;
    return true;
}

// Vertices nothing voted for take the motion of those beside them in the mesh,
// spreading outwards until it runs out. Whole objects without a pair follow
// whatever was submitted before them.
void place_remaining_vertices(BlendScratch& s,
                              const NdsGxSnapshotPolygon* current,
                              size_t count) {
    const auto polygon_motion = [&](size_t j, int32_t* shift) {
        return mean_motion(current[j].num_vertices,
                           [&](uint32_t v, int32_t* corner) {
            const VertexSlot& slot =
                s.slots[s.corner_slot[j * kMaxVertices + v]];
            if (!slot.placed) return false;
            for (int c = 0; c < 2; ++c) {
                corner[c] = slot.from
                    ? (slot.from->HiresPosition[c] -
                       slot.to->HiresPosition[c]) / 2
                    : slot.hires_shift[c];
            }
            return true;
        }, shift);
    };
    const auto fill_polygon = [&](size_t j, const int32_t* shift) {
        bool changed = false;
        for (uint32_t v = 0; v < current[j].num_vertices; ++v) {
            VertexSlot& slot = s.slots[s.corner_slot[j * kMaxVertices + v]];
            if (slot.placed) continue;
            slot.placed = changed = true;
            slot.hires_shift[0] = shift[0];
            slot.hires_shift[1] = shift[1];
        }
        return changed;
    };
    for (bool changed = true, forward = true; changed; forward = !forward) {
        changed = false;
        for (size_t k = 0; k < count; ++k) {
            const size_t j = forward ? k : count - 1 - k;
            int32_t shift[2];
            if (polygon_motion(j, shift)) changed |= fill_polygon(j, shift);
        }
    }
    int32_t shift[2] = {};
    for (size_t j = 0; j < count; ++j) {
        if (polygon_motion(j, shift)) continue;
        const int32_t clamped[2] = {
            std::clamp(shift[0], -kMaxShift, kMaxShift),
            std::clamp(shift[1], -kMaxShift, kMaxShift)};
        fill_polygon(j, clamped);
    }
}

void resolve_vertices(BlendScratch& s, int32_t render_width) {
    for (size_t index = 0; index < s.slot_count; ++index) {
        VertexSlot& slot = s.slots[index];
        const Vertex& to = *slot.to;
        slot.w = to.Position[3];
        for (int c = 0; c < 2; ++c) {
            slot.final_position[c] = to.FinalPosition[c];
            slot.hires_position[c] = to.HiresPosition[c];
        }
        if (slot.from) {
            slot.w = midpoint(slot.from->Position[3], to.Position[3]);
            for (int c = 0; c < 2; ++c) {
                slot.final_position[c] = midpoint(slot.from->FinalPosition[c],
                                                  to.FinalPosition[c]);
                slot.hires_position[c] = midpoint(slot.from->HiresPosition[c],
                                                  to.HiresPosition[c]);
            }
            continue;
        }
        for (int c = 0; c < 2; ++c) {
            // A clipped vertex reaches the screen edge and has to stay there,
            // whichever way the mesh moves along it.
            const int32_t limit = c ? 192 : render_width;
            if (slot.clipped && (to.FinalPosition[c] <= 0 ||
                                 to.FinalPosition[c] >= limit - 1))
                continue;
            slot.hires_position[c] = std::clamp(
                to.HiresPosition[c] + slot.hires_shift[c], 0, limit * 16);
            slot.final_position[c] = slot.hires_position[c] / 16;
        }
    }
}

void write_polygons(BlendScratch& s, const NdsGxSnapshotPolygon* last,
                    const NdsGxSnapshotPolygon* current, const Polygon* bank,
                    size_t count, int32_t render_width, Polygon* polygons,
                    Vertex* vertices) {
    for (size_t j = 0; j < count; ++j) {
        const Polygon& to = bank[j];
        Polygon* out = &polygons[j];
        *out = to;
        const uint32_t n = current[j].num_vertices;
        for (uint32_t v = 0; v < n; ++v) {
            const VertexSlot& slot =
                s.slots[s.corner_slot[j * kMaxVertices + v]];
            const int8_t corner = s.corner_from[j * kMaxVertices + v];
            const Vertex& b = *to.Vertices[v];
            Vertex& mid = vertices[j * kMaxVertices + v];
            mid = b;
            out->Vertices[v] = &mid;
            for (int c = 0; c < 2; ++c) {
                mid.FinalPosition[c] = slot.final_position[c];
                mid.HiresPosition[c] = slot.hires_position[c];
            }
            mid.Position[3] = slot.w;
            if (slot.from)
                out->FinalZ[v] = midpoint(slot.from_z, to.FinalZ[v]);
            if (corner < 0) continue;
            const Vertex& a = last[s.pairs[j]].vertices[corner];
            for (int c = 0; c < 2; ++c) {
                // A scrolling texture wraps, and half of a wrap is not a step.
                if (std::abs(a.TexCoords[c] - b.TexCoords[c]) <=
                    kMaxTexCoordTravel)
                    mid.TexCoords[c] = midpoint(a.TexCoords[c], b.TexCoords[c]);
            }
            for (int c = 0; c < 3; ++c)
                mid.FinalColor[c] = midpoint(a.FinalColor[c], b.FinalColor[c]);
        }
        // The rasterisers take the winding from FacingView, and a polygon
        // turning over between the images has the other one halfway.
        const int64_t before = signed_area(to.Vertices, n);
        const int64_t after = signed_area(out->Vertices, n);
        if ((before > 0 && after < 0) || (before < 0 && after > 0)) {
            std::reverse(out->Vertices, out->Vertices + n);
            std::reverse(out->FinalZ, out->FinalZ + n);
        }
        finish_polygon(out, render_width);
    }
}

// Polygons of the last image that the new one lacks. Most have turned away
// from the camera or gone off screen, and halfway there many still show: the
// mesh has a hole without them. Their corners are shared with polygons that
// stayed, which says where they go. An object that went away whole shares
// nothing, and goes with it.
size_t write_departing_polygons(BlendScratch& s,
                                const NdsGxSnapshotPolygon* last,
                                size_t last_count,
                                const NdsGxSnapshotPolygon* current,
                                size_t count, int32_t render_width,
                                Polygon* polygons, Vertex* vertices) {
    std::fill_n(s.last_paired.begin(), last_count, uint8_t{0});
    for (size_t j = 0; j < count; ++j)
        if (s.pairs[j] >= 0) s.last_paired[size_t(s.pairs[j])] = 1;

    std::fill(s.last_table.begin(), s.last_table.end(), uint16_t{0});
    s.last_vertex_count = 0;
    for (size_t j = 0; j < count; ++j) {
        if (s.pairs[j] < 0) continue;
        const NdsGxSnapshotPolygon& from = last[s.pairs[j]];
        for (uint32_t v = 0; v < current[j].num_vertices; ++v) {
            const int8_t corner = s.corner_from[j * kMaxVertices + v];
            if (corner < 0) continue;
            const int32_t slot = s.corner_slot[j * kMaxVertices + v];
            if (s.slots[size_t(slot)].from != &from.vertices[corner]) continue;
            LastVertex& vertex = s.last_vertices[size_t(
                find_last_vertex(s, from.vertices[corner]))];
            vertex.slot = slot;
            vertex.placed = true;
        }
    }
    s.departing.clear();
    for (size_t i = 0; i < last_count; ++i) {
        const NdsGxSnapshotPolygon& polygon = last[i];
        if (s.last_paired[i] || polygon.source.Translucent ||
            polygon.source.IsShadow || polygon.source.IsShadowMask) {
            continue;
        }
        s.departing.push_back(int32_t(i));
        for (uint32_t v = 0; v < polygon.num_vertices; ++v) {
            s.departing_corners[i * kMaxVertices + v] =
                find_last_vertex(s, polygon.vertices[v]);
        }
    }

    const auto step = [&](const LastVertex& vertex, const Vertex& from,
                          int32_t* corner) {
        if (!vertex.placed) return false;
        for (int c = 0; c < 2; ++c) {
            corner[c] = vertex.slot >= 0
                ? s.slots[size_t(vertex.slot)].hires_position[c] -
                  from.HiresPosition[c]
                : vertex.hires_shift[c];
        }
        return true;
    };
    for (bool changed = true, forward = true; changed; forward = !forward) {
        changed = false;
        for (size_t k = 0; k < s.departing.size(); ++k) {
            const size_t i = size_t(
                s.departing[forward ? k : s.departing.size() - 1 - k]);
            const NdsGxSnapshotPolygon& polygon = last[i];
            const int32_t* corners = &s.departing_corners[i * kMaxVertices];
            int32_t shift[2];
            if (!mean_motion(polygon.num_vertices,
                             [&](uint32_t v, int32_t* corner) {
                    return step(s.last_vertices[size_t(corners[v])],
                                polygon.vertices[v], corner);
                }, shift)) {
                continue;
            }
            for (uint32_t v = 0; v < polygon.num_vertices; ++v) {
                LastVertex& vertex = s.last_vertices[size_t(corners[v])];
                if (vertex.placed) continue;
                vertex.placed = changed = true;
                vertex.hires_shift[0] = shift[0];
                vertex.hires_shift[1] = shift[1];
            }
        }
    }

    size_t written = 0;
    for (const int32_t index : s.departing) {
        const size_t i = size_t(index);
        if (count + written >= kMaxPolygons) break;
        const NdsGxSnapshotPolygon& polygon = last[i];
        const int32_t* corners = &s.departing_corners[i * kMaxVertices];
        const size_t at = count + written;
        Polygon* out = &polygons[at];
        *out = polygon.source;
        bool placed = true;
        const Vertex* real[kMaxVertices];
        for (uint32_t v = 0; v < polygon.num_vertices && placed; ++v) {
            const LastVertex& record = s.last_vertices[size_t(corners[v])];
            placed = record.placed;
            const Vertex& a = polygon.vertices[v];
            real[v] = &a;
            Vertex& mid = vertices[at * kMaxVertices + v];
            mid = a;
            out->Vertices[v] = &mid;
            if (record.slot >= 0) {
                const VertexSlot& slot = s.slots[size_t(record.slot)];
                for (int c = 0; c < 2; ++c) {
                    mid.FinalPosition[c] = slot.final_position[c];
                    mid.HiresPosition[c] = slot.hires_position[c];
                }
                mid.Position[3] = slot.w;
                out->FinalZ[v] = midpoint(polygon.final_z[v], slot.to_z);
            } else {
                for (int c = 0; c < 2; ++c) {
                    const int32_t limit = c ? 192 : render_width;
                    mid.HiresPosition[c] = std::clamp(
                        a.HiresPosition[c] + record.hires_shift[c], 0,
                        limit * 16);
                    mid.FinalPosition[c] = mid.HiresPosition[c] / 16;
                }
            }
        }
        if (!placed) continue;
        // The hardware's own test: past edge-on it would have been culled.
        const int64_t before = signed_area(real, polygon.num_vertices);
        const int64_t after = signed_area(out->Vertices, polygon.num_vertices);
        if ((before > 0) != (after > 0) || after == 0) continue;
        finish_polygon(out, render_width);
        ++written;
    }
    return written;
}

}  // namespace

bool nds_gx_blend_image(const NdsGxSnapshotPolygon* last, size_t last_count,
                        const NdsGxSnapshotPolygon* current,
                        const Polygon* bank, size_t count,
                        int32_t render_width, Polygon* polygons,
                        Vertex* vertices, NdsGxBlendCounts* counts) {
    BlendScratch& s = scratch();
    const size_t paired = nds_gx_match(last, last_count, current, count,
                                       s.pairs.data());
    if (paired * 100 < count * kMinPairedPercent) return false;
    vote_on_vertices(s, last, current, count);
    place_remaining_vertices(s, current, count);
    resolve_vertices(s, render_width);
    write_polygons(s, last, current, bank, count, render_width, polygons,
                   vertices);
    counts->paired = paired;
    counts->departing = write_departing_polygons(
        s, last, last_count, current, count, render_width, polygons, vertices);
    return true;
}

void nds_gx_interp_show_real(GPU3D& gpu3d) {
    InterpState& s = state();
    if (!s.showing_blend) return;
    std::copy_n(s.real.begin(), s.real_count, gpu3d.RenderPolygonRAM.begin());
    gpu3d.RenderNumPolygons = static_cast<uint32_t>(s.real_count);
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
