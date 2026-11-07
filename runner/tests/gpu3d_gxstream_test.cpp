// smooth_3d's command-stream path against the vendored melonDS geometry
// engine: what the recorder captures must replay to the same render list, and
// a blend must move things halfway without mixing up look-alikes.
#include "gpu3d_gxstream.h"

#include "NDS.h"
#include "Platform.h"
#include "Savestate.h"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <stdexcept>
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

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

constexpr int32_t kOne = 0x1000;  // 1.0 in matrix 20.12 and vertex 4.12

// Commands, as the engine executes them.
struct Builder {
    std::vector<std::pair<uint8_t, std::vector<uint32_t>>> commands;
    void cmd(uint8_t op, std::vector<uint32_t> params = {}) {
        commands.emplace_back(op, std::move(params));
    }
    void mode(uint32_t m) { cmd(0x10, {m}); }
    void identity() { cmd(0x15); }
    void translate(int32_t x, int32_t y, int32_t z) {
        cmd(0x1C, {uint32_t(x), uint32_t(y), uint32_t(z)});
    }
    void vertex(int16_t x, int16_t y, int16_t z) {
        cmd(0x23, {uint32_t(uint16_t(x)) | (uint32_t(uint16_t(y)) << 16), uint32_t(uint16_t(z))});
    }
    // One opaque triangle, about a tenth of the screen wide, at the current matrix.
    void triangle(int16_t size = 0x200) {
        cmd(0x29, {0x001F00C0u});  // alpha 31, front and back
        cmd(0x40, {0});
        vertex(0, 0, 0);
        vertex(size, 0, 0);
        vertex(0, size, 0);
        cmd(0x41);
    }
    // Camera: identity projection, a full-screen viewport. Each model after
    // it starts from its own identity, as a game's models start from the
    // camera.
    void begin() {
        mode(0);
        identity();
        mode(2);
        cmd(0x60, {0xBFFF0000u});
    }
    void model(int32_t x, int32_t y = 0) {
        identity();
        translate(x, y, 0);
    }
    void swap() { cmd(0x50, {0}); }
};

void run(GPU3D& g, const Builder& b) {
    for (const auto& [op, params] : b.commands)
        g.ExecuteReplayCommand(op, params.data(), uint32_t(params.size()));
}

struct Engine {
    melonDS::NDS nds;
    GPU3D& g = nds.GPU.GPU3D;
    NdsGxRecorder recorder;

    Engine() {
        g.Reset();
        g.SetEnabled(true, true);
        recorder.attach(g);
        // A first, empty image so the recorder knows a start state.
        Builder empty;
        empty.swap();
        frame(empty, nullptr);
    }
    bool frame(const Builder& b, NdsGxStream* out) {
        run(g, b);
        const bool flushed = g.GeometryEnabled && g.FlushRequest;
        g.VBlank();
        NdsGxStream ignored;
        return recorder.after_vblank(g, flushed, out ? out : &ignored);
    }
};

float centroid_x(const Polygon& p) {
    float x = 0;
    for (uint32_t v = 0; v < p.NumVertices; ++v) x += float(p.Vertices[v]->HiresPosition[0]);
    return x / float(p.NumVertices) / 16.f;
}

bool same_polygon(const Polygon& a, const Polygon& b) {
    if (a.NumVertices != b.NumVertices || a.Attr != b.Attr || a.Translucent != b.Translucent ||
        a.FacingView != b.FacingView || a.WBuffer != b.WBuffer)
        return false;
    for (uint32_t v = 0; v < a.NumVertices; ++v) {
        const auto& va = *a.Vertices[v];
        const auto& vb = *b.Vertices[v];
        for (int c = 0; c < 2; ++c) {
            if (va.FinalPosition[c] != vb.FinalPosition[c] ||
                va.HiresPosition[c] != vb.HiresPosition[c])
                return false;
        }
        for (int c = 0; c < 3; ++c)
            if (va.FinalColor[c] != vb.FinalColor[c]) return false;
        if (va.TexCoords[0] != vb.TexCoords[0] || va.TexCoords[1] != vb.TexCoords[1] ||
            a.FinalZ[v] != b.FinalZ[v] || a.FinalW[v] != b.FinalW[v])
            return false;
    }
    return true;
}

Builder scene(int32_t x) {
    Builder b;
    b.begin();
    b.model(x);
    b.triangle();
    b.swap();
    return b;
}

void test_replay_matches_engine() {
    Engine e;
    NdsGxStream stream;
    Builder b;
    b.begin();
    b.model(-kOne / 2, kOne / 4);
    b.cmd(0x11);  // push, so the stack is used
    b.triangle();
    b.translate(kOne / 3, -kOne / 5, 0);
    b.triangle(0x300);
    b.cmd(0x12, {1});
    b.swap();
    check(e.frame(b, &stream), "a recorded image is complete");
    check(stream.complete && stream.size() == b.commands.size(), "every command recorded");

    NdsGxReplayer replayer(e.nds);
    check(replayer.run(stream), "replay reaches the swap");
    check(replayer.count() == e.g.RenderNumPolygons && replayer.count() == 2,
          "replay draws as many polygons");
    for (uint32_t i = 0; i < replayer.count() && i < e.g.RenderNumPolygons; ++i)
        check(same_polygon(*replayer.polygons()[i], *e.g.RenderPolygonRAM[i]),
              "replayed polygon equals the engine's");

    // Replaying again (the other bank) is the same.
    check(replayer.run(stream) && replayer.count() == 2 &&
              same_polygon(*replayer.polygons()[0], *e.g.RenderPolygonRAM[0]),
          "a second replay is the same");
}

void test_halfway() {
    Engine e;
    NdsGxStream a, b, mid;
    check(e.frame(scene(0), &a), "image a");
    const float xa = centroid_x(*e.g.RenderPolygonRAM[0]);
    check(e.frame(scene(kOne / 4), &b), "image b");
    const float xb = centroid_x(*e.g.RenderPolygonRAM[0]);
    NdsGxLerpStats stats;
    check(nds_gx_lerp_streams(a, b, 0.5f, &mid, &stats), "the pair blends");
    check(stats.moving_units == 1 && stats.blended_units == 1, "one moving unit, blended");
    NdsGxReplayer replayer(e.nds);
    check(replayer.run(mid) && replayer.count() == 1, "halfway image draws");
    if (replayer.count() == 1) {
        const float xm = centroid_x(*replayer.polygons()[0]);
        check(std::abs(xm - (xa + xb) / 2) < 1.f, "the triangle is halfway");
        check(xb - xa > 20.f, "it moved enough to tell");
    }
    // t = 1 is b exactly.
    NdsGxStream end;
    check(nds_gx_lerp_streams(a, b, 1.f, &end, nullptr) && end.params == b.params,
          "t = 1 gives b's parameters");
}

// Two copies of one model; the first leaves the view. The one left must blend
// with itself, not with the one that left.
void test_look_alikes() {
    Engine e;
    NdsGxStream a, b, mid;
    Builder two;
    two.begin();
    two.model(-kOne / 2);
    two.triangle();
    two.model(kOne / 2);
    two.triangle();
    two.swap();
    check(e.frame(two, &a), "image a");
    Builder one;
    one.begin();
    one.model(kOne / 2 + kOne / 16);
    one.triangle();
    one.swap();
    check(e.frame(one, &b), "image b");
    const float xb = centroid_x(*e.g.RenderPolygonRAM[0]);
    check(nds_gx_lerp_streams(a, b, 0.5f, &mid, nullptr), "the pair blends");
    NdsGxReplayer replayer(e.nds);
    check(replayer.run(mid) && replayer.count() == 1, "halfway image draws one copy");
    if (replayer.count() == 1) {
        const float xm = centroid_x(*replayer.polygons()[0]);
        // Its own half step is 1/32 of the half screen (4 px); the other copy
        // would put it 64 px away.
        check(std::abs(xm - xb) < 8.f, "the copy blends with itself");
    }
}

// Another mesh under the same matrices is not the same thing.
void test_other_mesh() {
    Engine e;
    NdsGxStream a, b, mid;
    check(e.frame(scene(0), &a), "image a");
    Builder other;
    other.begin();
    other.model(kOne / 4);
    other.triangle(0x280);
    other.swap();
    check(e.frame(other, &b), "image b");
    NdsGxLerpStats stats;
    check(!nds_gx_lerp_streams(a, b, 0.5f, &mid, &stats) && stats.blended_units == 0,
          "different meshes do not blend");
}

// A cut: the same model far away.
void test_jump() {
    Engine e;
    NdsGxStream a, b, mid;
    check(e.frame(scene(0), &a), "image a");
    check(e.frame(scene(int32_t(kOne) * 40), &b), "image b");
    check(!nds_gx_lerp_streams(a, b, 0.5f, &mid, nullptr), "a jump does not blend");
}

void test_start_needs_a_swap() {
    melonDS::NDS nds;
    GPU3D& g = nds.GPU.GPU3D;
    g.Reset();
    g.SetEnabled(true, true);
    NdsGxRecorder recorder;
    recorder.attach(g);
    NdsGxStream out;
    run(g, scene(0));
    const bool flushed = g.FlushRequest != 0;
    g.VBlank();
    check(!recorder.after_vblank(g, flushed, &out),
          "the first image after attach has no known start");
    run(g, scene(0));
    g.VBlank();
    check(recorder.after_vblank(g, true, &out), "the next one does");
}

}  // namespace

int main() {
    test_replay_matches_engine();
    test_halfway();
    test_look_alikes();
    test_other_mesh();
    test_jump();
    test_start_needs_a_swap();
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("gpu3d_gxstream_test: all passed\n");
    return 0;
}
