add_library(backend_shader STATIC
    auto_constants.cpp
    effect_desc.cpp
    front_end.cpp
    shader_cache.cpp
    watcher.cpp
)
render_backend_configure_target(backend_shader)
target_link_libraries(backend_shader PUBLIC render_backend::eastl PRIVATE ole32)
add_library(render_backend::shader ALIAS backend_shader)
