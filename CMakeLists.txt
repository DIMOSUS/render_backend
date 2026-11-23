cmake_minimum_required(VERSION 3.24)

project(RenderBackend LANGUAGES C CXX)

# Standalone builds default to the static CRT (/MT). Embedded builds use the parent's runtime setting.
if(PROJECT_IS_TOP_LEVEL AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
endif()

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "RenderBackend requires the Windows SDK and MSVC-compatible Windows C++17 tools")
endif()

option(RENDER_BACKEND_BUILD_EXAMPLE "Build the spinning mesh example" ${PROJECT_IS_TOP_LEVEL})
option(RENDER_BACKEND_BUILD_TESTS "Build the backend tests" ${PROJECT_IS_TOP_LEVEL})
option(RENDER_BACKEND_BUILD_GPU_TESTS "Build the tests that need a D3D12 hardware adapter" OFF)
option(RENDER_BACKEND_CHECKS "Enable additional backend validation" OFF)
option(RENDER_BACKEND_EXPORT_SDK
    "Copy the patched NVRHI and pinned DirectX-Headers include trees to <build>/consumer/include at configure time" OFF)
option(RENDER_BACKEND_DISABLE_MSVC_STL_EXCEPTIONS "Use the MSVC STL with _HAS_EXCEPTIONS=0" OFF)

# Public headers expose STL and EASTL types, so consumers must compile with the same _HAS_EXCEPTIONS setting.
set(RENDER_BACKEND_MSVC_STL_DEFINITION "$<$<BOOL:${RENDER_BACKEND_DISABLE_MSVC_STL_EXCEPTIONS}>:_HAS_EXCEPTIONS=0>")

set(RENDER_BACKEND_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

function(render_backend_configure_target target)
    target_compile_features(${target} PUBLIC cxx_std_17)
    target_include_directories(${target} PUBLIC "$<BUILD_INTERFACE:${RENDER_BACKEND_ROOT}/include>")
    target_compile_definitions(${target} PUBLIC "${RENDER_BACKEND_MSVC_STL_DEFINITION}")
    target_compile_options(${target} PRIVATE /W3 /GR- /bigobj /Zc:threadSafeInit- /fp:precise)
endfunction()

include(cmake/Dependencies.cmake)
render_backend_shader_dependencies()
render_backend_gpu_dependencies()

add_library(backend_eastl_allocator STATIC src/eastl_allocator.cpp)
render_backend_configure_target(backend_eastl_allocator)

# For builds outside CMake (for example MSBuild projects) that compile against the same third-party headers.
if(RENDER_BACKEND_EXPORT_SDK)
    file(COPY "${RENDER_BACKEND_NVRHI_INCLUDE}/nvrhi" "${RENDER_BACKEND_DIRECTX_INCLUDE}/directx"
              "${RENDER_BACKEND_DIRECTX_INCLUDE}/wsl" DESTINATION "${CMAKE_BINARY_DIR}/consumer/include")
endif()

add_subdirectory(src/shader)
add_subdirectory(src/render)
# EASTL expects the application to define its two array operator new overloads. This static library supplies
# them; an application that defines both itself takes precedence, because the linker then never pulls this one in.
target_link_libraries(backend_shader PUBLIC backend_eastl_allocator)

if(RENDER_BACKEND_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(RENDER_BACKEND_BUILD_EXAMPLE)
    add_subdirectory(examples/spinning_mesh)
endif()
