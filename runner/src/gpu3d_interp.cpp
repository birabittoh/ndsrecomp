#include "gpu3d_interp.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <deque>
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
// With NDS_GX_CMD_TRACE_RING, the most images written around marks.
constexpr uint32_t kMaxMarkedTraceImages = 20000;

// Mark key presses not yet seen by the emulation thread.
std::atomic<uint32_t> g_mark_requests{0};

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
    // NDS_GX_CMD_TRACE_RING=<n>: keep the last n images in memory and write
    // them, and the n/4 after, only when the mark key is pressed.
    uint32_t ring_size = 0;
    std::deque<std::vector<uint8_t>> ring;
    uint32_t write_after = 0;  // images still to write since the last mark
    uint32_t marks_seen = 0;
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
    InterpState& s = state();
    s.trace = file;
    if (const char* ring = std::getenv("NDS_GX_CMD_TRACE_RING"))
        s.ring_size = static_cast<uint32_t>(std::strtoul(ring, nullptr, 10));
    if (file && s.ring_size)
        std::fprintf(stderr,
                     "[gx] command trace keeps the last %u images; press M to save "
                     "them when something looks wrong\n",
                     s.ring_size);
}

void nds_gx_interp_mark() {
    g_mark_requests.fetch_add(1, std::memory_order_relaxed);
}

namespace {

// Writes a traced image now (no ring), or keeps it for the next mark.
void trace_image(InterpState& s, std::vector<uint8_t>&& bytes) {
    if (!s.ring_size) {
        std::fwrite(bytes.data(), 1, bytes.size(), s.trace);
        if (++s.traced == kMaxTraceImages) std::fprintf(stderr, "[gx] command trace full\n");
        std::fflush(s.trace);
        return;
    }
    if (s.write_after > 0) {
        std::fwrite(bytes.data(), 1, bytes.size(), s.trace);
        ++s.traced;
        if (--s.write_after == 0) {
            std::fflush(s.trace);
            std::fprintf(stderr, "[gx] mark %u saved (%u images in the trace)\n",
                         s.marks_seen, s.traced);
        }
        return;
    }
    s.ring.push_back(std::move(bytes));
    while (s.ring.size() > s.ring_size) s.ring.pop_front();
}

void trace_marks(InterpState& s) {
    const uint32_t requests = g_mark_requests.exchange(0, std::memory_order_relaxed);
    if (!requests || !s.trace) return;
    for (uint32_t i = 0; i < requests; ++i) {
        ++s.marks_seen;
        std::fprintf(stderr, "[gx] mark %u at vblank %llu\n", s.marks_seen,
                     static_cast<unsigned long long>(s.vblank));
        // The ring first, so the mark sits right after the image on screen.
        for (const std::vector<uint8_t>& bytes : s.ring) {
            std::fwrite(bytes.data(), 1, bytes.size(), s.trace);
            ++s.traced;
        }
        s.ring.clear();
        nds_gx_trace_mark(s.trace, static_cast<uint32_t>(s.vblank), s.marks_seen);
    }
    std::fflush(s.trace);
    if (s.ring_size) s.write_after = std::max<uint32_t>(30, s.ring_size / 4);
}

}  // namespace

void nds_gx_interp_after_vblank(GPU3D& gpu3d, melonDS::NDS& nds, bool active,
                                NdsGxInterpTarget target, bool flushed,
                                bool swap, bool new_image,
                                NdsGxInterpStats* stats) {
    InterpState& s = state();
    ++s.vblank;
    trace_marks(s);
    const bool tracing =
        s.trace && s.traced < (s.ring_size ? kMaxMarkedTraceImages : kMaxTraceImages);
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
        std::vector<uint8_t> bytes;
        nds_gx_trace_image(&bytes, static_cast<uint32_t>(s.vblank), s.incoming, gpu3d);
        trace_image(s, std::move(bytes));
    }

    const uint64_t gap = s.vblank - s.last_vblank;
    if (active && s.last_valid && gap == 1) {
        // The game drew this image one DS frame after the last: there is no
        // free frame to fill, so the last image is shown again, exactly,
        // keeping 3D one frame late as after a blend. A game at 60 that
        // misses a frame then holds the newer image, and the image after
        // that comes two frames apart and gets the halfway image, so a slip
        // shows as smooth motion instead of a repeated frame.
        if (!s.replayer) s.replayer = std::make_unique<NdsGxReplayer>(nds);
        if (s.replayer->run(s.last) && s.replayer->count() > 0)
            show_blend(gpu3d, target, s.replayer->polygons(), s.replayer->count());
    } else if (active && s.last_valid && gap <= kMaxImageGap) {
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
