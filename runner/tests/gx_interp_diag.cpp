// Offline diagnosis of smooth_3d halfway images, from an NDS_GX_CMD_TRACE.
//
//   gx_interp_diag <trace.gxc> [--worst N] [--around-marks IMAGES] [--pair VBLANK]
//
// For every pair of consecutive traced images the runner would blend, this
// redraws a, b and the halfway stream (nds_gx_lerp_streams) through the
// vendored geometry engine, keeping the clip position of every vertex
// command, and reports:
//
//   replay  whether replaying b's stream gives the render list the real
//           engine built (if not, the recording misses state, and the
//           halfway image starts from a wrong b);
//   units   how each draw unit of b was treated (blended, or why not);
//   seams   vertices of different units at one clip position in both a and
//           b (a mesh seam, a bone joint) that come apart in the halfway
//           image. This is a tear by construction: if both sides moved
//           halfway, they would still meet;
//   bend    how far each halfway vertex lands from the screen midpoint of
//           its a and b positions (product of blended matrices, shrinking
//           rotations).
//
// Distances are in DS pixels (256x192).
#include "gpu3d_gxstream.h"

#include "NDS.h"
#include "Platform.h"
#include "Savestate.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace melonDS {
void NDS::SetIRQ(u32, u32) {}
void NDS::ClearIRQ(u32, u32) {}
void NDS::CheckDMAs(u32, u32) {}
void NDS::GXFIFOStall() {}
void NDS::GXFIFOUnstall() {}
bool GPU::MakeVRAMFlat_TextureCoherent(NonStupidBitField<1024>&) noexcept { return false; }
bool GPU::MakeVRAMFlat_TexPalCoherent(NonStupidBitField<256>&) noexcept { return false; }
namespace Platform {
void Log(LogLevel, const char*, ...) {}
struct Thread {};
struct Semaphore {};
Thread* Thread_Create(std::function<void()>) { throw std::runtime_error("unexpected renderer thread"); }
void Thread_Free(Thread*) {}
void Thread_Wait(Thread*) {}
Semaphore* Semaphore_Create() { return new Semaphore; }
void Semaphore_Free(Semaphore* s) { delete s; }
void Semaphore_Reset(Semaphore*) {}
void Semaphore_Wait(Semaphore*) {}
void Semaphore_Post(Semaphore*, int) {}
}  // namespace Platform
}  // namespace melonDS

namespace {
using melonDS::GPU3D;
using melonDS::Polygon;

constexpr uint64_t kMaxImageGap = 4;  // as gpu3d_interp.cpp

bool is_vertex(uint8_t op) { return op >= 0x23 && op <= 0x28; }

struct Clip {
    int32_t p[4] = {};
    bool operator==(const Clip& o) const { return std::memcmp(p, o.p, sizeof(p)) == 0; }
};

struct ClipHash {
    size_t operator()(const Clip& c) const {
        uint64_t h = 1469598103934665603ull;
        for (int32_t v : c.p) h = (h ^ uint32_t(v)) * 1099511628211ull;
        return size_t(h);
    }
};

struct Screen {
    double x = 0, y = 0;
    bool ok = false;  // in front of the camera
};

Screen to_screen(const Clip& c, const uint32_t* viewport) {
    Screen s;
    const double w = c.p[3];
    if (w <= 0) return s;
    // As GPU3D: x0, y0 (bottom), x1, y1 (top), width, height; y grows down.
    s.x = (c.p[0] + w) * viewport[4] / (2 * w) + viewport[0];
    s.y = (-c.p[1] + w) * viewport[5] / (2 * w) + viewport[3];
    s.ok = true;
    return s;
}

double dist(const Screen& a, const Screen& b) {
    return std::hypot(a.x - b.x, a.y - b.y);
}

// One replay: the clip position of each vertex command (by command index)
// and the render list.
struct Replay {
    std::vector<int32_t> vertex_of;  // command index -> index in clips, or -1
    std::vector<Clip> clips;
    std::vector<uint32_t> clip_viewport;  // per clip: index into viewports
    std::vector<std::array<uint32_t, 6>> viewports;
    std::vector<NdsGxTracePolygon> polygons;
    bool flushed = false;
};

class Engine {
public:
    Engine() { g_.Reset(); }

    void run(const NdsGxStream& s, Replay* out) {
        GPU3D& g = g_;
        g.FlushRequest = 0;
        g.NumVertices = 0;
        g.NumPolygons = 0;
        g.NumOpaquePolygons = 0;
        g.ExecParamCount = 0;
        g.GeometryEnabled = true;
        g.RenderingEnabled = true;
        nds_gx_apply_state(s.start, &g);
        out->vertex_of.assign(s.size(), -1);
        out->clips.clear();
        out->clip_viewport.clear();
        out->viewports.clear();
        out->polygons.clear();
        std::array<uint32_t, 6> vp{};
        for (size_t i = 0; i < s.size(); ++i) {
            const uint32_t n = s.count(i);
            g.ExecuteReplayCommand(s.ops[i], n ? &s.params[s.first[i]] : nullptr, n);
            if (is_vertex(s.ops[i])) {
                // As GPU3D::SubmitVertex: the clip matrix is current.
                const int64_t v[4] = {g.CurVertex[0], g.CurVertex[1], g.CurVertex[2], 0x1000};
                Clip c;
                for (int k = 0; k < 4; ++k)
                    c.p[k] = int32_t((v[0] * g.ClipMatrix[k] + v[1] * g.ClipMatrix[4 + k] +
                                      v[2] * g.ClipMatrix[8 + k] + v[3] * g.ClipMatrix[12 + k]) >>
                                     12);
                std::array<uint32_t, 6> now{};
                std::memcpy(now.data(), g.Viewport, sizeof(g.Viewport));
                if (out->viewports.empty() || now != vp) {
                    out->viewports.push_back(now);
                    vp = now;
                }
                out->vertex_of[i] = int32_t(out->clips.size());
                out->clips.push_back(c);
                out->clip_viewport.push_back(uint32_t(out->viewports.size() - 1));
            }
            if (g.FlushRequest) break;
        }
        out->flushed = g.FlushRequest != 0;
        if (!out->flushed) return;
        g.VBlank();
        out->polygons.resize(g.RenderNumPolygons);
        for (uint32_t i = 0; i < g.RenderNumPolygons; ++i)
            nds_gx_trace_polygon(*g.RenderPolygonRAM[i], &out->polygons[i]);
    }

    Screen screen(const Replay& r, int32_t clip) const {
        return to_screen(r.clips[size_t(clip)], r.viewports[r.clip_viewport[size_t(clip)]].data());
    }

private:
    melonDS::NDS nds_;
    GPU3D& g_ = nds_.GPU.GPU3D;
};

// Whether two render lists draw the same thing, and if not, the first
// polygon that differs.
int first_difference(const std::vector<NdsGxTracePolygon>& real,
                     const std::vector<NdsGxTracePolygon>& replay) {
    const size_t n = std::min(real.size(), replay.size());
    for (size_t i = 0; i < n; ++i) {
        const NdsGxTracePolygon& a = real[i];
        const NdsGxTracePolygon& b = replay[i];
        if (a.attr != b.attr || a.num_vertices != b.num_vertices || a.flags != b.flags ||
            a.tex_param != b.tex_param)
            return int(i);
        for (uint32_t v = 0; v < a.num_vertices; ++v) {
            if (a.vertices[v].FinalPosition[0] != b.vertices[v].FinalPosition[0] ||
                a.vertices[v].FinalPosition[1] != b.vertices[v].FinalPosition[1] ||
                a.final_z[v] != b.final_z[v])
                return int(i);
        }
    }
    return real.size() == replay.size() ? -1 : int(n);
}

const char* status_name(NdsGxUnitStatus s) {
    switch (s) {
        case NdsGxUnitStatus::Still: return "still";
        case NdsGxUnitStatus::Blended: return "blended";
        case NdsGxUnitStatus::NoSignature: return "no-signature";
        case NdsGxUnitStatus::Implausible: return "implausible";
        case NdsGxUnitStatus::Taken: return "taken";
    }
    return "?";
}

struct SeamReport {
    double gap = 0;
    uint32_t vblank = 0;
    uint32_t cmd_1 = 0, cmd_2 = 0;  // the two vertex commands in b
    int unit_1 = 0, unit_2 = 0;
    NdsGxUnitStatus status_1{}, status_2{};
    Screen at;
};

struct PairResult {
    uint32_t vblank_a = 0, vblank_b = 0;
    bool blended = false;
    NdsGxLerpStats stats;
    size_t status_vertices[5] = {};  // b's vertices by their unit's status
    size_t seam_groups = 0, seams_1px = 0, seams_4px = 0;
    double seam_max = 0;
    size_t bend_count = 0, bend_2px = 0, bend_4px = 0, far_pairs = 0;
    double bend_max = 0;
    int replay_diff = -1;  // first polygon replay(b) gets wrong, -1 none
    size_t real_polygons = 0, replay_polygons = 0, half_polygons = 0;
    std::vector<SeamReport> seams;  // worst few
    std::vector<NdsGxUnitMatch> units;
    double bend_worst_x = 0, bend_worst_y = 0;
    Screen worst_a, worst_b, worst_h;
    int worst_unit = -1;
    uint32_t bend_worst_cmd = 0;
};

void analyse(Engine& engine, const NdsGxTraceImage& a, const NdsGxTraceImage& b,
             PairResult* r) {
    r->vblank_a = a.vblank;
    r->vblank_b = b.vblank;
    NdsGxStream half;
    r->blended = nds_gx_lerp_streams(a.stream, b.stream, 0.5f, &half, &r->stats, &r->units);

    static Replay ra, rb, rh;
    engine.run(b.stream, &rb);
    r->real_polygons = b.polygons.size();
    r->replay_polygons = rb.polygons.size();
    r->replay_diff = first_difference(b.polygons, rb.polygons);
    if (!r->blended) return;
    engine.run(a.stream, &ra);
    engine.run(half, &rh);
    r->half_polygons = rh.polygons.size();

    const NdsGxStream& sb = b.stream;
    // Each vertex command of b: its unit and, when blended, the matching
    // command in a.
    std::vector<int32_t> a_command(sb.size(), -1);
    std::vector<int32_t> unit(sb.size(), -1);
    for (size_t u = 0; u < r->units.size(); ++u) {
        const NdsGxUnitMatch& m = r->units[u];
        for (uint32_t i = m.begin; i < m.end; ++i) {
            unit[i] = int32_t(u);
            if (m.status == NdsGxUnitStatus::Blended) a_command[i] = m.a_begin + int32_t(i - m.begin);
        }
    }

    // Seams: group b's vertices by exact clip position.
    std::unordered_map<Clip, std::vector<uint32_t>, ClipHash> groups;
    for (uint32_t i = 0; i < sb.size(); ++i) {
        const int32_t vb = rb.vertex_of[i];
        if (vb < 0) continue;
        const NdsGxUnitStatus st = r->units[size_t(unit[i])].status;
        ++r->status_vertices[size_t(st)];
        groups[rb.clips[size_t(vb)]].push_back(i);
    }
    for (const auto& [clip, members] : groups) {
        if (members.size() < 2) continue;
        // Only joints between units: within one unit the same matrix moves
        // every copy alike.
        bool cross = false;
        for (uint32_t m : members) cross |= unit[m] != unit[members[0]];
        if (!cross) continue;
        // Their a positions must meet too, where known, or the joint opened
        // in the game's own images (a cut, a part detaching).
        const Clip* in_a = nullptr;
        bool meets_in_a = true;
        for (uint32_t m : members) {
            if (a_command[m] < 0) continue;
            const int32_t va = ra.vertex_of[size_t(a_command[m])];
            if (va < 0) { meets_in_a = false; break; }
            if (!in_a) in_a = &ra.clips[size_t(va)];
            else if (!(*in_a == ra.clips[size_t(va)])) { meets_in_a = false; break; }
        }
        if (!meets_in_a) continue;
        ++r->seam_groups;
        double worst = 0;
        uint32_t w1 = members[0], w2 = members[1];
        for (size_t x = 0; x < members.size(); ++x) {
            for (size_t y = x + 1; y < members.size(); ++y) {
                if (unit[members[x]] == unit[members[y]]) continue;
                const int32_t hx = rh.vertex_of[members[x]], hy = rh.vertex_of[members[y]];
                if (hx < 0 || hy < 0) continue;
                const Screen sx = engine.screen(rh, hx), sy = engine.screen(rh, hy);
                if (!sx.ok || !sy.ok) continue;
                const double d = dist(sx, sy);
                if (d > worst) { worst = d; w1 = members[x]; w2 = members[y]; }
            }
        }
        if (worst > 1) ++r->seams_1px;
        if (worst > 4) ++r->seams_4px;
        r->seam_max = std::max(r->seam_max, worst);
        if (worst > 1) {
            SeamReport s;
            s.gap = worst;
            s.vblank = b.vblank;
            s.cmd_1 = w1;
            s.cmd_2 = w2;
            s.unit_1 = unit[w1];
            s.unit_2 = unit[w2];
            s.status_1 = r->units[size_t(unit[w1])].status;
            s.status_2 = r->units[size_t(unit[w2])].status;
            s.at = engine.screen(rh, rh.vertex_of[w1]);
            r->seams.push_back(s);
        }
    }
    std::sort(r->seams.begin(), r->seams.end(),
              [](const SeamReport& l, const SeamReport& rr) { return l.gap > rr.gap; });
    if (r->seams.size() > 8) r->seams.resize(8);

    // Bend: halfway against the screen midpoint, blended units only.
    for (uint32_t i = 0; i < sb.size(); ++i) {
        if (a_command[i] < 0) continue;
        const int32_t vb = rb.vertex_of[i], vh = rh.vertex_of[i];
        const int32_t va = ra.vertex_of[size_t(a_command[i])];
        if (va < 0 || vb < 0 || vh < 0) continue;
        const Screen pa = engine.screen(ra, va), pb = engine.screen(rb, vb),
                     ph = engine.screen(rh, vh);
        if (!pa.ok || !pb.ok || !ph.ok) continue;
        // Off-screen vertices do not show.
        if (ph.x < -32 || ph.x > 288 || ph.y < -32 || ph.y > 224) continue;
        // Over 32 px in one image is another copy of a look-alike (a unit
        // that only scales the matrix it inherits pairs with whichever copy),
        // not motion: the halfway image follows the inherited matrix.
        if (dist(pa, pb) > 32) {
            ++r->far_pairs;
            continue;
        }
        Screen mid;
        mid.x = (pa.x + pb.x) / 2;
        mid.y = (pa.y + pb.y) / 2;
        const double d = dist(mid, ph);
        ++r->bend_count;
        if (d > 2) ++r->bend_2px;
        if (d > 4) ++r->bend_4px;
        if (d > r->bend_max) {
            r->bend_max = d;
            r->bend_worst_x = ph.x;
            r->bend_worst_y = ph.y;
            r->bend_worst_cmd = i;
            r->worst_a = pa;
            r->worst_b = pb;
            r->worst_h = ph;
            r->worst_unit = unit[i];
        }
    }
}

std::string describe_unit(const NdsGxStream& s, const NdsGxUnitMatch& u) {
    std::string out;
    char buf[64];
    std::map<uint8_t, int> vertices;
    for (uint32_t i = u.begin; i < u.end && out.size() < 160; ++i) {
        if (is_vertex(s.ops[i])) {
            ++vertices[s.ops[i]];
            continue;
        }
        std::snprintf(buf, sizeof(buf), "%02X", s.ops[i]);
        if (s.ops[i] == 0x10 || s.ops[i] == 0x12 || s.ops[i] == 0x13 || s.ops[i] == 0x14)
            std::snprintf(buf, sizeof(buf), "%02X(%u)", s.ops[i], s.params[s.first[i]]);
        if (!out.empty()) out += ' ';
        out += buf;
    }
    for (const auto& [op, n] : vertices) {
        std::snprintf(buf, sizeof(buf), " v%02Xx%d", op, n);
        out += buf;
    }
    return out;
}

void print_matrices(const NdsGxStream& a, const NdsGxStream& b, const NdsGxUnitMatch& u) {
    for (uint32_t i = u.begin; i < u.end; ++i) {
        const uint8_t op = b.ops[i];
        if (op < 0x16 || op > 0x1C) continue;
        std::printf("      b %02X:", op);
        for (uint32_t e = 0; e < b.count(i); ++e)
            std::printf(" %8.4f", int32_t(b.params[b.first[i] + e]) / 4096.0);
        std::printf("\n");
        if (u.a_begin >= 0) {
            const uint32_t ia = uint32_t(u.a_begin) + (i - u.begin);
            std::printf("      a %02X:", a.ops[ia]);
            for (uint32_t e = 0; e < a.count(ia); ++e)
                std::printf(" %8.4f", int32_t(a.params[a.first[ia] + e]) / 4096.0);
            std::printf("\n");
        }
    }
}


// Truth: the game's own middle image. Blends images k-1 and k+1 and compares
// with the real image k, vertex by vertex, against holding k+1.
void truth(Engine& engine, const std::vector<NdsGxTraceImage>& images) {
    std::vector<double> err, hold;
    size_t triples = 0, refused = 0;
    static Replay rh, rm, rc;
    for (size_t k = 1; k + 1 < images.size(); ++k) {
        const NdsGxTraceImage& a = images[k - 1];
        const NdsGxTraceImage& m = images[k];
        const NdsGxTraceImage& c = images[k + 1];
        if (m.vblank - a.vblank != 2 || c.vblank - m.vblank != 2) continue;
        NdsGxStream half, ignored;
        std::vector<NdsGxUnitMatch> ac, mc;
        if (!nds_gx_lerp_streams(a.stream, c.stream, 0.5f, &half, nullptr, &ac) ||
            !nds_gx_lerp_streams(m.stream, c.stream, 0.5f, &ignored, nullptr, &mc)) {
            ++refused;
            continue;
        }
        ++triples;
        engine.run(half, &rh);
        engine.run(m.stream, &rm);
        engine.run(c.stream, &rc);
        for (size_t u = 0; u < mc.size(); ++u) {
            if (mc[u].status != NdsGxUnitStatus::Blended) continue;
            if (ac[u].status != NdsGxUnitStatus::Blended) continue;
            for (uint32_t i = mc[u].begin; i < mc[u].end; ++i) {
                const int32_t vh = rh.vertex_of[i], vc = rc.vertex_of[i];
                const uint32_t im = uint32_t(mc[u].a_begin) + (i - mc[u].begin);
                const int32_t vm = rm.vertex_of[im];
                if (vh < 0 || vc < 0 || vm < 0) continue;
                const Screen ph = engine.screen(rh, vh), pc = engine.screen(rc, vc),
                             pm = engine.screen(rm, vm);
                if (!ph.ok || !pc.ok || !pm.ok) continue;
                if (pm.x < -32 || pm.x > 288 || pm.y < -32 || pm.y > 224) continue;
                if (dist(pm, pc) > 32) continue;  // a look-alike's other copy
                err.push_back(dist(ph, pm));
                hold.push_back(dist(pc, pm));
            }
        }
    }
    auto report = [](const char* name, std::vector<double>& v) {
        if (v.empty()) return;
        std::sort(v.begin(), v.end());
        size_t over2 = 0, over4 = 0, over8 = 0;
        for (double d : v) {
            over2 += d > 2;
            over4 += d > 4;
            over8 += d > 8;
        }
        const double n = double(v.size());
        std::printf("  %-6s p50 %.2f p95 %.2f p99 %.2f p99.9 %.2f max %.1f | >2px %.3f%% >4px %.3f%% >8px %.3f%%\n",
                    name, v[v.size() / 2], v[size_t(n * 0.95)], v[size_t(n * 0.99)],
                    v[size_t(n * 0.999)], v.back(), 100 * over2 / n, 100 * over4 / n,
                    100 * over8 / n);
    };
    std::printf("\ntruth (blend k-1,k+1 vs real k): %zu triples, %zu refused, %zu vertices\n",
                triples, refused, err.size());
    report("blend", err);
    report("hold", hold);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <trace.gxc> [--worst N] [--around-marks IMAGES] [--pair VBLANK]\n",
                     argv[0]);
        return 2;
    }
    size_t worst_n = 15;
    int around = -1;
    long pair_vblank = -1;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--worst")) worst_n = size_t(std::atol(argv[i + 1]));
        else if (!std::strcmp(argv[i], "--around-marks")) around = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--pair")) pair_vblank = std::atol(argv[i + 1]);
    }
    FILE* file = std::fopen(argv[1], "rb");
    if (!file) {
        std::perror(argv[1]);
        return 1;
    }
    std::vector<NdsGxTraceImage> images;
    for (;;) {
        NdsGxTraceImage image;
        if (!nds_gx_trace_read(file, &image)) break;
        images.push_back(std::move(image));
    }
    std::fclose(file);
    std::vector<uint32_t> marks;
    for (const auto& image : images)
        for (uint32_t m : image.marks) marks.push_back(m);
    std::printf("%zu images, %zu marks\n", images.size(), marks.size());

    Engine engine;
    std::vector<PairResult> results;
    size_t replay_bad = 0, pairs = 0, blended = 0;
    size_t totals[5] = {};
    size_t seam_groups = 0, seams_1 = 0, seams_4 = 0, bend = 0, bend2 = 0, bend4 = 0, far = 0;
    for (size_t i = 1; i < images.size(); ++i) {
        const NdsGxTraceImage& a = images[i - 1];
        const NdsGxTraceImage& b = images[i];
        if (b.vblank - a.vblank > kMaxImageGap) continue;
        if (pair_vblank >= 0 && b.vblank != uint32_t(pair_vblank)) continue;
        if (around >= 0) {
            bool near = false;
            for (uint32_t m : marks)
                near |= b.vblank <= m + 4 && m <= b.vblank + uint32_t(around) * 2;
            if (!near) continue;
        }
        PairResult r;
        analyse(engine, a, b, &r);
        ++pairs;
        if (r.replay_diff >= 0) ++replay_bad;
        if (r.blended) ++blended;
        for (int k = 0; k < 5; ++k) totals[k] += r.status_vertices[k];
        seam_groups += r.seam_groups;
        seams_1 += r.seams_1px;
        seams_4 += r.seams_4px;
        bend += r.bend_count;
        bend2 += r.bend_2px;
        bend4 += r.bend_4px;
        far += r.far_pairs;
        if (r.replay_diff >= 0 && replay_bad <= 5)
            std::printf("replay of %u differs from the real list at polygon %d (%zu real, %zu replayed)\n",
                        b.vblank, r.replay_diff, r.real_polygons, r.replay_polygons);
        results.push_back(std::move(r));
    }
    const auto pct = [](size_t n, size_t d) { return d ? 100.0 * double(n) / double(d) : 0.0; };
    size_t all_vertices = 0;
    for (size_t t : totals) all_vertices += t;
    std::printf("pairs %zu, blended %zu (%.1f%%), replay(b) != real in %zu\n", pairs, blended,
                pct(blended, pairs), replay_bad);
    std::printf("vertices by unit status:");
    for (int k = 0; k < 5; ++k)
        std::printf(" %s %.2f%%", status_name(NdsGxUnitStatus(k)), pct(totals[k], all_vertices));
    std::printf("\n");
    std::printf("seams (joints between units) %zu: open >1px %.2f%%, >4px %.2f%%\n", seam_groups,
                pct(seams_1, seam_groups), pct(seams_4, seam_groups));
    std::printf("bend (halfway vs midpoint) %zu vertices: >2px %.2f%%, >4px %.2f%% (%zu far pairs skipped)\n",
                bend, pct(bend2, bend), pct(bend4, bend), far);

    truth(engine, images);

    // Per-mark windows: one line per pair.
    if (!marks.empty()) {
        std::printf("\npairs near marks (b vblank, gap, blended units/moving, seams open>1/>4 max, bend>4 max, replay):\n");
        for (const PairResult& r : results) {
            bool near = false;
            for (uint32_t m : marks) near |= r.vblank_b <= m + 4 && m <= r.vblank_b + 90;
            if (!near) continue;
            std::printf("  %6u +%u %s %zu/%zu  seams %zu/%zu %.1f  bend %zu %.1f%s\n", r.vblank_b,
                        r.vblank_b - r.vblank_a, r.blended ? "blend" : "SKIP ",
                        r.stats.blended_units, r.stats.moving_units, r.seams_1px, r.seams_4px,
                        r.seam_max, r.bend_4px, r.bend_max, r.replay_diff >= 0 ? "  REPLAY-DIFF" : "");
            for (uint32_t m : marks)
                if (m >= r.vblank_a && m < r.vblank_b) std::printf("  ---- mark at %u ----\n", m);
        }
    }

    // The worst pairs by seam gap, with details.
    std::vector<const PairResult*> order;
    for (const PairResult& r : results) order.push_back(&r);
    std::sort(order.begin(), order.end(), [](const PairResult* l, const PairResult* r) {
        return std::max(l->seam_max, l->bend_max) > std::max(r->seam_max, r->bend_max);
    });
    std::printf("\nworst pairs:\n");
    for (size_t k = 0; k < std::min(worst_n, order.size()); ++k) {
        const PairResult& r = *order[k];
        std::printf("  b=%u (+%u): seam max %.1f px (%zu open >4), bend max %.1f px at (%.0f,%.0f) cmd %u\n",
                    r.vblank_b, r.vblank_b - r.vblank_a, r.seam_max, r.seams_4px, r.bend_max,
                    r.bend_worst_x, r.bend_worst_y, r.bend_worst_cmd);
        if (r.worst_unit >= 0)
            std::printf("    worst vertex: a (%.1f,%.1f) b (%.1f,%.1f) half (%.1f,%.1f), unit %d %s\n",
                        r.worst_a.x, r.worst_a.y, r.worst_b.x, r.worst_b.y, r.worst_h.x,
                        r.worst_h.y, r.worst_unit,
                        status_name(r.units[size_t(r.worst_unit)].status));
        for (size_t s = 0; s < std::min<size_t>(r.seams.size(), 3); ++s) {
            const SeamReport& sr = r.seams[s];
            std::printf("    seam %.1f px at (%.0f,%.0f): unit %d %s / unit %d %s\n", sr.gap, sr.at.x,
                        sr.at.y, sr.unit_1, status_name(sr.status_1), sr.unit_2,
                        status_name(sr.status_2));
        }
    }

    // Full detail of one pair.
    if (pair_vblank >= 0) {
        for (size_t i = 1; i < images.size(); ++i) {
            if (images[i].vblank != uint32_t(pair_vblank)) continue;
            const NdsGxTraceImage& a = images[i - 1];
            const NdsGxTraceImage& b = images[i];
            PairResult r;
            analyse(engine, a, b, &r);
            std::printf("\npair %u -> %u, units:\n", a.vblank, b.vblank);
            for (size_t u = 0; u < r.units.size(); ++u) {
                const NdsGxUnitMatch& m = r.units[u];
                std::printf("  unit %zu [%u,%u) %s cost %lld: %s\n", u, m.begin, m.end,
                            status_name(m.status), static_cast<long long>(m.cost),
                            describe_unit(b.stream, m).c_str());
            }
            std::vector<int> shown;
            for (const SeamReport& s : r.seams) {
                for (int u : {s.unit_1, s.unit_2}) {
                    if (std::find(shown.begin(), shown.end(), u) != shown.end()) continue;
                    shown.push_back(u);
                    std::printf("    matrices of unit %d:\n", u);
                    print_matrices(a.stream, b.stream, r.units[size_t(u)]);
                }
            }
        }
    }
    return 0;
}
