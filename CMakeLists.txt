cmake_minimum_required(VERSION 3.20)
project(dabble LANGUAGES CXX)

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

# ==================== dabble ====================
add_executable(dabble
    src/main.cpp
    src/parser.cpp
    src/interpreter.cpp
    src/utils.cpp
)

target_include_directories(dabble PRIVATE
    ${duckdb_SOURCE_DIR}
    include
)

# Use platform-appropriate extension (.so on Linux, .dylib on macOS)
set(DUCKDB_LIB "${duckdb_SOURCE_DIR}/libduckdb${CMAKE_SHARED_LIBRARY_SUFFIX}")
target_link_libraries(dabble PRIVATE ${DUCKDB_LIB})

set_target_properties(dabble PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

message(STATUS "DuckDB library: ${DUCKDB_LIB}")
