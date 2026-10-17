cmake_minimum_required(VERSION 3.28)

project(matchbook-cpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Warnings apply to every target defined below; vcpkg's imported targets are
# out of reach of directory properties, so third-party headers stay clean.
add_compile_options(-Wall -Wextra -Wpedantic -Werror)

find_package(GTest CONFIG REQUIRED)
find_package(benchmark CONFIG REQUIRED)
find_package(hdr_histogram CONFIG REQUIRED)
find_package(xxHash CONFIG REQUIRED)

# Fast engine. The implementation lands in Epic 1; the target, includes and
# dependency wiring are frozen here.
file(GLOB LOB_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
add_library(lob STATIC ${LOB_SOURCES})
target_include_directories(lob PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(lob PUBLIC xxHash::xxhash)

# Naive reference engine (test-only). It shares no code with `lob` beyond the
# frozen headers, so it gets the include paths directly and does not link the
# fast engine.
file(GLOB LOB_REFERENCE_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/reference/*.cpp")
add_library(lob_reference STATIC ${LOB_REFERENCE_SOURCES})
target_include_directories(lob_reference PUBLIC
                           "${CMAKE_CURRENT_SOURCE_DIR}"
                           "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(lob_reference PUBLIC xxHash::xxhash)

# Flow file records/reader/writer (WS C).
file(GLOB LOB_FLOWFILE_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/tools/flowfile/*.cpp")
add_library(lob_flowfile STATIC ${LOB_FLOWFILE_SOURCES})
target_include_directories(lob_flowfile PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(lob_flowfile PUBLIC lob)

# Seeded operation generator (WS B).
file(GLOB LOB_OPGEN_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/tools/opgen/*.cpp")
add_library(lob_opgen STATIC ${LOB_OPGEN_SOURCES})
target_include_directories(lob_opgen PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(lob_opgen PUBLIC lob)

# CLI: gen, replay, difftest, convert-tardis.
file(GLOB LOBSIM_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/tools/lobsim/*.cpp")
add_executable(lobsim ${LOBSIM_SOURCES})
target_include_directories(lobsim PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(lobsim PRIVATE lob lob_reference lob_flowfile lob_opgen)

# Benchmarks.
file(GLOB LOB_BENCH_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/bench/*.cpp")
add_executable(lob_bench ${LOB_BENCH_SOURCES})
target_link_libraries(lob_bench PRIVATE lob benchmark::benchmark
                                       hdr_histogram::hdr_histogram_static)

# Tests. test_alloc replaces global operator new/delete, so it is its own
# binary and stays out of lob_tests.
file(GLOB LOB_TEST_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.cpp")
list(REMOVE_ITEM LOB_TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_alloc.cpp")
add_executable(lob_tests ${LOB_TEST_SOURCES})
target_include_directories(lob_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
target_link_libraries(lob_tests PRIVATE lob lob_reference lob_flowfile lob_opgen
                                        GTest::gtest_main)

add_executable(test_alloc "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_alloc.cpp")
target_link_libraries(test_alloc PRIVATE lob GTest::gtest_main)

enable_testing()
include(GoogleTest)
gtest_discover_tests(lob_tests DISCOVERY_MODE PRE_TEST)
gtest_discover_tests(test_alloc DISCOVERY_MODE PRE_TEST)
