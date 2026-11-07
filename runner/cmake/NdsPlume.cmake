# The Plume renderer host (NDS_ACCELERATED_RENDERER=PLUME).
#
# Plume is a low level render hardware interface over Vulkan, D3D12 and Metal
# (external/plume, the fork Eternal Sonata Reprise uses). It compiles no
# shaders of its own and has no OpenGL backend: every shader is HLSL under
# src/plume/shaders, compiled ahead of time by DXC to SPIR-V (and DXIL on
# Windows) and embedded in the executable as byte arrays. Nothing is compiled
# by the driver at startup, unlike the GLSL the two GL backends hand to
# glCompileShader.
#
# This replaces the GL host (src/melonds_compute/ComputeHost.cpp) behind the
# same ComputeHost.h API, so the frontend drives it unchanged. None of the GL
# sources are built: no glad, no OpenGLSupport, no GL texture cache.
include_guard(GLOBAL)

function(nds_add_plume_renderer target)
    set(_plume_dir "${CMAKE_CURRENT_SOURCE_DIR}/../external/plume")
    if(NOT EXISTS "${_plume_dir}/plume_render_interface.h")
        message(FATAL_ERROR
            "Plume submodule is missing. Run: git submodule update --init --recursive")
    endif()
    if(NOT NDS_SDL_BACKEND STREQUAL "SDL3")
        message(FATAL_ERROR "NDS_ACCELERATED_RENDERER=PLUME requires NDS_SDL_BACKEND=SDL3")
    endif()

    # Linux: Plume builds its Vulkan surface straight from the frontend's
    # SDL_Window. Not on Apple, where the same #elif chain in Plume would swap
    # its native {NSWindow, CAMetalLayer} pair for a bare SDL_Window* that its
    # Metal surface code does not accept.
    if(UNIX AND NOT APPLE AND NOT ANDROID)
        set(PLUME_SDL3_VULKAN_ENABLED ON CACHE BOOL "Enable SDL3 Vulkan integration" FORCE)
    endif()
    add_subdirectory("${_plume_dir}" "${CMAKE_BINARY_DIR}/plume" EXCLUDE_FROM_ALL)

    # Plume's D3D12 backend names D3D12_HEAP_TYPE_GPU_UPLOAD unguarded, which
    # only exists in Windows SDK 10.0.26100+ or the Agility SDK. Supply the one
    # enumerator, and only after checking it is really missing, so this goes
    # away by itself on a newer SDK. Nothing here requests such a heap.
    if(WIN32)
        include(CheckCXXSourceCompiles)
        set(CMAKE_REQUIRED_QUIET ON)
        check_cxx_source_compiles(
            "#include <d3d12.h>\nint main() { return D3D12_HEAP_TYPE_GPU_UPLOAD; }"
            NDS_HAVE_D3D12_GPU_UPLOAD_HEAP)
        unset(CMAKE_REQUIRED_QUIET)
        if(NOT NDS_HAVE_D3D12_GPU_UPLOAD_HEAP)
            target_compile_definitions(plume PRIVATE
                "D3D12_HEAP_TYPE_GPU_UPLOAD=((D3D12_HEAP_TYPE)5)")
        endif()
    endif()

    # Fetches a pinned DXC unless PLUME_DXC_EXECUTABLE is given. The Windows
    # SDK's DXC has no SPIR-V backend, so the fetched one is used there too.
    include("${_plume_dir}/examples/cmake/PlumeShaders.cmake")
    plume_shaders_init()

    set(_shaders "${CMAKE_CURRENT_SOURCE_DIR}/src/plume/shaders")
    # name, file, entry point, stage. gx.hlsl holds every 3D pass (several
    # entry points sharing one constant block), compose.hlsl the HD composite.
    set(_blobs
        "ndsFullscreenVert|fullscreen.vert.hlsl|VSMain|vertex"
        "ndsBlitFrag|blit.frag.hlsl|PSMain|pixel"
        "ndsComposeFrag|compose.hlsl|PSMain|pixel"
        "ndsGxVertZ|gx.hlsl|VSMainZ|vertex"
        "ndsGxVertW|gx.hlsl|VSMainW|vertex"
        "ndsGxOpaqueZ|gx.hlsl|PSOpaqueZ|pixel"
        "ndsGxOpaqueW|gx.hlsl|PSOpaqueW|pixel"
        "ndsGxTransZ|gx.hlsl|PSTransZ|pixel"
        "ndsGxTransW|gx.hlsl|PSTransW|pixel"
        "ndsGxShadowZ|gx.hlsl|PSShadowZ|pixel"
        "ndsGxShadowW|gx.hlsl|PSShadowW|pixel"
        "ndsGxNull|gx.hlsl|PSNull|pixel"
        "ndsGxFinal|gx.hlsl|PSFinal|pixel")
    set(_blob_headers)
    foreach(_blob IN LISTS _blobs)
        string(REPLACE "|" ";" _parts "${_blob}")
        list(GET _parts 0 _name)
        list(GET _parts 1 _file)
        list(GET _parts 2 _entry)
        list(GET _parts 3 _stage)
        if(_stage STREQUAL "vertex")
            plume_compile_vertex_shader(${target} "${_shaders}/${_file}" "${_name}" "${_entry}")
        else()
            plume_compile_pixel_shader(${target} "${_shaders}/${_file}" "${_name}" "${_entry}")
        endif()
        list(APPEND _blob_headers "${CMAKE_BINARY_DIR}/shaders/${_name}.hlsl.spirv.h")
    endforeach()
    # The generated blob headers have no dependency edge of their own until a
    # source includes them; make the Plume sources wait for all of them.
    set_property(SOURCE src/plume/PlumeHost.cpp src/plume/Plume3D.cpp APPEND
        PROPERTY OBJECT_DEPENDS ${_blob_headers})

    target_sources(${target} PRIVATE
        src/plume/PlumeHost.cpp
        src/plume/Plume3D.cpp
        src/melonds_compute/TextureUpscaleStub.cpp)
    target_compile_definitions(${target} PRIVATE
        NDS_HAVE_COMPUTE_RENDERER=1
        NDS_USE_PLUME=1)
    target_link_libraries(${target} PRIVATE plume)
endfunction()
