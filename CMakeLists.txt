cmake_minimum_required(VERSION 3.20)
project(glft_wf_backtest CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Release by default
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG   "-g -O0 -fsanitize=address,undefined")

# Brew installs these under /opt/homebrew/opt/<name>; tell CMake where to look
list(APPEND CMAKE_PREFIX_PATH
    /opt/homebrew/opt/nlopt
    /opt/homebrew/opt/duckdb
    /opt/homebrew/opt/fmt
    /opt/homebrew
)

find_package(NLopt  CONFIG REQUIRED)   # nlopt cmake module exports NLopt::nlopt
find_package(duckdb CONFIG REQUIRED)
find_package(fmt    CONFIG REQUIRED)

# ── Main executable ───────────────────────────────────────────────────────────
set(SOURCES
    src/data_reader.cpp
    src/intensity.cpp
    src/fill_engine.cpp
    src/walk_forward.cpp
    src/results_writer.cpp
    src/main.cpp
)

add_executable(wf_backtest ${SOURCES})

# DuckDBConfig.cmake sets DuckDB_INCLUDE_DIRS but does not attach it to a
# target's interface, so add it explicitly (brew ships the header under
# /opt/homebrew/include via this var).
target_include_directories(wf_backtest PRIVATE src ${DuckDB_INCLUDE_DIRS})

target_link_libraries(wf_backtest PRIVATE
    duckdb
    NLopt::nlopt
    fmt::fmt
)

# ── Unit test executable ──────────────────────────────────────────────────────
set(TEST_SOURCES
    src/data_reader.cpp
    src/intensity.cpp
    src/fill_engine.cpp
    src/walk_forward.cpp
    src/results_writer.cpp
    tests/test_main.cpp
)

add_executable(run_tests ${TEST_SOURCES})

target_include_directories(run_tests PRIVATE src ${DuckDB_INCLUDE_DIRS})

target_link_libraries(run_tests PRIVATE
    duckdb
    NLopt::nlopt
    fmt::fmt
)

enable_testing()
add_test(NAME unit_tests COMMAND run_tests)
