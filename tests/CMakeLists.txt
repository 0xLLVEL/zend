cmake_minimum_required(VERSION 3.25)
project(zend_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(load-run load-run.cpp)
add_library(fake-stock SHARED fake-stock.cpp)

set_target_properties(fake-stock PROPERTIES
    OUTPUT_NAME "RobloxPlayerBeta.stock"
    PREFIX ""
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

foreach(target load-run fake-stock)
    target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc /utf-8)
    endif()
endforeach()
