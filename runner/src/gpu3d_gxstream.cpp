#include "gpu3d_gxstream.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "NDS.h"

using melonDS::GPU3D;
using melonDS::Polygon;
using melonDS::Vertex;

namespace {

bool is_matrix(uint8_t op) { return op >= 0x10 && op <= 0x1C; }

// Loads and multiplies whose parameters are matrix entries.
bool is_blended(uint8_t op) { return op >= 0x16 && op <= 0x1C; }

// Commands whose parameters change between images without the thing drawn
// being another one: colours, materials and lights fade or flash, textures
// animate, tests vary. The newer image's values are used as they are.
bool is_soft(uint8_t op) {
    switch (op) {
        case 0x20: case 0x2A: case 0x2B: case 0x30: case 0x31: case 0x32:
        case 0x33: case 0x34: case 0x60: case 0x70: case 0x71: case 0x72:
        case 0x50:
            return true;
        default:
            return false;
    }
}

// Whether parameter `i` of a matrix command is a translation, which has no
// natural scale, rather than a rotation, scale or projection entry.
bool is_translation(uint8_t op, uint32_t i) {
    switch (op) {
        case 0x16: case 0x18: return i >= 12 && i <= 14;
        case 0x17: case 0x19: return i >= 9;
        case 0x1C: return true;
        default: return false;
    }
}

// How far one entry may move between two images and still be the same thing
// moving. Generous: a wrong match among look-alikes is avoided by taking the
// one that moved least, not by this limit. 20.12 fixed point.
bool plausible(int32_t a, int32_t b, bool translation) {
    const int64_t delta = std::llabs(int64_t(b) - int64_t(a));
    const int64_t magnitude = std::max(std::llabs(int64_t(a)), std::llabs(int64_t(b)));
    if (translation) return delta <= magnitude / 2 + 0x4000;
    return delta <= std::max<int64_t>(0x800, magnitude / 2);
}

int32_t blend(int32_t a, int32_t b, float t) {
    return static_cast<int32_t>(
        int64_t(a) + std::llround(double(int64_t(b) - int64_t(a)) * double(t)));
}

// Where the 3x3 linear part of a matrix command's parameters sits: the
// first entry of each row, or none (scale, translation, 3-row-less forms).
// The DS multiplies row vectors, so each row is a local axis.
struct Layout {
    int rows[3] = {-1, -1, -1};
    int stride = 0;  // entries per row, 3 or 4
};

Layout layout_of(uint8_t op) {
    Layout l;
    switch (op) {
        case 0x16: case 0x18: l = {{0, 4, 8}, 4}; break;
        case 0x17: case 0x19: case 0x1A: l = {{0, 3, 6}, 3}; break;
        default: break;
    }
    return l;
}

// A 3x3 block as rotation times a scale per row (local axis), which is what
// a model's bones, a camera and a scaled model are made of.
struct Pose {
    double q[4] = {};  // unit quaternion w, x, y, z
    double scale[3] = {};
};

bool decompose(const int32_t* m, const Layout& l, Pose* out) {
    double u[3][3];
    for (int r = 0; r < 3; ++r) {
        const int32_t* row = m + l.rows[r];
        const double x = row[0], y = row[1], z = row[2];
        const double len = std::sqrt(x * x + y * y + z * z);
        // Below 1/64: degenerate or a projection's rows, not a pose.
        if (len < 64.0) return false;
        out->scale[r] = len;
        u[r][0] = x / len;
        u[r][1] = y / len;
        u[r][2] = z / len;
    }
    // Orthogonal rows only: shear does not survive this decomposition.
    for (int a = 0; a < 3; ++a)
        for (int b = a + 1; b < 3; ++b)
            if (std::fabs(u[a][0] * u[b][0] + u[a][1] * u[b][1] + u[a][2] * u[b][2]) > 0.02)
                return false;
    const double det = u[0][0] * (u[1][1] * u[2][2] - u[1][2] * u[2][1]) -
                       u[0][1] * (u[1][0] * u[2][2] - u[1][2] * u[2][0]) +
                       u[0][2] * (u[1][0] * u[2][1] - u[1][1] * u[2][0]);
    if (det < 0) {
        // A mirror: carried by the last axis's scale.
        out->scale[2] = -out->scale[2];
        for (double& v : u[2]) v = -v;
    }
    // Quaternion of the rotation whose rows are u (Shepperd).
    double* q = out->q;
    const double tr = u[0][0] + u[1][1] + u[2][2];
    if (tr > 0) {
        const double k = std::sqrt(tr + 1.0) * 2;
        q[0] = 0.25 * k;
        q[1] = (u[1][2] - u[2][1]) / k;
        q[2] = (u[2][0] - u[0][2]) / k;
        q[3] = (u[0][1] - u[1][0]) / k;
    } else if (u[0][0] > u[1][1] && u[0][0] > u[2][2]) {
        const double k = std::sqrt(1.0 + u[0][0] - u[1][1] - u[2][2]) * 2;
        q[0] = (u[1][2] - u[2][1]) / k;
        q[1] = 0.25 * k;
        q[2] = (u[1][0] + u[0][1]) / k;
        q[3] = (u[2][0] + u[0][2]) / k;
    } else if (u[1][1] > u[2][2]) {
        const double k = std::sqrt(1.0 + u[1][1] - u[0][0] - u[2][2]) * 2;
        q[0] = (u[2][0] - u[0][2]) / k;
        q[1] = (u[1][0] + u[0][1]) / k;
        q[2] = 0.25 * k;
        q[3] = (u[2][1] + u[1][2]) / k;
    } else {
        const double k = std::sqrt(1.0 + u[2][2] - u[0][0] - u[1][1]) * 2;
        q[0] = (u[0][1] - u[1][0]) / k;
        q[1] = (u[2][0] + u[0][2]) / k;
        q[2] = (u[2][1] + u[1][2]) / k;
        q[3] = 0.25 * k;
    }
    const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int i = 0; i < 4; ++i) q[i] /= n;
    return true;
}

// The poses of one thing moving: at most this much rotation and scale change
// between two images. A limb in a run turns about 40 degrees per image at 30.
constexpr double kMaxTurnCos = 0.5;  // cos of half of 120 degrees
bool plausible_poses(const Pose& a, const Pose& b) {
    double dot = 0;
    for (int i = 0; i < 4; ++i) dot += a.q[i] * b.q[i];
    if (std::fabs(dot) < kMaxTurnCos) return false;
    for (int r = 0; r < 3; ++r) {
        if ((a.scale[r] < 0) != (b.scale[r] < 0)) return false;
        const double ratio = std::fabs(b.scale[r]) / std::fabs(a.scale[r]);
        if (ratio < 0.5 || ratio > 2.0) return false;
    }
    return true;
}

void write_pose(const Pose& a, const Pose& b, double t, const Layout& l, int32_t* out) {
    // Slerp, the short way round.
    double qb[4];
    double dot = 0;
    for (int i = 0; i < 4; ++i) dot += a.q[i] * b.q[i];
    const double sign = dot < 0 ? -1.0 : 1.0;
    for (int i = 0; i < 4; ++i) qb[i] = b.q[i] * sign;
    dot = std::fabs(dot);
    double q[4];
    if (dot > 0.9995) {
        for (int i = 0; i < 4; ++i) q[i] = a.q[i] + (qb[i] - a.q[i]) * t;
    } else {
        const double theta = std::acos(dot);
        const double wa = std::sin((1 - t) * theta) / std::sin(theta);
        const double wb = std::sin(t * theta) / std::sin(theta);
        for (int i = 0; i < 4; ++i) q[i] = a.q[i] * wa + qb[i] * wb;
    }
    const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    const double w = q[0] / n, x = q[1] / n, y = q[2] / n, z = q[3] / n;
    // Rows of the rotation, matching decompose().
    const double rot[3][3] = {
        {1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y)},
        {2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x)},
        {2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)},
    };
    for (int r = 0; r < 3; ++r) {
        const double s = a.scale[r] + (b.scale[r] - a.scale[r]) * t;
        for (int c = 0; c < 3; ++c) out[l.rows[r] + c] = int32_t(std::llround(rot[r][c] * s));
    }
}

// The parameters of one matrix command `t` of the way from a to b, or false
// when the two are not one thing moving. `out` may be null (check only) and
// may alias b.
//
// The 3x3 part of a load or multiply is blended as rotation (slerp) and
// scale, so a fast-turning bone neither shrinks halfway nor is refused;
// anything else (projections, sheared matrices, the rest of the entries) is
// blended linearly and judged per entry.
bool blend_command(uint8_t op, const int32_t* a, const int32_t* b, uint32_t n, float t,
                   int32_t* out) {
    const Layout l = layout_of(op);
    Pose pa, pb;
    bool posed = l.stride && n >= uint32_t(l.rows[2] + 3) && decompose(a, l, &pa) &&
                 decompose(b, l, &pb);
    if (posed && !plausible_poses(pa, pb)) return false;
    // Under about 3.6 degrees, linear blending shrinks nothing visible and,
    // unlike rebuilding the rotation, keeps a still matrix exactly as it is.
    bool turning = false;
    if (posed) {
        double dot = 0;
        for (int i = 0; i < 4; ++i) dot += pa.q[i] * pb.q[i];
        turning = std::fabs(dot) <= 0.9995;
    }
    auto in_pose = [&](uint32_t e) {
        if (!posed) return false;
        for (int r = 0; r < 3; ++r)
            if (e >= uint32_t(l.rows[r]) && e < uint32_t(l.rows[r] + 3)) return true;
        return false;
    };
    for (uint32_t e = 0; e < n; ++e)
        if (!in_pose(e) && !plausible(a[e], b[e], is_translation(op, e))) return false;
    if (!out) return true;
    int32_t rotated[16];
    if (turning) write_pose(pa, pb, t, l, rotated);
    for (uint32_t e = 0; e < n; ++e)
        out[e] = turning && in_pose(e) ? rotated[e] : blend(a[e], b[e], t);
    return true;
}

uint64_t mix(uint64_t hash, uint32_t value) {
    hash ^= value;
    hash *= 1099511628211ull;
    return hash ^ (hash >> 31);
}

struct Unit {
    uint32_t begin = 0, end = 0;  // command range
    uint64_t signature = 0;
    bool moving = false;          // has a blended command outside texture mode
};

// The matrix mode each command runs in.
void matrix_modes(const NdsGxStream& s, std::vector<uint8_t>* modes) {
    modes->resize(s.size());
    uint8_t mode = uint8_t(s.start.matrix_mode & 3);
    for (size_t i = 0; i < s.size(); ++i) {
        (*modes)[i] = mode;
        if (s.ops[i] == 0x10 && s.count(i) > 0)
            mode = uint8_t(s.params[s.first[i]] & 3);
    }
}

void split_units(const NdsGxStream& s, const std::vector<uint8_t>& modes,
                 std::vector<Unit>* units) {
    units->clear();
    for (size_t i = 0; i < s.size(); ++i) {
        const uint8_t op = s.ops[i];
        // The swap stands alone, so the last model's unit looks the same
        // whether or not anything follows it.
        if (i == 0 || op == 0x50 || s.ops[i - 1] == 0x50 ||
            (is_matrix(op) && !is_matrix(s.ops[i - 1]))) {
            if (!units->empty()) units->back().end = uint32_t(i);
            Unit unit;
            unit.begin = uint32_t(i);
            unit.signature = 1469598103934665603ull;
            units->push_back(unit);
        }
        Unit& unit = units->back();
        unit.signature = mix(unit.signature, op);
        const uint32_t* p = &s.params[s.first[i]];
        const uint32_t n = s.count(i);
        if (is_blended(op)) {
            if (modes[i] != 3) unit.moving = true;
        } else if (op == 0x29) {
            // Polygon alpha fades; the rest says what is drawn.
            unit.signature = mix(unit.signature, p[0] & ~0x001F0000u);
        } else if (!is_soft(op)) {
            for (uint32_t k = 0; k < n; ++k) unit.signature = mix(unit.signature, p[k]);
        }
    }
    if (!units->empty()) units->back().end = uint32_t(s.size());
}

// The summed movement of the blended entries of two units with the same
// signature, or -1 when they are not the same thing (or the signature hash
// collided).
int64_t unit_cost(const NdsGxStream& a, const Unit& ua, const std::vector<uint8_t>& modes_a,
                  const NdsGxStream& b, const Unit& ub) {
    if (ua.end - ua.begin != ub.end - ub.begin) return -1;
    int64_t cost = 0;
    for (uint32_t k = 0; k < ub.end - ub.begin; ++k) {
        const uint32_t i = ua.begin + k, j = ub.begin + k;
        const uint8_t op = b.ops[j];
        if (a.ops[i] != op || a.count(i) != b.count(j)) return -1;
        if (!is_blended(op) || modes_a[i] == 3) continue;
        const auto* pa = reinterpret_cast<const int32_t*>(&a.params[a.first[i]]);
        const auto* pb = reinterpret_cast<const int32_t*>(&b.params[b.first[j]]);
        if (!blend_command(op, pa, pb, b.count(j), 0.5f, nullptr)) return -1;
        for (uint32_t e = 0; e < b.count(j); ++e)
            cost += std::llabs(int64_t(pb[e]) - int64_t(pa[e]));
    }
    return cost;
}

// A 4x4 matrix halfway, or b's own when the two are not one thing moving.
void blend_matrix(const int32_t* a, const int32_t* b, float t, int32_t* out) {
    int32_t mid[16];
    // A 4x4 load (0x16) has the layout of a stored matrix.
    if (blend_command(0x16, a, b, 16, t, mid)) std::copy_n(mid, 16, out);
    else std::copy_n(b, 16, out);
}

void blend_state(const NdsGxGeometryState& a, const NdsGxGeometryState& b, float t,
                 NdsGxGeometryState* out) {
    *out = b;
    blend_matrix(a.proj, b.proj, t, out->proj);
    blend_matrix(a.pos, b.pos, t, out->pos);
    blend_matrix(a.vec, b.vec, t, out->vec);
    blend_matrix(a.proj_stack, b.proj_stack, t, out->proj_stack);
    for (int i = 0; i < 32; ++i) {
        blend_matrix(a.pos_stack[i], b.pos_stack[i], t, out->pos_stack[i]);
        blend_matrix(a.vec_stack[i], b.vec_stack[i], t, out->vec_stack[i]);
    }
}

class NullRenderer final : public melonDS::Renderer3D {
public:
    NullRenderer() : Renderer3D(false) {}
    void Reset(melonDS::GPU&) override {}
    void RenderFrame(melonDS::GPU&) override {}
    melonDS::u32* GetLine(int) override { return line_; }

private:
    melonDS::u32 line_[448] = {};
};

void put_bytes(std::vector<uint8_t>* out, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

template <typename T>
void put(std::vector<uint8_t>* out, const T& value) {
    put_bytes(out, &value, sizeof(value));
}

template <typename T>
bool get(FILE* file, T* value) {
    return std::fread(value, sizeof(*value), 1, file) == 1;
}

}  // namespace

void nds_gx_capture_state(const GPU3D& g, NdsGxGeometryState* s) {
    *s = NdsGxGeometryState{};
    s->matrix_mode = g.MatrixMode;
    std::memcpy(s->proj, g.ProjMatrix, sizeof(s->proj));
    std::memcpy(s->pos, g.PosMatrix, sizeof(s->pos));
    std::memcpy(s->vec, g.VecMatrix, sizeof(s->vec));
    std::memcpy(s->tex, g.TexMatrix, sizeof(s->tex));
    std::memcpy(s->proj_stack, g.ProjMatrixStack, sizeof(s->proj_stack));
    std::memcpy(s->pos_stack, g.PosMatrixStack, sizeof(s->pos_stack));
    std::memcpy(s->vec_stack, g.VecMatrixStack, sizeof(s->vec_stack));
    std::memcpy(s->tex_stack, g.TexMatrixStack, sizeof(s->tex_stack));
    s->proj_sp = g.ProjMatrixStackPointer;
    s->pos_sp = g.PosMatrixStackPointer;
    s->tex_sp = g.TexMatrixStackPointer;
    std::memcpy(s->viewport, g.Viewport, sizeof(s->viewport));
    s->polygon_mode = g.PolygonMode;
    std::memcpy(s->cur_vertex, g.CurVertex, sizeof(s->cur_vertex));
    std::memcpy(s->vertex_color, g.VertexColor, sizeof(s->vertex_color));
    std::memcpy(s->tex_coords, g.TexCoords, sizeof(s->tex_coords));
    std::memcpy(s->raw_tex_coords, g.RawTexCoords, sizeof(s->raw_tex_coords));
    std::memcpy(s->normal, g.Normal, sizeof(s->normal));
    std::memcpy(s->light_direction, g.LightDirection, sizeof(s->light_direction));
    std::memcpy(s->spec_recip, g.SpecRecip, sizeof(s->spec_recip));
    std::memcpy(s->light_color, g.LightColor, sizeof(s->light_color));
    std::memcpy(s->mat_diffuse, g.MatDiffuse, sizeof(s->mat_diffuse));
    std::memcpy(s->mat_ambient, g.MatAmbient, sizeof(s->mat_ambient));
    std::memcpy(s->mat_specular, g.MatSpecular, sizeof(s->mat_specular));
    std::memcpy(s->mat_emission, g.MatEmission, sizeof(s->mat_emission));
    s->use_shininess = g.UseShininessTable ? 1 : 0;
    std::memcpy(s->shininess, g.ShininessTable, sizeof(s->shininess));
    s->polygon_attr = g.PolygonAttr;
    s->cur_polygon_attr = g.CurPolygonAttr;
    s->tex_param = g.TexParam;
    s->tex_palette = g.TexPalette;
    s->zero_dot_w_limit = g.ZeroDotWLimit;
    s->flush_attributes = g.FlushAttributes;
    s->vertex_num = g.VertexNum;
    s->vertex_num_in_poly = g.VertexNumInPoly;
    s->num_consecutive_polygons = g.NumConsecutivePolygons;
    s->render_width = g.GetRenderWidth();
    s->guest_wide = g.GetGuestWideProjection() ? 1 : 0;
    std::memcpy(s->temp_vertices, g.TempVertexBuffer, sizeof(s->temp_vertices));
}

void nds_gx_apply_state(const NdsGxGeometryState& s, GPU3D* out) {
    GPU3D& g = *out;
    g.MatrixMode = s.matrix_mode;
    std::memcpy(g.ProjMatrix, s.proj, sizeof(s.proj));
    std::memcpy(g.PosMatrix, s.pos, sizeof(s.pos));
    std::memcpy(g.VecMatrix, s.vec, sizeof(s.vec));
    std::memcpy(g.TexMatrix, s.tex, sizeof(s.tex));
    std::memcpy(g.ProjMatrixStack, s.proj_stack, sizeof(s.proj_stack));
    std::memcpy(g.PosMatrixStack, s.pos_stack, sizeof(s.pos_stack));
    std::memcpy(g.VecMatrixStack, s.vec_stack, sizeof(s.vec_stack));
    std::memcpy(g.TexMatrixStack, s.tex_stack, sizeof(s.tex_stack));
    g.ProjMatrixStackPointer = s.proj_sp;
    g.PosMatrixStackPointer = s.pos_sp;
    g.TexMatrixStackPointer = s.tex_sp;
    g.ClipMatrixDirty = true;
    std::memcpy(g.Viewport, s.viewport, sizeof(s.viewport));
    g.PolygonMode = s.polygon_mode;
    std::memcpy(g.CurVertex, s.cur_vertex, sizeof(s.cur_vertex));
    std::memcpy(g.VertexColor, s.vertex_color, sizeof(s.vertex_color));
    std::memcpy(g.TexCoords, s.tex_coords, sizeof(s.tex_coords));
    std::memcpy(g.RawTexCoords, s.raw_tex_coords, sizeof(s.raw_tex_coords));
    std::memcpy(g.Normal, s.normal, sizeof(s.normal));
    std::memcpy(g.LightDirection, s.light_direction, sizeof(s.light_direction));
    std::memcpy(g.SpecRecip, s.spec_recip, sizeof(s.spec_recip));
    std::memcpy(g.LightColor, s.light_color, sizeof(s.light_color));
    std::memcpy(g.MatDiffuse, s.mat_diffuse, sizeof(s.mat_diffuse));
    std::memcpy(g.MatAmbient, s.mat_ambient, sizeof(s.mat_ambient));
    std::memcpy(g.MatSpecular, s.mat_specular, sizeof(s.mat_specular));
    std::memcpy(g.MatEmission, s.mat_emission, sizeof(s.mat_emission));
    g.UseShininessTable = s.use_shininess != 0;
    std::memcpy(g.ShininessTable, s.shininess, sizeof(s.shininess));
    g.PolygonAttr = s.polygon_attr;
    g.CurPolygonAttr = s.cur_polygon_attr;
    g.TexParam = s.tex_param;
    g.TexPalette = s.tex_palette;
    g.ZeroDotWLimit = s.zero_dot_w_limit;
    g.FlushAttributes = s.flush_attributes;
    g.VertexNum = s.vertex_num;
    g.VertexNumInPoly = s.vertex_num_in_poly;
    g.NumConsecutivePolygons = s.num_consecutive_polygons;
    // A strip never continues into a new image.
    g.LastStripPolygon = nullptr;
    std::memcpy(g.TempVertexBuffer, s.temp_vertices, sizeof(s.temp_vertices));
    if (g.GetRenderWidth() != s.render_width) g.SetRenderWidth(s.render_width);
    g.SetGuestWideProjection(s.guest_wide != 0);
}

void NdsGxRecorder::attach(GPU3D& gpu3d) {
    gpu3d.ExecObserver = &NdsGxRecorder::observe;
    gpu3d.ExecObserverContext = this;
    attached_ = true;
    valid_ = false;
}

void NdsGxRecorder::detach(GPU3D& gpu3d) {
    if (gpu3d.ExecObserverContext == this) {
        gpu3d.ExecObserver = nullptr;
        gpu3d.ExecObserverContext = nullptr;
    }
    attached_ = false;
    valid_ = false;
}

void NdsGxRecorder::observe(void* self, uint32_t command, const uint32_t* params,
                            uint32_t count) {
    NdsGxStream& s = static_cast<NdsGxRecorder*>(self)->recording_;
    // Far past any real image; a game that never swaps must not grow this.
    if (s.ops.size() >= (1u << 20)) return;
    s.push(uint8_t(command), params, count);
    if (command == 0x50) s.complete = true;
}

bool NdsGxRecorder::after_vblank(const GPU3D& gpu3d, bool flushed, NdsGxStream* out) {
    if (!attached_ || !flushed) return false;
    const bool finished = valid_ && recording_.complete;
    if (finished) std::swap(recording_, *out);
    recording_.clear();
    nds_gx_capture_state(gpu3d, &recording_.start);
    valid_ = true;
    return finished;
}

bool nds_gx_lerp_streams(const NdsGxStream& a, const NdsGxStream& b, float t,
                         NdsGxStream* out, NdsGxLerpStats* stats,
                         std::vector<NdsGxUnitMatch>* report) {
    NdsGxLerpStats local;
    NdsGxLerpStats& st = stats ? *stats : local;
    st = NdsGxLerpStats{};
    if (!a.complete || !b.complete || b.size() == 0) return false;

    static thread_local std::vector<uint8_t> modes_a, modes_b;
    static thread_local std::vector<Unit> units_a, units_b;
    matrix_modes(a, &modes_a);
    matrix_modes(b, &modes_b);
    split_units(a, modes_a, &units_a);
    split_units(b, modes_b, &units_b);
    st.units = units_b.size();
    st.commands = b.size();

    std::unordered_map<uint64_t, std::vector<uint32_t>> by_signature;
    by_signature.reserve(units_a.size());
    for (uint32_t i = 0; i < units_a.size(); ++i)
        if (units_a[i].moving) by_signature[units_a[i].signature].push_back(i);

    struct Candidate {
        int64_t cost;
        uint32_t b, a;
    };
    static thread_local std::vector<Candidate> candidates;
    candidates.clear();
    if (report) {
        report->assign(units_b.size(), NdsGxUnitMatch{});
        for (uint32_t j = 0; j < units_b.size(); ++j) {
            (*report)[j].begin = units_b[j].begin;
            (*report)[j].end = units_b[j].end;
        }
    }
    for (uint32_t j = 0; j < units_b.size(); ++j) {
        if (!units_b[j].moving) continue;
        ++st.moving_units;
        const auto found = by_signature.find(units_b[j].signature);
        if (report) (*report)[j].status = NdsGxUnitStatus::NoSignature;
        if (found == by_signature.end()) continue;
        bool any = false;
        for (const uint32_t i : found->second) {
            const int64_t cost = unit_cost(a, units_a[i], modes_a, b, units_b[j]);
            if (cost >= 0) candidates.push_back({cost, j, i});
            any |= cost >= 0;
        }
        if (report)
            (*report)[j].status = any ? NdsGxUnitStatus::Taken : NdsGxUnitStatus::Implausible;
    }
    // Look-alikes: each copy takes the one that moved least.
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& l, const Candidate& r) {
        return l.cost != r.cost ? l.cost < r.cost : (l.b != r.b ? l.b < r.b : l.a < r.a);
    });
    static thread_local std::vector<int32_t> b_to_a;
    static thread_local std::vector<uint8_t> a_used;
    b_to_a.assign(units_b.size(), -1);
    a_used.assign(units_a.size(), 0);
    for (const Candidate& c : candidates) {
        if (b_to_a[c.b] >= 0 || a_used[c.a]) continue;
        b_to_a[c.b] = int32_t(c.a);
        a_used[c.a] = 1;
        ++st.blended_units;
        if (report) {
            (*report)[c.b].status = NdsGxUnitStatus::Blended;
            (*report)[c.b].a_begin = int32_t(units_a[c.a].begin);
            (*report)[c.b].cost = c.cost;
        }
    }
    // Below half, the two images are different scenes (a cut, a menu).
    if (st.moving_units > 0 && st.blended_units * 2 < st.moving_units) return false;

    out->ops = b.ops;
    out->first = b.first;
    out->params = b.params;
    out->complete = true;
    blend_state(a.start, b.start, t, &out->start);
    for (uint32_t j = 0; j < units_b.size(); ++j) {
        if (b_to_a[j] < 0) continue;
        const Unit& ua = units_a[size_t(b_to_a[j])];
        const Unit& ub = units_b[j];
        for (uint32_t k = 0; k < ub.end - ub.begin; ++k) {
            const uint32_t i = ua.begin + k, jj = ub.begin + k;
            if (!is_blended(b.ops[jj]) || modes_b[jj] == 3) continue;
            const auto* pa = reinterpret_cast<const int32_t*>(&a.params[a.first[i]]);
            auto* po = reinterpret_cast<int32_t*>(&out->params[b.first[jj]]);
            // Checked by unit_cost; b's own values stay if it still refuses.
            blend_command(b.ops[jj], pa, po, b.count(jj), t, po);
        }
    }
    return true;
}

NdsGxReplayer::NdsGxReplayer(melonDS::NDS& nds)
    : gpu_(std::make_unique<GPU3D>(nds, std::make_unique<NullRenderer>())) {
    gpu_->Reset();
}

NdsGxReplayer::~NdsGxReplayer() = default;

bool NdsGxReplayer::run(const NdsGxStream& stream) {
    GPU3D& g = *gpu_;
    g.FlushRequest = 0;
    g.NumVertices = 0;
    g.NumPolygons = 0;
    g.NumOpaquePolygons = 0;
    g.ExecParamCount = 0;
    g.GeometryEnabled = true;
    g.RenderingEnabled = true;
    nds_gx_apply_state(stream.start, &g);
    for (size_t i = 0; i < stream.size(); ++i) {
        const uint32_t n = stream.count(i);
        g.ExecuteReplayCommand(stream.ops[i], n ? &stream.params[stream.first[i]] : nullptr, n);
        if (g.FlushRequest) break;
    }
    if (!g.FlushRequest) return false;
    g.VBlank();
    return true;
}

melonDS::Polygon* const* NdsGxReplayer::polygons() const {
    return gpu_->RenderPolygonRAM.data();
}

uint32_t NdsGxReplayer::count() const { return gpu_->RenderNumPolygons; }

GPU3D& NdsGxReplayer::gpu() { return *gpu_; }

void nds_gx_trace_polygon(const Polygon& p, NdsGxTracePolygon* out) {
    *out = NdsGxTracePolygon{};
    out->attr = p.Attr;
    out->tex_param = p.TexParam;
    out->tex_palette = p.TexPalette;
    out->num_vertices = std::min<uint32_t>(p.NumVertices, 10);
    out->flags = (p.Translucent ? 1u : 0u) | (p.IsShadowMask ? 2u : 0u) |
                 (p.IsShadow ? 4u : 0u) | (p.FacingView ? 8u : 0u) |
                 (p.WBuffer ? 16u : 0u) | (p.Degenerate ? 32u : 0u) |
                 (p.Type == 1 ? 64u : 0u);
    for (uint32_t v = 0; v < out->num_vertices; ++v) {
        out->final_z[v] = p.FinalZ[v];
        out->final_w[v] = p.FinalW[v];
        out->vertices[v] = *p.Vertices[v];
    }
}

void nds_gx_trace_image(std::vector<uint8_t>* out, uint32_t vblank,
                        const NdsGxStream& stream, const GPU3D& gpu3d) {
    put_bytes(out, "GXCI", 4);
    put(out, vblank);
    put(out, uint32_t(sizeof(NdsGxGeometryState)));
    put(out, stream.start);
    put(out, uint32_t(stream.size()));
    put(out, uint32_t(stream.params.size()));
    put_bytes(out, stream.ops.data(), stream.ops.size());
    for (size_t i = 0; i < stream.size(); ++i) put(out, uint8_t(stream.count(i)));
    put_bytes(out, stream.params.data(), stream.params.size() * sizeof(uint32_t));
    put(out, uint32_t(gpu3d.RenderNumPolygons));
    for (uint32_t i = 0; i < gpu3d.RenderNumPolygons; ++i) {
        NdsGxTracePolygon p;
        nds_gx_trace_polygon(*gpu3d.RenderPolygonRAM[i], &p);
        put(out, p.attr);
        put(out, p.tex_param);
        put(out, p.tex_palette);
        put(out, p.num_vertices);
        put(out, p.flags);
        put(out, p.final_z);
        put(out, p.final_w);
        put_bytes(out, p.vertices, sizeof(Vertex) * p.num_vertices);
    }
}

void nds_gx_trace_image(FILE* file, uint32_t vblank, const NdsGxStream& stream,
                        const GPU3D& gpu3d) {
    std::vector<uint8_t> bytes;
    nds_gx_trace_image(&bytes, vblank, stream, gpu3d);
    std::fwrite(bytes.data(), 1, bytes.size(), file);
}

void nds_gx_trace_mark(FILE* file, uint32_t vblank, uint32_t mark) {
    std::vector<uint8_t> bytes;
    put_bytes(&bytes, "GXMK", 4);
    put(&bytes, vblank);
    put(&bytes, mark);
    std::fwrite(bytes.data(), 1, bytes.size(), file);
}

bool nds_gx_trace_read(FILE* file, NdsGxTraceImage* out) {
    char magic[4];
    out->marks.clear();
    for (;;) {
        if (std::fread(magic, 4, 1, file) != 1) return false;
        if (std::memcmp(magic, "GXMK", 4) != 0) break;
        uint32_t vblank = 0, mark = 0;
        if (!get(file, &vblank) || !get(file, &mark)) return false;
        (void)mark;
        out->marks.push_back(vblank);
    }
    if (std::memcmp(magic, "GXCI", 4) != 0) return false;
    uint32_t state_size = 0, commands = 0, params = 0, polygons = 0;
    if (!get(file, &out->vblank) || !get(file, &state_size) ||
        state_size != sizeof(NdsGxGeometryState) || !get(file, &out->stream.start) ||
        !get(file, &commands) || !get(file, &params))
        return false;
    NdsGxStream& s = out->stream;
    s.ops.resize(commands);
    s.first.resize(commands);
    s.params.resize(params);
    std::vector<uint8_t> counts(commands);
    if (std::fread(s.ops.data(), 1, commands, file) != commands ||
        std::fread(counts.data(), 1, commands, file) != commands ||
        std::fread(s.params.data(), sizeof(uint32_t), params, file) != params)
        return false;
    uint32_t at = 0;
    for (uint32_t i = 0; i < commands; ++i) {
        s.first[i] = at;
        at += counts[i];
    }
    if (at != params) return false;
    s.complete = commands > 0 && s.ops.back() == 0x50;
    if (!get(file, &polygons) || polygons > 2048) return false;
    out->polygons.resize(polygons);
    for (NdsGxTracePolygon& p : out->polygons) {
        if (!get(file, &p.attr) || !get(file, &p.tex_param) || !get(file, &p.tex_palette) ||
            !get(file, &p.num_vertices) || p.num_vertices > 10 || !get(file, &p.flags) ||
            !get(file, &p.final_z) || !get(file, &p.final_w) ||
            std::fread(p.vertices, sizeof(Vertex), p.num_vertices, file) != p.num_vertices)
            return false;
    }
    return true;
}
