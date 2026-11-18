# Luau VM + compiler, built into module.dll so the core can compile scripts
# with the same bytecode layout the client expects.

file(GLOB_RECURSE ZEND_LUAU_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/Ast/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Bytecode/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Common/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Compiler/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/VM/src/*.cpp
)

add_library(zend_luau STATIC ${ZEND_LUAU_SOURCES})

target_include_directories(zend_luau PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Ast/src
    ${CMAKE_CURRENT_SOURCE_DIR}/Bytecode/include
    ${CMAKE_CURRENT_SOURCE_DIR}/Common/include
    ${CMAKE_CURRENT_SOURCE_DIR}/Compiler/include
    ${CMAKE_CURRENT_SOURCE_DIR}/VM/include
    ${CMAKE_CURRENT_SOURCE_DIR}/VM/src
)

set_target_properties(zend_luau PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

if(MSVC)
    # Third-party sources are not /W4 clean; keep their noise out of our build.
    # /EHsc is required: the client's LuaD_Throw raises C++ exceptions that the
    # VM's protected call has to catch.
    target_compile_options(zend_luau PRIVATE /W0 /utf-8 /permissive- /EHsc)
endif()
