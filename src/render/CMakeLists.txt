# Compile the scatter-upload compute shader at build time and embed the DXIL in the library, so applications
# need neither the shader source nor DXC at run time. DXC is taken from the Windows SDK unless
# RENDER_BACKEND_DXC_EXECUTABLE is set.
cmake_host_system_information(RESULT windows_kits_root QUERY WINDOWS_REGISTRY
    "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots" VALUE KitsRoot10 VIEW 32)
find_program(RENDER_BACKEND_DXC_EXECUTABLE NAMES dxc HINTS
    "${windows_kits_root}/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
    "$ENV{WindowsSdkDir}/bin/$ENV{WindowsSDKVersion}/x64")
if(NOT RENDER_BACKEND_DXC_EXECUTABLE)
    # Fall back to the newest installed SDK that ships dxc.exe.
    file(GLOB windows_sdk_versions LIST_DIRECTORIES true "${windows_kits_root}/bin/10.*")
    list(SORT windows_sdk_versions COMPARE NATURAL ORDER DESCENDING)
    foreach(version IN LISTS windows_sdk_versions)
        if(EXISTS "${version}/x64/dxc.exe")
            set(RENDER_BACKEND_DXC_EXECUTABLE "${version}/x64/dxc.exe")
            break()
        endif()
    endforeach()
endif()
if(NOT RENDER_BACKEND_DXC_EXECUTABLE)
    message(FATAL_ERROR "DXC is required to build the scatter-upload shader. Set RENDER_BACKEND_DXC_EXECUTABLE to dxc.exe.")
endif()
set(scatter_dxil "${CMAKE_CURRENT_BINARY_DIR}/scatter_upload_dxil.hpp")
add_custom_command(OUTPUT "${scatter_dxil}"
    COMMAND "${RENDER_BACKEND_DXC_EXECUTABLE}" -T cs_6_6 -E ScatterUpload -O3
        -Qstrip_debug -Qstrip_reflect -Fh "${scatter_dxil}" -Vn c_scatterUploadDxil
        "${CMAKE_CURRENT_SOURCE_DIR}/scatter_upload.hlsl"
    DEPENDS scatter_upload.hlsl VERBATIM)

add_library(backend_render STATIC
    scatter_upload.cpp
    "${scatter_dxil}"
    context.cpp
    playback.cpp
    frame_executor.cpp
    resource_use.cpp
    render_packets.cpp
    frame_graph.cpp
    upload_queue.cpp
    target_pool.cpp
    diagnostics.cpp
    markers.cpp
    memory.cpp
    window_frame.cpp
    vertex_layout.cpp
    dx12_platform.cpp
    dds_texture.cpp
    texture_load.cpp
    renderer.cpp
)
render_backend_configure_target(backend_render)
target_include_directories(backend_render PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(backend_render PRIVATE RENDER_BACKEND_RUNTIME_CHECKS=$<BOOL:${RENDER_BACKEND_CHECKS}>)
target_link_libraries(backend_render PUBLIC render_backend::nvrhi render_backend::eastl render_backend::shader
    PRIVATE render_backend::stb d3d12 dxgi user32)
add_library(render_backend::render ALIAS backend_render)
