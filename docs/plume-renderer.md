# Plume renderer host

`NDS_ACCELERATED_RENDERER=PLUME` builds the runner's renderer host on
[Plume](https://github.com/birabittoh/plume) (`external/plume`, the fork
Eternal Sonata Reprise uses): an RHI over Vulkan, D3D12 and Metal. The top
window then carries a Vulkan surface (Linux) or a D3D12 swap chain (Windows)
instead of a GL context, and every shader is HLSL compiled ahead of time.
Nothing is compiled by the driver at startup.

The two GL backends (`COMPUTE`, `OPENGL`) are unchanged and remain the
default. This is a third compile-time choice, not a runtime switch.

## Build

```sh
git submodule update --init --recursive      # external/plume and its contrib/
cmake -S runner -B build-plume -G Ninja -DNDS_ACCELERATED_RENDERER=PLUME ...
```

Configure fetches a pinned DXC (`renderbag/dxc-bin`) through Plume's
`PlumeShaders.cmake`, unless `PLUME_DXC_EXECUTABLE` names one. DXC turns
`runner/src/plume/shaders/*.hlsl` into SPIR-V (and DXIL on Windows), and
Plume's `file_to_c` embeds each blob as a byte array (`<build>/shaders/`).

A second build directory beside a GL one does not need to recompile the
banks: pass the GL build's archive with `NDS_PREBUILT_BANK_ARCHIVE` and its
`NDS_PREBUILT_BANK_ARCHIVE_SHA256`. Only runner sources then compile (about
a minute). The SHA has to be refreshed whenever the GL build rebuilds the
banks.

## Run

`NDS_3D_RENDERER=plume` is strict (a failed device or swap chain is fatal);
`auto` falls back to the SDL renderer path with threaded software 3D, like
the GL backends. On Windows `NDS_PLUME_API=vulkan` asks for Vulkan before
D3D12.

## Design

`src/plume/PlumeHost.cpp` implements the same `ComputeHost.h` API as the GL
host, so the frontend drives it unchanged. `cmake/NdsPlume.cmake` wires the
submodule, DXC and the shaders, and builds none of the GL sources (no glad,
OpenGLSupport or GL texture cache). In `gpu3d.cpp` the GL bridge (PBO
readback, `glFinish` around rebuilds, the melonDS GL/compute renderer) is
gated on `NDS_GPU3D_GL_BRIDGE`, which the Plume build does not define.

`nds_compute_host_composes_3d()` says whether a host's presenter composes the
3D layer itself; `nds_compute_host_presents_direct()` whether it can also take
the OBJ-only direct path, which needs native 3D on the GPU. The GL host does
both. The Plume host composes HD frames only: native 3D stays on the CPU.

### 3D (Plume3D.cpp)

The integer soft renderer stays the installed `Renderer3D` and serves every
faithful consumer (2D compositor lines, capture, readback). Above 1x,
`nds_gpu3d_vcount215` also hands the same latched render list to the Plume
renderer, which draws it for presentation only. Nothing is read back.

The raster has **any size**. The host sizes it from the rect it presents
into (`resolution_scale` percent of it, or `internal_resolution` x native),
so 3D lands 1:1 on screen pixels instead of at a whole multiple of 256x192.
Vertices are melonDS's 1/16 pixel `HiresPosition` times a float ratio per
axis. The frontend's integer internal scale only switches HD on (> 1).

The passes port `GPU3D_OpenGL.cpp`: the same render keys, batching and
opaque / translucent / shadow stencil programs. GL state becomes Plume
pipelines, built on first use and keyed by every state the passes toggle,
with the stencil reference dynamic. One final pass does edge marking, fog
and the RGB6/alpha5 quantisation (`shaders/gx.hlsl`); lines become quads one
DS pixel thick. The compose shader (`shaders/compose.hlsl`) is
`kPresentFragment`'s HD path. It draws straight into the rect when the raster
is the rect's size; otherwise it draws offscreen and box-resolves or filters.

## Status

Milestone 1 (presentation): done, Linux/Vulkan verified (guest frame hashes
identical to a soft-3D GL build).

Milestones 2 and 3 (3D passes, HD compose): first draft, then fixes for the
first playtest (untested in game since):
- Freezes: emulation frames stalled for seconds while pipelines compiled in
  them. Pass 2 no longer needs a pipeline per polygon ID (bit 7 test plus
  REPLACE with the dynamic reference); every pipeline the passes can bind is
  found by a dry run and built on a background thread at start-up
  (`[plume3d] warm-up: N pipelines ready`); the driver pipeline cache is
  saved as `plume-pipelines.bin` beside settings.toml. Frames over 8 ms log
  `[plume3d] slow frame` with pipelines built and resizes. Raster resizes
  wait for the request to hold 15 frames. The host acquires a swap chain
  image only after every step that can fail.
- Outlines: `[display] thin_outlines` (default on, Esc menu) draws them one
  presented pixel wide; off gives one DS pixel.
- smooth_3d works: the soft renderer keeps the real list and only the Plume
  raster draws the halfway list (`nds_gx_interp_presented`). It needs HD
  (internal scale above 1).

Not done: antialiasing (resolution_scale above 100 supersamples instead),
Windows/D3D12 and macOS (no `{NSWindow, CAMetalLayer}` wiring),
`nds_compute_host_read_presented`, vsync via MAILBOX, a validation-layer run.
