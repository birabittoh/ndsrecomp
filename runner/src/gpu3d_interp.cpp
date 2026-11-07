#include "gpu3d_interp.h"

#include <algorithm>
#include <memory>
#include <vector>

#include "gpu3d_gxstream.h"

using melonDS::GPU3D;
using melonDS::Polygon;

namespace {

// Past this many DS frames apart, the game was not animating between them.
constexpr uint64_t kMaxImageGap = 4;
// Images per NDS_GX_CMD_TRACE file: about a minute of play at 30.
constexpr uint32_t kMaxTraceImages = 1800;

struct InterpState {
    NdsGxRecorder recorder;
    NdsGxStream last, incoming, halfway;
    std::unique_ptr<NdsGxReplayer> replayer;
    std::vector<Polygon*> real = std::vector<Polygon*>(2048);
    size_t real_count = 0;
    bool last_valid = false;
    bool showing_blend = false;
    Polygon* const* presented = nullptr;
    uint32_t presented_count = 0;
    uint64_t vblank = 0;
    uint64_t last_vblank = 0;
    FILE* trace = nullptr;
    uint32_t traced = 0;
};

InterpState& state() {
    static InterpState instance;
    return instance;
}

void show_blend(GPU3D& gpu3d, NdsGxInterpTarget target, Polygon* const* list,
                uint32_t count) {
    InterpState& s = state();
    if (target == NdsGxInterpTarget::Presentation) {
        s.presented = list;
        s.presented_count = count;
        return;
    }
    s.real_count = gpu3d.RenderNumPolygons;
    std::copy_n(gpu3d.RenderPolygonRAM.begin(), s.real_count, s.real.begin());
    std::copy_n(list, count, gpu3d.RenderPolygonRAM.begin());
    gpu3d.RenderNumPolygons = count;
    gpu3d.RenderFrameIdentical = false;
    s.showing_blend = true;
}

}  // namespace

void nds_gx_interp_show_real(GPU3D& gpu3d) {
    InterpState& s = state();
    s.presented = nullptr;
    s.presented_count = 0;
    if (!s.showing_blend) return;
    std::copy_n(s.real.begin(), s.real_count, gpu3d.RenderPolygonRAM.begin());
    gpu3d.RenderNumPolygons = static_cast<uint32_t>(s.real_count);
    gpu3d.RenderFrameIdentical = false;
    s.showing_blend = false;
}

bool nds_gx_interp_presented(Polygon* const** list, uint32_t* count) {
    const InterpState& s = state();
    if (!s.presented) return false;
    *list = s.presented;
    *count = s.presented_count;
    return true;
}

void nds_gx_interp_reset() {
    InterpState& s = state();
    s.last_valid = false;
    s.showing_blend = false;
    s.presented = nullptr;
    s.presented_count = 0;
    s.recorder.invalidate();
}

void nds_gx_interp_set_trace(FILE* file) {
    state().trace = file;
}

void nds_gx_interp_after_vblank(GPU3D& gpu3d, melonDS::NDS& nds, bool active,
                                NdsGxInterpTarget target, bool flushed,
                                bool swap, bool new_image,
                                NdsGxInterpStats* stats) {
    InterpState& s = state();
    ++s.vblank;
    const bool tracing = s.trace && s.traced < kMaxTraceImages;
    if (!active && !tracing) {
        nds_gx_interp_show_real(gpu3d);
        if (s.recorder.attached()) s.recorder.detach(gpu3d);
        s.last_valid = false;
        return;
    }
    if (!s.recorder.attached()) s.recorder.attach(gpu3d);
    const bool finished = s.recorder.after_vblank(gpu3d, flushed, &s.incoming);
    if (!swap) {
        nds_gx_interp_show_real(gpu3d);
        // An image drawn with rendering off was never shown.
        if (flushed) s.last_valid = false;
        return;
    }
    // GPU3D::VBlank has just rebuilt the render list from the bank.
    s.showing_blend = false;
    s.presented = nullptr;
    s.presented_count = 0;
    if (!finished || gpu3d.RenderNumPolygons == 0) {
        s.last_valid = false;
        return;
    }
    if (!new_image) return;
    if (tracing) {
        nds_gx_trace_image(s.trace, static_cast<uint32_t>(s.vblank), s.incoming, gpu3d);
        if (++s.traced == kMaxTraceImages) std::fprintf(stderr, "[gx] command trace full\n");
        std::fflush(s.trace);
    }

    if (active && s.last_valid && s.vblank - s.last_vblank <= kMaxImageGap) {
        NdsGxLerpStats lerp;
        bool shown = false;
        if (nds_gx_lerp_streams(s.last, s.incoming, 0.5f, &s.halfway, &lerp)) {
            if (!s.replayer) s.replayer = std::make_unique<NdsGxReplayer>(nds);
            if (s.replayer->run(s.halfway) && s.replayer->count() > 0) {
                show_blend(gpu3d, target, s.replayer->polygons(), s.replayer->count());
                shown = true;
            }
        }
        if (stats) {
            stats->units += lerp.moving_units;
            stats->blended_units += lerp.blended_units;
            if (shown) ++stats->images;
            else ++stats->skipped;
        }
    }
    std::swap(s.last, s.incoming);
    s.last_valid = true;
    s.last_vblank = s.vblank;
}
