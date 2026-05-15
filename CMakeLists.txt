cmake_minimum_required(VERSION 3.20)
project(datalang LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ==================== DuckDB — three ways to provide it ====================
#
# Option 1 (default): download prebuilt release binary
#   cmake -B build
#
# Option 2: use a local libduckdb (custom build with extensions baked in)
#   cmake -B build -DDUCKDB_LOCAL=/path/to/your/duckdb/build/release/src
#   The path must contain libduckdb.so and duckdb.h (or duckdb/ include dir).
#
# Option 3: build DuckDB from source at a specific commit (for extension dev)
#   cmake -B build -DDUCKDB_COMMIT=abc123def456
#   Builds DuckDB from that exact commit — extensions can then be built
#   against the same commit and loaded without signing issues.
#   Add -DDUCKDB_EXTENSIONS="ext1;ext2" to bundle extensions at build time.

include(FetchContent)

if(DEFINED DUCKDB_LOCAL)
    # ── Option 2: local prebuilt ──────────────────────────────────────────────
    message(STATUS "Using local DuckDB: ${DUCKDB_LOCAL}")

    # Strip trailing slash for reliable path joins
    string(REGEX REPLACE "/+$" "" DUCKDB_LOCAL "${DUCKDB_LOCAL}")

    # Find the .so directly — don't use find_library which can drop the path
    if(EXISTS "${DUCKDB_LOCAL}/libduckdb.so")
        set(DUCKDB_LIB "${DUCKDB_LOCAL}/libduckdb.so")
    else()
        message(FATAL_ERROR "Cannot find libduckdb.so in ${DUCKDB_LOCAL}")
    endif()

    # Find duckdb.h — check common locations, or override with -DDUCKDB_INCLUDE=...
    if(DEFINED DUCKDB_INCLUDE)
        # User-specified header path — use as-is
        message(STATUS "DuckDB headers (manual): ${DUCKDB_INCLUDE}")
    elseif(EXISTS "${DUCKDB_LOCAL}/duckdb.h")
        set(DUCKDB_INCLUDE "${DUCKDB_LOCAL}")
    elseif(EXISTS "${DUCKDB_LOCAL}/../include/duckdb.h")
        get_filename_component(DUCKDB_INCLUDE "${DUCKDB_LOCAL}/../include" ABSOLUTE)
    elseif(EXISTS "${DUCKDB_LOCAL}/../../src/include/duckdb.h")
        get_filename_component(DUCKDB_INCLUDE "${DUCKDB_LOCAL}/../../src/include" ABSOLUTE)
    elseif(EXISTS "${DUCKDB_LOCAL}/../../../tools/amalgamation/duckdb.h")
        get_filename_component(DUCKDB_INCLUDE "${DUCKDB_LOCAL}/../../../tools/amalgamation" ABSOLUTE)
    else()
        message(FATAL_ERROR
            "Cannot find duckdb.h near ${DUCKDB_LOCAL}\n"
            "Run: find /path/to/duckdb -name duckdb.h\n"
            "Then pass: -DDUCKDB_INCLUDE=/path/to/dir/containing/duckdb.h"
        )
    endif()

    message(STATUS "DuckDB library:  ${DUCKDB_LIB}")
    message(STATUS "DuckDB headers:  ${DUCKDB_INCLUDE}")

    add_library(duckdb_lib SHARED IMPORTED GLOBAL)
    set_target_properties(duckdb_lib PROPERTIES
        IMPORTED_LOCATION "${DUCKDB_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${DUCKDB_INCLUDE}"
    )

elseif(DEFINED DUCKDB_COMMIT)
    # ── Option 3: build from source at specific commit ────────────────────────
    message(STATUS "Building DuckDB from commit: ${DUCKDB_COMMIT}")

    set(DUCKDB_EXTENSIONS "" CACHE STRING "Semicolon-separated list of extensions to bundle")

    FetchContent_Declare(
        duckdb_src
        GIT_REPOSITORY https://github.com/duckdb/duckdb.git
        GIT_TAG        ${DUCKDB_COMMIT}
        GIT_SHALLOW    FALSE  # need exact commit, not shallow
    )

    # Build only the library — skip tests, benchmarks, tools
    set(BUILD_UNITTESTS    OFF CACHE BOOL "" FORCE)
    set(BUILD_BENCHMARKS   OFF CACHE BOOL "" FORCE)
    set(BUILD_SHELL        OFF CACHE BOOL "" FORCE)
    set(DUCKDB_BUILD_EXTENSIONS "${DUCKDB_EXTENSIONS}" CACHE STRING "" FORCE)

    FetchContent_MakeAvailable(duckdb_src)

    add_library(duckdb_lib ALIAS duckdb)

else()
    # ── Option 1: prebuilt release (default) ─────────────────────────────────
    message(STATUS "Downloading DuckDB prebuilt v1.5.2")

    if(APPLE)
        set(DUCKDB_URL "https://github.com/duckdb/duckdb/releases/download/v1.5.2/libduckdb-osx-universal.zip")
    elseif(UNIX)
        set(DUCKDB_URL "https://github.com/duckdb/duckdb/releases/download/v1.5.2/libduckdb-linux-amd64.zip")
    else()
        message(FATAL_ERROR "Unsupported platform")
    endif()

    FetchContent_Declare(
        duckdb
        URL ${DUCKDB_URL}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(duckdb)

    add_library(duckdb_lib SHARED IMPORTED)
    set_target_properties(duckdb_lib PROPERTIES
        IMPORTED_LOCATION ${duckdb_SOURCE_DIR}/libduckdb.so
        INTERFACE_INCLUDE_DIRECTORIES ${duckdb_SOURCE_DIR}
    )
endif()

# ==================== dabble ====================
add_executable(dabble
    src/main.cpp
    src/parser.cpp
    src/interpreter.cpp
    src/analyzer.cpp
    src/utils.cpp
)

target_include_directories(dabble PRIVATE include)
target_link_libraries(dabble PRIVATE duckdb_lib)

# For IMPORTED libraries, INTERFACE_INCLUDE_DIRECTORIES may not propagate
# on older CMake versions — add explicitly as fallback.
get_target_property(_duckdb_inc duckdb_lib INTERFACE_INCLUDE_DIRECTORIES)
if(_duckdb_inc)
    target_include_directories(dabble PRIVATE ${_duckdb_inc})
endif()

set_target_properties(dabble PROPERTIES 
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Embed rpath so the binary finds libduckdb.so at runtime without
# needing LD_LIBRARY_PATH. Works for both local and prebuilt modes.
if(DEFINED DUCKDB_LOCAL)
    set_target_properties(dabble PROPERTIES
        BUILD_RPATH  "${DUCKDB_LOCAL}"
        INSTALL_RPATH "${DUCKDB_LOCAL}"
    )
endif()

message(STATUS "DuckDB library: ${duckdb_SOURCE_DIR}/libduckdb.so")

# ==================== Tests ====================
enable_testing()

set(DABBLE_BIN ${CMAKE_BINARY_DIR}/bin/dabble)
set(TEST_DIR   ${CMAKE_SOURCE_DIR}/tests)

# Helper: each .dabble test file is a CTest test.
# Tests use "check ... else fail" internally — a failing check
# exits non-zero, which CTest treats as a test failure.
file(GLOB DABBLE_TESTS "${TEST_DIR}/test_*.dabble")
foreach(TEST_FILE ${DABBLE_TESTS})
    get_filename_component(TEST_NAME ${TEST_FILE} NAME_WE)
    add_test(
        NAME    ${TEST_NAME}
        COMMAND ${DABBLE_BIN} ${TEST_FILE}
    )
    set_tests_properties(${TEST_NAME} PROPERTIES
        TIMEOUT 30
    )
endforeach()