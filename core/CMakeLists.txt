cmake_minimum_required(VERSION 3.25)
project(zend_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(third_party/luau)

add_library(zend_core SHARED
    src/bootstrap/dllmain.cpp
    src/ipc/pipe.cpp
    src/ipc/session.cpp
    src/rbx/drawing.cpp
    src/rbx/env.cpp
    src/rbx/executor.cpp
    src/rbx/scheduler.cpp
    src/rbx/render.cpp
    src/util/log.cpp
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp
)

set_target_properties(zend_core PROPERTIES
    OUTPUT_NAME module
    PREFIX ""
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

target_include_directories(zend_core PRIVATE
    src
    vendor
    third_party/imgui
    third_party/imgui/backends
)

target_compile_definitions(zend_core PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
)

if(MSVC)
    target_compile_options(zend_core PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_link_options(zend_core PRIVATE /INCREMENTAL:NO)
endif()

target_link_libraries(zend_core PRIVATE zend_luau bcrypt advapi32 winhttp d3d11 dxgi d3dcompiler)

source_group("bootstrap" FILES src/bootstrap/dllmain.cpp)
source_group("ipc" FILES src/ipc/pipe.cpp src/ipc/session.cpp)
source_group("rbx" FILES src/rbx/drawing.cpp src/rbx/env.cpp src/rbx/executor.cpp src/rbx/render.cpp src/rbx/scheduler.cpp)
source_group("third_party" FILES
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp
)
source_group("util" FILES src/util/log.cpp)
