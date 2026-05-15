cmake_minimum_required(VERSION 3.20)
project(datalang LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ==================== Auto-download DuckDB prebuilt library ====================
include(FetchContent)

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

# ==================== datalang ====================
add_executable(dabble
    src/main.cpp
    src/parser.cpp
    src/interpreter.cpp
    src/analyzer.cpp
    src/utils.cpp
)

target_include_directories(dabble PRIVATE 
    ${duckdb_SOURCE_DIR}
    include
)

# Link the actual library file
target_link_libraries(dabble PRIVATE 
    ${duckdb_SOURCE_DIR}/libduckdb.so
)

set_target_properties(dabble PROPERTIES 
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

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
