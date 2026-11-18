cmake_minimum_required(VERSION 3.25)
project(zend_loader LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Per-build manifest written by tools/offsetdump. `builds/ACTIVE` names the
# directory to use; the loader refuses to configure when config.h names a
# different build or when the payload size in the manifest disagrees with the
# constants compiled into the loader.
set(ZEND_BUILD_DIR "" CACHE PATH
    "Directory holding build.json and an optional payload.7z (empty uses builds/ACTIVE)")
if(NOT ZEND_BUILD_DIR)
    set(ZEND_ACTIVE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../builds/ACTIVE")
    if(EXISTS "${ZEND_ACTIVE_FILE}")
        file(STRINGS "${ZEND_ACTIVE_FILE}" ZEND_ACTIVE LIMIT_COUNT 1)
        string(STRIP "${ZEND_ACTIVE}" ZEND_ACTIVE)
        set(ZEND_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../builds/${ZEND_ACTIVE}")
    else()
        set(ZEND_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../builds/version-4ea8e85481e34f46")
    endif()
endif()
set(ZEND_MANIFEST "${ZEND_BUILD_DIR}/build.json")

# Fallback payload used when the build directory has no payload.7z. Kept
# outside the repo (reference material); `repack` writes the same shape here. A
# missing payload produces a fail-open only loader so CI can still build and
# link the proxy.
set(ZEND_PAYLOAD_BIN "${CMAKE_CURRENT_SOURCE_DIR}/../emulator/emulator/payload/RobloxPlayerBeta.bin"
    CACHE FILEPATH "Path to the extracted 0.747 client payload")
set(ZEND_7ZIP "${CMAKE_CURRENT_SOURCE_DIR}/../tools/7zip/7z.exe"
    CACHE FILEPATH "Path to 7z.exe used to pack the payload resource")

set(ZEND_CLIENT_VERSION "0.747.0.7471481")

if(EXISTS "${ZEND_MANIFEST}")
    file(READ "${ZEND_MANIFEST}" ZEND_MANIFEST_JSON)
    string(JSON ZEND_MANIFEST_BUILD ERROR_VARIABLE ZEND_JSON_ERROR
           GET "${ZEND_MANIFEST_JSON}" build)
    if(ZEND_JSON_ERROR)
        message(FATAL_ERROR "cannot read ${ZEND_MANIFEST}: ${ZEND_JSON_ERROR}")
    endif()
    string(JSON ZEND_MANIFEST_CLIENT GET "${ZEND_MANIFEST_JSON}" client)
    string(JSON ZEND_MANIFEST_PAYLOAD_SIZE GET "${ZEND_MANIFEST_JSON}" payload size)
    string(JSON ZEND_MANIFEST_PAYLOAD_SHA GET "${ZEND_MANIFEST_JSON}" payload sha256)

    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/include/config/config.h" ZEND_CONFIG_H)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/include/emulator/offsets.h" ZEND_OFFSETS_H)

    if(NOT ZEND_CONFIG_H MATCHES "kBuildId\\[\\] = \"([0-9A-Fa-f]+)\"")
        message(FATAL_ERROR "config.h has no kBuildId")
    endif()
    if(NOT CMAKE_MATCH_1 STREQUAL ZEND_MANIFEST_BUILD)
        message(FATAL_ERROR
            "config.h builds ${CMAKE_MATCH_1}, manifest is ${ZEND_MANIFEST_BUILD}")
    endif()
    if(NOT ZEND_CONFIG_H MATCHES "kTextSize = 0x([0-9A-Fa-f]+)u")
        message(FATAL_ERROR "config.h has no kTextSize")
    endif()
    set(ZEND_TEXT_SIZE "0x${CMAKE_MATCH_1}")
    if(NOT ZEND_OFFSETS_H MATCHES "kRtCount = 0x([0-9A-Fa-f]+)u")
        message(FATAL_ERROR "offsets.h has no kRtCount")
    endif()
    set(ZEND_RT_COUNT "0x${CMAKE_MATCH_1}")

    math(EXPR ZEND_EXPECTED_PAYLOAD
         "${ZEND_TEXT_SIZE} + 8 + ${ZEND_RT_COUNT} * 12")
    if(NOT ZEND_EXPECTED_PAYLOAD EQUAL ZEND_MANIFEST_PAYLOAD_SIZE)
        message(FATAL_ERROR
            "payload size ${ZEND_MANIFEST_PAYLOAD_SIZE} in ${ZEND_MANIFEST}, "
            "but .text + count + .pdata is ${ZEND_EXPECTED_PAYLOAD}; "
            "regenerate config.h/offsets.h from the same build")
    endif()
    if(ZEND_MANIFEST_CLIENT MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+$")
        set(ZEND_CLIENT_VERSION "${ZEND_MANIFEST_CLIENT}")
    endif()
    message(STATUS "build ${ZEND_MANIFEST_BUILD} (${ZEND_CLIENT_VERSION}) "
                   "payload ${ZEND_MANIFEST_PAYLOAD_SIZE} bytes "
                   "${ZEND_MANIFEST_PAYLOAD_SHA}")
else()
    message(WARNING "no build manifest at ${ZEND_MANIFEST}; "
                    "building with the constants in config.h/offsets.h")
endif()

set(LZMA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lzma")
set(LZMA_SOURCES
    ${LZMA_DIR}/mem7z.c
    ${LZMA_DIR}/C/7zAlloc.c
    ${LZMA_DIR}/C/7zArcIn.c
    ${LZMA_DIR}/C/7zBuf.c
    ${LZMA_DIR}/C/7zCrc.c
    ${LZMA_DIR}/C/7zCrcOpt.c
    ${LZMA_DIR}/C/7zDec.c
    ${LZMA_DIR}/C/7zStream.c
    ${LZMA_DIR}/C/Bcj2.c
    ${LZMA_DIR}/C/Bra.c
    ${LZMA_DIR}/C/Bra86.c
    ${LZMA_DIR}/C/CpuArch.c
    ${LZMA_DIR}/C/Delta.c
    ${LZMA_DIR}/C/Lzma2Dec.c
    ${LZMA_DIR}/C/LzmaDec.c
)

add_library(zend_loader SHARED
    src/detlog.cpp
    src/dllmain.cpp
    src/hooks.cpp
    src/hwid.cpp
    src/image.cpp
    src/init.cpp
    src/probes.cpp
    src/services.cpp
    src/state.cpp
    src/util.cpp
    ${LZMA_SOURCES}
)

set_target_properties(zend_loader PROPERTIES
    OUTPUT_NAME "RobloxPlayerBeta"
    PREFIX ""
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

target_include_directories(zend_loader PRIVATE
    include
    res
    ${LZMA_DIR}
    ${LZMA_DIR}/C
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_compile_definitions(zend_loader PRIVATE
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
    Z7_EXTRACT_ONLY
)

# QA builds publish the DET1 ring and detlog.jsonl. Off for shipped builds:
# the section and the log are observable from other processes.
option(ZEND_DETLOG "Publish detlog shared memory and detlog.jsonl" OFF)
if(ZEND_DETLOG)
    target_compile_definitions(zend_loader PRIVATE ZEND_DETLOG=1)
    message(STATUS "detlog: ON (QA build; DET1 section is visible)")
else()
    target_compile_definitions(zend_loader PRIVATE ZEND_DETLOG=0)
endif()

# Payload resource. Prefer the archive generated by tools/repack for this build;
# otherwise pack the fallback payload with the pinned 7-Zip. Both paths embed a
# 7z archive as IDR_PAYLOAD (101); Mem7zExtract picks the entry whose
# uncompressed size equals kPayloadSize.
set(ZEND_PACKED_7Z "${CMAKE_CURRENT_BINARY_DIR}/payload/RobloxPlayerBeta.7z")
if(EXISTS "${ZEND_BUILD_DIR}/payload.7z")
    set(ZEND_HAS_PAYLOAD 1)
    set(ZEND_PAYLOAD_7Z "${ZEND_BUILD_DIR}/payload.7z")
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/loader.rc"
        PROPERTIES OBJECT_DEPENDS "${ZEND_PAYLOAD_7Z}")
    message(STATUS "payload from ${ZEND_PAYLOAD_7Z}")
elseif(EXISTS "${ZEND_PAYLOAD_BIN}")
    set(ZEND_HAS_PAYLOAD 1)
    set(ZEND_PAYLOAD_7Z "${ZEND_PACKED_7Z}")

    add_custom_command(
        OUTPUT "${ZEND_PAYLOAD_7Z}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/payload"
        COMMAND "${CMAKE_COMMAND}" -E rm -f "${ZEND_PAYLOAD_7Z}"
        COMMAND "${ZEND_7ZIP}" a -t7z -m0=BCJ2 "-m1=LZMA2:d256m:fb273"
                "-m2=LZMA2:d64k" "-m3=LZMA2:d64k" -mb0:1 -mb0s1:2 -mb0s2:3
                -mx=9 -mmt=on "${ZEND_PAYLOAD_7Z}" "${ZEND_PAYLOAD_BIN}"
        DEPENDS "${ZEND_PAYLOAD_BIN}" "${ZEND_7ZIP}"
        COMMENT "Packing payload into payload/RobloxPlayerBeta.7z (slow, cached)"
        VERBATIM
    )
    add_custom_target(zend_payload DEPENDS "${ZEND_PAYLOAD_7Z}")
    add_dependencies(zend_loader zend_payload)
else()
    set(ZEND_HAS_PAYLOAD 0)
    set(ZEND_PAYLOAD_7Z "")
    message(STATUS
        "note: payload not found at ${ZEND_PAYLOAD_BIN}; building a fail-open only loader. "
        "Run tools/repack into ${ZEND_BUILD_DIR} or set -DZEND_PAYLOAD_BIN=<path>.")
endif()

configure_file(res/loader.rc.in "${CMAKE_CURRENT_BINARY_DIR}/loader.rc" @ONLY)

target_compile_definitions(zend_loader PRIVATE ZEND_HAS_PAYLOAD=${ZEND_HAS_PAYLOAD})

target_sources(zend_loader PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/loader.rc"
    res/loader.def
)

# The vendored 7z decoder is C89 and noisy at /W4.
if(MSVC)
    set_source_files_properties(${LZMA_SOURCES} PROPERTIES COMPILE_OPTIONS "/W0")
    target_compile_options(zend_loader PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_link_options(zend_loader PRIVATE /INCREMENTAL:NO)
endif()

target_link_libraries(zend_loader PRIVATE bcrypt iphlpapi ws2_32 WindowsApp)
