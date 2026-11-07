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
3D layer itself (the OBJ-only direct path and the HD path). The GL host
does. The Plume host does not yet, so the frontend keeps direct present and
HD layer emission off and the CPU composite carries the 3D layer, exactly
as on the SDL path.

The presenter mirrors the GL one: `nds_single_layout` for the rect, the
same nearest/bilinear choice, the integer-mode box resolve, the inset and
the alpha-blended overlay. Images are top row first everywhere, with no GL
flips. Two frames in flight, each with its own command list, fence, upload
buffer and textures; a slot is reused only after its fence signals. Vsync is
off, as the GL presenter's swap interval 0 is: the frontend paces from the
audio queue.

## Status

Milestone 1, presentation: done. Linux/Vulkan verified headless (600
frames on a GTX 1060; guest frame hashes identical to a soft-3D GL build).
Windows/D3D12 and macOS are not built or tested yet; macOS has no surface
wiring (`{NSWindow, CAMetalLayer}`).

Next:

1. The 3D passes: port `GPU3D_OpenGL.cpp` and `GPU3D_OpenGL_shaders.h` to a
   Plume `Renderer3D` with HLSL shaders. It keeps the readback contract:
   256..448-wide integer output, RGB6/alpha5 plus six polygon-ID bits, PBO-
   style readback for the faithful compositor, HD normalisation. Guest
   capture still comes from the retained integer rasterizer.
2. The compose shader (`kPresentFragment` in `PresentShaders.h`): direct OBJ
   and HD paths sampling that renderer's surfaces, then
   `nds_compute_host_composes_3d()` returns true.
3. `nds_compute_host_read_presented` (debug server observer): needs an
   offscreen copy of the presented rect; it returns false today.
4. Vsync via MAILBOX where the surface has it, and a Vulkan validation-layer
   run.
