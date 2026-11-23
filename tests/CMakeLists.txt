add_executable(backend_frame_graph_test frame_graph_test.cpp)
render_backend_configure_target(backend_frame_graph_test)
target_link_libraries(backend_frame_graph_test PRIVATE backend_render)
add_test(NAME backend_frame_graph COMMAND backend_frame_graph_test)
set_tests_properties(backend_frame_graph PROPERTIES TIMEOUT 10)

add_executable(backend_recording_context_test recording_context_test.cpp)
render_backend_configure_target(backend_recording_context_test)
target_link_libraries(backend_recording_context_test PRIVATE backend_render)
add_test(NAME backend_recording_context COMMAND backend_recording_context_test)
set_tests_properties(backend_recording_context PROPERTIES TIMEOUT 10)

add_executable(backend_dds_texture_test dds_texture_test.cpp)
render_backend_configure_target(backend_dds_texture_test)
target_link_libraries(backend_dds_texture_test PRIVATE backend_render)
add_test(NAME backend_dds_texture COMMAND backend_dds_texture_test)
set_tests_properties(backend_dds_texture PROPERTIES TIMEOUT 30)

add_executable(backend_packet_worker_test packet_worker_test.cpp)
render_backend_configure_target(backend_packet_worker_test)
target_link_libraries(backend_packet_worker_test PRIVATE backend_render)
add_test(NAME backend_packet_worker COMMAND backend_packet_worker_test)
set_tests_properties(backend_packet_worker PROPERTIES TIMEOUT 10)

# The shader tests take a base directory and work in a fresh subdirectory of it on every run.
add_executable(backend_shader_boundary_test shader_boundary_test.cpp)
render_backend_configure_target(backend_shader_boundary_test)
target_link_libraries(backend_shader_boundary_test PRIVATE backend_shader)
add_test(NAME backend_shader_boundary COMMAND backend_shader_boundary_test "${CMAKE_CURRENT_BINARY_DIR}/shader_boundary")
set_tests_properties(backend_shader_boundary PROPERTIES TIMEOUT 120)

add_executable(backend_shader_cache_test shader_cache_test.cpp)
render_backend_configure_target(backend_shader_cache_test)
target_link_libraries(backend_shader_cache_test PRIVATE backend_shader)
add_test(NAME backend_shader_cache COMMAND backend_shader_cache_test "${CMAKE_CURRENT_BINARY_DIR}/shader_cache")
set_tests_properties(backend_shader_cache PROPERTIES TIMEOUT 60)

# Hardware tests exit with 77 (reported as skipped) when no suitable D3D12 adapter or debug layer is available.
# They run one at a time so that they do not compete for the GPU.
if(RENDER_BACKEND_BUILD_GPU_TESTS)
    function(render_backend_add_gpu_test name source)
        add_executable(${name}_test ${source})
        render_backend_configure_target(${name}_test)
        target_compile_definitions(${name}_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        target_link_libraries(${name}_test PRIVATE backend_render d3d12 dxgi)
        add_test(NAME ${name} COMMAND ${name}_test ${ARGN})
        set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60 RUN_SERIAL TRUE)
    endfunction()

    render_backend_add_gpu_test(backend_bc_texture_gpu native_bc_texture_gpu_test.cpp --debug)
    render_backend_add_gpu_test(backend_upload_queue_gpu upload_queue_gpu_test.cpp)
    render_backend_add_gpu_test(backend_scatter_upload_gpu scatter_upload_gpu_test.cpp)
    render_backend_add_gpu_test(backend_target_pool_gpu target_pool_gpu_test.cpp)
endif()
