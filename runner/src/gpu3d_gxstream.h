#pragma once

// The DS geometry engine's input, kept per 3D image: the command stream the
// game sent between two SWAP_BUFFERS and the engine state it started from.
// Two of them can be blended and redrawn (matrices interpolated, everything
// else from the newer image), which is how smooth_3d builds the image between
// two real ones. The blend runs before projection and clipping, so meshes stay
// closed and clipping stays exact; nothing is paired after the fact.
//
// Host-only: the recorder observes the real GPU3D, and the replay runs on a
// private GPU3D the guest never sees.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "GPU3D.h"

namespace melonDS {
class NDS;
}

// The part of GPU3D that a command stream reads without setting it first.
// Plain data, so a trace can store it as bytes.
struct NdsGxGeometryState {
    uint32_t matrix_mode = 0;
    int32_t proj[16] = {};
    int32_t pos[16] = {};
    int32_t vec[16] = {};
    int32_t tex[16] = {};
    int32_t proj_stack[16] = {};
    int32_t pos_stack[32][16] = {};
    int32_t vec_stack[32][16] = {};
    int32_t tex_stack[16] = {};
    int32_t proj_sp = 0, pos_sp = 0, tex_sp = 0;
    uint32_t viewport[6] = {};
    uint32_t polygon_mode = 0;
    int16_t cur_vertex[3] = {};
    uint8_t vertex_color[3] = {};
    uint8_t use_shininess = 0;
    int16_t tex_coords[2] = {};
    int16_t raw_tex_coords[2] = {};
    int16_t normal[3] = {};
    int16_t light_direction[4][3] = {};
    int32_t spec_recip[4] = {};
    uint8_t light_color[4][3] = {};
    uint8_t mat_diffuse[3] = {};
    uint8_t mat_ambient[3] = {};
    uint8_t mat_specular[3] = {};
    uint8_t mat_emission[3] = {};
    uint8_t shininess[128] = {};
    uint32_t polygon_attr = 0;
    uint32_t cur_polygon_attr = 0;
    uint32_t tex_param = 0;
    uint32_t tex_palette = 0;
    uint32_t zero_dot_w_limit = 0;
    uint32_t flush_attributes = 0;
    uint32_t vertex_num = 0;
    uint32_t vertex_num_in_poly = 0;
    uint32_t num_consecutive_polygons = 0;
    uint32_t render_width = 256;
    uint32_t guest_wide = 0;
    melonDS::Vertex temp_vertices[4] = {};
};

void nds_gx_capture_state(const melonDS::GPU3D& gpu3d, NdsGxGeometryState* out);
void nds_gx_apply_state(const NdsGxGeometryState& state, melonDS::GPU3D* gpu3d);

struct NdsGxStream {
    NdsGxGeometryState start;
    std::vector<uint8_t> ops;
    std::vector<uint32_t> first;   // index of each command's first parameter
    std::vector<uint32_t> params;
    bool complete = false;          // ends with the SWAP_BUFFERS

    void clear() {
        ops.clear();
        first.clear();
        params.clear();
        complete = false;
    }
    size_t size() const { return ops.size(); }
    uint32_t count(size_t i) const {
        return (i + 1 < first.size() ? first[i + 1]
                                     : static_cast<uint32_t>(params.size())) -
               first[i];
    }
    void push(uint8_t op, const uint32_t* values, uint32_t n) {
        ops.push_back(op);
        first.push_back(static_cast<uint32_t>(params.size()));
        params.insert(params.end(), values, values + n);
    }
};

// Records what the real geometry engine executes, one stream per image.
class NdsGxRecorder {
public:
    void attach(melonDS::GPU3D& gpu3d);
    void detach(melonDS::GPU3D& gpu3d);
    bool attached() const { return attached_; }
    // Right after GPU3D::VBlank. `flushed`: that VBlank consumed a
    // SWAP_BUFFERS. Then the finished stream is swapped into `out` and true
    // returned, unless its start was never seen (attach, savestate load).
    bool after_vblank(const melonDS::GPU3D& gpu3d, bool flushed, NdsGxStream* out);
    // The stream being recorded did not start at a known state.
    void invalidate() { valid_ = false; }

private:
    static void observe(void* self, uint32_t command, const uint32_t* params,
                        uint32_t count);
    NdsGxStream recording_;
    bool valid_ = false;
    bool attached_ = false;
};

struct NdsGxLerpStats {
    size_t units = 0;         // draw units of the newer image
    size_t moving_units = 0;  // units that set matrices
    size_t blended_units = 0; // of those, found in the older image
    size_t commands = 0;
};

// The stream between `a` (t = 0) and `b` (t = 1): b's commands, with the
// matrix parameters of the units both images share blended, and the start
// state's matrices blended too. A unit is a run of matrix commands and what
// is drawn with it; it is the same in both images when every command other
// than a matrix one is identical (same vertices, texcoords, normals, stack
// slots), and among look-alikes (two copies of one model) the one whose
// matrices moved least is taken. False when too little of `b` is in `a`.
// How each unit of `b` was treated, for diagnostics.
enum class NdsGxUnitStatus : uint8_t {
    Still,        // sets no blended matrix
    Blended,      // matched in `a`
    NoSignature,  // nothing in `a` draws the same
    Implausible,  // same signature, but a matrix entry jumped too far
    Taken,        // its look-alikes in `a` went to cheaper matches
};

struct NdsGxUnitMatch {
    uint32_t begin = 0, end = 0;  // command range in `b`
    int32_t a_begin = -1;         // its partner's first command in `a`
    NdsGxUnitStatus status = NdsGxUnitStatus::Still;
    int64_t cost = -1;
};

bool nds_gx_lerp_streams(const NdsGxStream& a, const NdsGxStream& b, float t,
                         NdsGxStream* out, NdsGxLerpStats* stats,
                         std::vector<NdsGxUnitMatch>* units = nullptr);

// A private geometry engine that redraws streams.
class NdsGxReplayer {
public:
    explicit NdsGxReplayer(melonDS::NDS& nds);
    ~NdsGxReplayer();
    // Runs `stream` from its start state and builds the render list the way
    // GPU3D::VBlank does. The list stays valid until the second next run.
    bool run(const NdsGxStream& stream);
    melonDS::Polygon* const* polygons() const;
    uint32_t count() const;
    melonDS::GPU3D& gpu();

private:
    std::unique_ptr<melonDS::GPU3D> gpu_;
};

// NDS_GX_CMD_TRACE: per image, the stream and the real engine's render list
// for it, so the replay can be checked against the hardware model offline.
//   "GXCI", u32 vblank, u32 sizeof(state), state, u32 commands, u32 params,
//   u8 ops[commands], u8 counts[commands] (parameters per command),
//   u32 params[params], u32 polygons, then per polygon
//   u32 attr, tex_param, tex_palette, vertex count, flags (1 translucent,
//   2 shadow mask, 4 shadow, 8 facing view, 16 w-buffer, 32 degenerate,
//   64 line), s32 final_z[10], final_w[10], and its vertices as melonDS
//   Vertex bytes.
//   A "GXMK", u32 vblank, u32 mark record (the user's mark key, at that
//   vblank of the same count) comes before the first image traced after it.
void nds_gx_trace_image(FILE* file, uint32_t vblank, const NdsGxStream& stream,
                        const melonDS::GPU3D& gpu3d);
void nds_gx_trace_image(std::vector<uint8_t>* out, uint32_t vblank,
                        const NdsGxStream& stream, const melonDS::GPU3D& gpu3d);
void nds_gx_trace_mark(FILE* file, uint32_t vblank, uint32_t mark);

struct NdsGxTracePolygon {
    uint32_t attr = 0, tex_param = 0, tex_palette = 0, num_vertices = 0, flags = 0;
    int32_t final_z[10] = {}, final_w[10] = {};
    melonDS::Vertex vertices[10] = {};
};

struct NdsGxTraceImage {
    uint32_t vblank = 0;
    std::vector<uint32_t> marks;  // vblanks of marks recorded just before this image
    NdsGxStream stream;
    std::vector<NdsGxTracePolygon> polygons;
};

// Reads one image; false at the end or on a bad record.
bool nds_gx_trace_read(FILE* file, NdsGxTraceImage* out);
void nds_gx_trace_polygon(const melonDS::Polygon& polygon, NdsGxTracePolygon* out);
