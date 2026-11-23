include_guard(GLOBAL)
include(FetchContent)
include("${CMAKE_CURRENT_LIST_DIR}/DependencyVersions.cmake")

# Fetch sources without running the dependency's own CMakeLists.txt. The functions below define the targets.
function(render_backend_fetch_git name repository revision)
    FetchContent_Declare(${name}
        GIT_REPOSITORY "${repository}"
        GIT_TAG "${revision}"
        GIT_SUBMODULES ""
        GIT_CONFIG core.autocrlf=false
        SOURCE_SUBDIR _render_backend_source_only)
    FetchContent_MakeAvailable(${name})
    set(${name}_SOURCE_DIR "${${name}_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

function(render_backend_shader_dependencies)
    if(TARGET render_backend::eastl)
        return()
    endif()
    render_backend_fetch_git(render_backend_eabase
        https://github.com/electronicarts/EABase.git "${RENDER_BACKEND_PIN_EABASE}")
    render_backend_fetch_git(render_backend_eastl
        https://github.com/electronicarts/EASTL.git "${RENDER_BACKEND_PIN_EASTL}")
    set(eastl_src "${render_backend_eastl_SOURCE_DIR}/source")
    add_library(render_backend_eastl STATIC
        "${eastl_src}/allocator_eastl.cpp"
        "${eastl_src}/assert.cpp"
        "${eastl_src}/atomic.cpp"
        "${eastl_src}/fixed_pool.cpp"
        "${eastl_src}/hashtable.cpp"
        "${eastl_src}/intrusive_list.cpp"
        "${eastl_src}/numeric_limits.cpp"
        "${eastl_src}/red_black_tree.cpp"
        "${eastl_src}/string.cpp"
        "${eastl_src}/thread_support.cpp")
    add_library(render_backend::eastl ALIAS render_backend_eastl)
    target_include_directories(render_backend_eastl SYSTEM PUBLIC
        "${render_backend_eastl_SOURCE_DIR}/include" "${render_backend_eabase_SOURCE_DIR}/include/Common")
    target_compile_features(render_backend_eastl PUBLIC cxx_std_17)
    target_compile_definitions(render_backend_eastl PUBLIC
        "${RENDER_BACKEND_MSVC_STL_DEFINITION}"
        EASTL_EXCEPTIONS_ENABLED=0 EASTL_ASSERT_ENABLED=0 EASTL_CPP11_MUTEX_ENABLED=0)
    target_compile_options(render_backend_eastl PRIVATE /GR- /Zc:threadSafeInit- /fp:precise)
    set(RENDER_BACKEND_EASTL_INCLUDE "${render_backend_eastl_SOURCE_DIR}/include" CACHE INTERNAL "EASTL include path")
    set(RENDER_BACKEND_EABASE_INCLUDE "${render_backend_eabase_SOURCE_DIR}/include/Common" CACHE INTERNAL "EABase include path")
endfunction()

# Copy the NVRHI checkout and apply patches/nvrhi/*.patch to the copy, leaving the checkout untouched.
# The staged directory name hashes the pin, source path and patch contents, so any change produces a new tree.
function(render_backend_stage_nvrhi source output)
    find_package(Git REQUIRED)
    if(EXISTS "${source}/.git")
        execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD WORKING_DIRECTORY "${source}"
            RESULT_VARIABLE revision_status OUTPUT_VARIABLE revision OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT revision_status EQUAL 0 OR NOT revision STREQUAL RENDER_BACKEND_PIN_NVRHI)
            message(FATAL_ERROR "NVRHI source ${source} must be at ${RENDER_BACKEND_PIN_NVRHI}, found '${revision}'.")
        endif()
    endif()

    file(GLOB patches CONFIGURE_DEPENDS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../patches/nvrhi/*.patch")
    list(SORT patches)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${patches})
    set(key "${RENDER_BACKEND_PIN_NVRHI};${source}")
    foreach(patch IN LISTS patches)
        file(SHA256 "${patch}" hash)
        string(APPEND key ";${hash}")
    endforeach()
    string(SHA256 digest "${key}")
    set(staged "${CMAKE_BINARY_DIR}/_deps/render_backend_nvrhi-patched-${digest}")
    if(NOT EXISTS "${staged}/.patches-applied")
        set(apply_patch "${GIT_EXECUTABLE}" -c apply.ignoreWhitespace=no apply --no-index --whitespace=nowarn)
        string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef nonce)
        set(temp "${staged}-${nonce}")
        file(COPY "${source}/" DESTINATION "${temp}" PATTERN ".git" EXCLUDE)
        foreach(patch IN LISTS patches)
            get_filename_component(patch_name "${patch}" NAME)
            execute_process(COMMAND ${apply_patch} --check "${patch}"
                WORKING_DIRECTORY "${temp}" RESULT_VARIABLE check_status ERROR_VARIABLE check_error)
            if(check_status EQUAL 0)
                execute_process(COMMAND ${apply_patch} "${patch}"
                    WORKING_DIRECTORY "${temp}" COMMAND_ERROR_IS_FATAL ANY)
                message(STATUS "NVRHI: applied ${patch_name}")
            else()
                execute_process(COMMAND ${apply_patch} --reverse --check "${patch}"
                    WORKING_DIRECTORY "${temp}" RESULT_VARIABLE reverse_status ERROR_VARIABLE reverse_error)
                if(NOT reverse_status EQUAL 0)
                    message(FATAL_ERROR
                        "NVRHI ${RENDER_BACKEND_PIN_NVRHI}: ${patch_name} does not match the pinned sources. "
                        "Refresh the patch, or remove it after confirming upstream includes the fix.\n"
                        "Apply check:\n${check_error}\nReverse check:\n${reverse_error}")
                endif()
                message(STATUS "NVRHI: ${patch_name} already present")
            endif()
        endforeach()
        file(WRITE "${temp}/.patches-applied" "${key}\n")
        file(RENAME "${temp}" "${staged}")
    endif()
    set(${output} "${staged}" PARENT_SCOPE)
endfunction()

function(render_backend_gpu_dependencies)
    if(TARGET render_backend::nvrhi)
        return()
    endif()
    render_backend_shader_dependencies()
    render_backend_fetch_git(render_backend_directx_headers
        https://github.com/microsoft/DirectX-Headers.git "${RENDER_BACKEND_PIN_DIRECTX_HEADERS}")
    # NVRHI links whichever of these target names exists. Reuse a parent project's targets when it defines them.
    if(NOT TARGET DirectX-Headers AND NOT TARGET Microsoft::DirectX-Headers)
        add_library(DirectX-Headers INTERFACE)
        target_include_directories(DirectX-Headers SYSTEM INTERFACE "${render_backend_directx_headers_SOURCE_DIR}/include")
        add_library(Microsoft::DirectX-Headers ALIAS DirectX-Headers)
    endif()
    if(NOT TARGET DirectX-Guids AND NOT TARGET Microsoft::DirectX-Guids)
        add_library(DirectX-Guids STATIC "${render_backend_directx_headers_SOURCE_DIR}/src/dxguids.cpp")
        target_include_directories(DirectX-Guids PRIVATE "${render_backend_directx_headers_SOURCE_DIR}/include")
        target_compile_features(DirectX-Guids PRIVATE cxx_std_17)
        add_library(Microsoft::DirectX-Guids ALIAS DirectX-Guids)
    endif()

    render_backend_fetch_git(render_backend_nvrhi
        https://github.com/NVIDIA-RTX/NVRHI.git "${RENDER_BACKEND_PIN_NVRHI}")
    render_backend_stage_nvrhi("${render_backend_nvrhi_SOURCE_DIR}" nvrhi_source)
    set(NVRHI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(NVRHI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(NVRHI_INSTALL OFF CACHE BOOL "" FORCE)
    set(NVRHI_WITH_VALIDATION ON CACHE BOOL "" FORCE)
    set(NVRHI_WITH_VULKAN OFF CACHE BOOL "" FORCE)
    set(NVRHI_WITH_DX11 OFF CACHE BOOL "" FORCE)
    set(NVRHI_WITH_DX12 ON CACHE BOOL "" FORCE)
    set(NVRHI_WITH_NVAPI OFF CACHE BOOL "" FORCE)
    set(NVRHI_WITH_AFTERMATH OFF CACHE BOOL "" FORCE)
    set(NVRHI_WITH_RTXMU OFF CACHE BOOL "" FORCE)
    set(NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP OFF CACHE BOOL "" FORCE)
    set(NVRHI_D3D12_WITH_DXRCLUSTER OFF CACHE BOOL "" FORCE)
    set(NVRHI_FETCH_DIRECTX_HEADERS OFF CACHE BOOL "" FORCE)
    if(RENDER_BACKEND_CHECKS)
        # Enable NVRHI's asserts. Removing NDEBUG here affects only this function's scope and the NVRHI
        # subdirectory, and avoids passing both /DNDEBUG and /UNDEBUG.
        foreach(flags IN ITEMS CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_RELEASE CMAKE_CXX_FLAGS_RELWITHDEBINFO
            CMAKE_CXX_FLAGS_MINSIZEREL)
            string(REGEX REPLACE "(^| )[/-]D *NDEBUG( |$)" " " ${flags} "${${flags}}")
        endforeach()
    endif()
    add_subdirectory("${nvrhi_source}" "${CMAKE_BINARY_DIR}/_deps/render_backend_nvrhi-build")
    foreach(target IN ITEMS nvrhi nvrhi_d3d12)
        target_compile_definitions(${target} PUBLIC "${RENDER_BACKEND_MSVC_STL_DEFINITION}")
        target_compile_options(${target} PRIVATE /GR /fp:precise)
        if(RENDER_BACKEND_CHECKS)
            target_compile_options(${target} PRIVATE /UNDEBUG)
        endif()
    endforeach()
    add_library(render_backend_nvrhi INTERFACE)
    target_link_libraries(render_backend_nvrhi INTERFACE nvrhi_d3d12 nvrhi dxgi dxguid uuid)
    add_library(render_backend::nvrhi ALIAS render_backend_nvrhi)
    add_library(render_backend::nvrhi_base ALIAS nvrhi)
    add_library(render_backend::nvrhi_d3d12 ALIAS nvrhi_d3d12)
    set(RENDER_BACKEND_NVRHI_INCLUDE "${nvrhi_source}/include" CACHE INTERNAL "Patched NVRHI include path")
    set(RENDER_BACKEND_DIRECTX_INCLUDE "${render_backend_directx_headers_SOURCE_DIR}/include"
        CACHE INTERNAL "Pinned DirectX-Headers include path")
    set(RENDER_BACKEND_NVRHI_INCLUDE "${RENDER_BACKEND_NVRHI_INCLUDE}" PARENT_SCOPE)
    set(RENDER_BACKEND_DIRECTX_INCLUDE "${RENDER_BACKEND_DIRECTX_INCLUDE}" PARENT_SCOPE)

    render_backend_fetch_git(render_backend_stb
        https://github.com/nothings/stb.git "${RENDER_BACKEND_PIN_STB}")
    add_library(render_backend_stb INTERFACE)
    target_include_directories(render_backend_stb SYSTEM INTERFACE "${render_backend_stb_SOURCE_DIR}")
    add_library(render_backend::stb ALIAS render_backend_stb)
endfunction()

set(RENDER_BACKEND_AGILITY_SDK_ROOT "" CACHE PATH "Root of a restored Microsoft.Direct3D.D3D12 ${RENDER_BACKEND_PIN_AGILITY_VERSION} package")
option(RENDER_BACKEND_FETCH_AGILITY "Fetch the pinned Agility SDK binaries into the build tree" OFF)

# Deploy the Agility SDK runtime next to an executable target: D3D12Core.dll (and, with RENDER_BACKEND_CHECKS,
# d3d12SDKLayers.dll) is copied to <exe dir>/D3D12/. The target also gets the compile definition
# RENDER_BACKEND_AGILITY_SDK_VERSION, which it must use to export D3D12SDKVersion and D3D12SDKPath; without
# those exports Windows ignores the copied runtime. Without a package the target is left unchanged.
function(render_backend_copy_agility_runtime target)
    set(root "${RENDER_BACKEND_AGILITY_SDK_ROOT}")
    if(NOT root AND RENDER_BACKEND_FETCH_AGILITY)
        set(version "${RENDER_BACKEND_PIN_AGILITY_VERSION}")
        FetchContent_Declare(render_backend_agility
            URL "https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/${version}/microsoft.direct3d.d3d12.${version}.nupkg"
            URL_HASH "SHA256=${RENDER_BACKEND_PIN_AGILITY_SHA256}"
            DOWNLOAD_NAME "agility-${version}.zip"
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
            SOURCE_SUBDIR _render_backend_source_only)
        FetchContent_MakeAvailable(render_backend_agility)
        set(root "${render_backend_agility_SOURCE_DIR}")
    endif()
    if(NOT root)
        message(STATUS "${target}: set RENDER_BACKEND_AGILITY_SDK_ROOT or RENDER_BACKEND_FETCH_AGILITY to deploy the Agility runtime.")
        return()
    endif()
    set(runtime "${root}/build/native/bin/x64/D3D12Core.dll")
    if(NOT EXISTS "${runtime}")
        message(FATAL_ERROR "Agility SDK ${RENDER_BACKEND_PIN_AGILITY_VERSION} runtime not found: ${runtime}")
    endif()
    # The exported D3D12SDKVersion must equal the runtime's SDK version. Read it from the package header; a
    # package version 1.N.x has SDK version N.
    set(header "${root}/build/native/include/d3d12.h")
    set(sdk_version "")
    if(EXISTS "${header}")
        file(STRINGS "${header}" sdk_line REGEX "#define[ \t]+D3D12_SDK_VERSION[ \t]" LIMIT_COUNT 1)
        string(REGEX MATCH "\\([ \t]*([0-9]+)[ \t]*\\)" sdk_match "${sdk_line}")
        set(sdk_version "${CMAKE_MATCH_1}")
    endif()
    if(NOT sdk_version)
        string(REGEX REPLACE "^1\\.([0-9]+)\\..*$" "\\1" sdk_version "${RENDER_BACKEND_PIN_AGILITY_VERSION}")
        message(STATUS "${target}: ${header} not found; assuming the pinned Agility SDK version ${sdk_version}.")
    endif()
    target_compile_definitions(${target} PRIVATE RENDER_BACKEND_AGILITY_SDK_VERSION=${sdk_version})
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/D3D12"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${runtime}" "$<TARGET_FILE_DIR:${target}>/D3D12/"
        VERBATIM)
    set(layers "${root}/build/native/bin/x64/d3d12SDKLayers.dll")
    if(RENDER_BACKEND_CHECKS AND EXISTS "${layers}")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${layers}" "$<TARGET_FILE_DIR:${target}>/D3D12/"
            VERBATIM)
    endif()
endfunction()
