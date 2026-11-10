cmake_minimum_required(VERSION 3.20)
project(SCCTRemaster LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Splinter Cell Chaos Theory is a 32-bit game: configure with -A Win32")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: no VC++ redistributable needed on the player's machine.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde32.c
)
target_include_directories(minhook PUBLIC third_party/minhook/include)

# Lua 5.4, compiled as C++ so script errors unwind with C++ exceptions instead of longjmp.
file(GLOB LUA_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua/src/*.c")
add_library(lua STATIC ${LUA_SOURCES})
set_source_files_properties(${LUA_SOURCES} PROPERTIES LANGUAGE CXX)
target_include_directories(lua PUBLIC third_party/lua/src)
target_compile_options(lua PRIVATE /W0 /EHsc /TP)

# Dear ImGui (level editor overlay): core + DX9 and Win32 backends.
add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_dx9.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
)
target_include_directories(imgui PUBLIC third_party/imgui)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_DEMO_WINDOWS UNICODE _UNICODE)
target_compile_options(imgui PRIVATE /W0 /EHsc)

add_library(d3d9 SHARED
    src/dllmain.cpp
    src/log.cpp
    src/config.cpp
    src/memory.cpp
    src/d3d9_proxy.cpp
    src/d3d9_hooks.cpp
    src/d3d9_capture.cpp
    src/shaderpack.cpp
    src/framerate.cpp
    src/widescreen.cpp
    src/rawmouse.cpp
    src/misc.cpp
    src/window.cpp
    src/videos.cpp
    src/watchdog.cpp
    src/perf.cpp
    src/stuck.cpp
    src/movement.cpp
    src/script.cpp
    src/watch.cpp
    src/guards.cpp
    src/perception.cpp
    src/reaction.cpp
    src/noise.cpp
    src/alert.cpp
    src/aim.cpp
    src/scripted.cpp
    src/clusterfuck.cpp
    src/mods.cpp
    src/scripting.cpp
    src/textures.cpp
    src/editor.cpp
    src/victory.cpp
    src/plush.cpp
    src/exports.def
    src/version.rc
)
target_precompile_headers(d3d9 PRIVATE src/pch.h)
target_include_directories(d3d9 PRIVATE third_party/stb)
target_link_libraries(d3d9 PRIVATE minhook lua imgui winmm user32 ole32 shell32)
target_compile_definitions(d3d9 PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_compile_options(d3d9 PRIVATE /W3 /EHsc /permissive- /Zc:threadSafeInit- /MP)

target_link_options(d3d9 PRIVATE /DYNAMICBASE /NXCOMPAT)
set_target_properties(d3d9 PROPERTIES OUTPUT_NAME "d3d9")

# Developer tests (local only, not in the public repository): validate the AMD replacement shaders
# against the real D3D9 runtime/driver.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/shader_test.cpp")
    add_executable(shader_test tests/shader_test.cpp)
    target_link_libraries(shader_test PRIVATE user32)
    add_executable(render_test tests/render_test.cpp)
    target_link_libraries(render_test PRIVATE user32)
    add_executable(bink_close_test tests/bink_close_test.cpp)
    target_link_libraries(bink_close_test PRIVATE user32 dxguid)
endif()
