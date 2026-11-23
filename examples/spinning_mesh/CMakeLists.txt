add_executable(spinning_mesh WIN32 main.cpp spinning_mesh.manifest)
render_backend_configure_target(spinning_mesh)
target_compile_definitions(spinning_mesh PRIVATE UNICODE _UNICODE)
target_link_libraries(spinning_mesh PRIVATE backend_render backend_shader user32)

# With RENDER_BACKEND_AGILITY_SDK_ROOT or RENDER_BACKEND_FETCH_AGILITY set, this copies the Agility runtime next to
# the executable and defines RENDER_BACKEND_AGILITY_SDK_VERSION. Otherwise the example uses the D3D12 runtime
# installed with Windows.
render_backend_copy_agility_runtime(spinning_mesh)

# The effect source is compiled at run time, so it is deployed next to the executable.
add_custom_command(TARGET spinning_mesh POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders" "$<TARGET_FILE_DIR:spinning_mesh>/shaders"
    VERBATIM)
