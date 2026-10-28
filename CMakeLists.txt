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
find_package(ZLIB REQUIRED)

# Fast engine: the matching engine and its private data structures. Every
# .cpp under src/ is picked up automatically, so new engine sources never
# require an edit here.
file(GLOB LOB_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
add_library(lob STATIC ${LOB_SOURCES})
target_include_directories(lob PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(lob PUBLIC xxHash::xxhash)

# Price ladder implemented as a std::map tree (Epic 1 baseline) or as an
# array + occupancy bitmap (Epic 2), selected at build time. The definition is
# PUBLIC because OrderBook's layout and the ladder type depend on it, so every
# consumer has to compile against the same choice.
set(LOB_PRICE_LADDER "array" CACHE STRING "Price ladder implementation: tree or array")
set_property(CACHE LOB_PRICE_LADDER PROPERTY STRINGS tree array)
if(LOB_PRICE_LADDER STREQUAL "tree")
  target_compile_definitions(lob PUBLIC LOB_PRICE_LADDER_TREE=1)
elseif(LOB_PRICE_LADDER STREQUAL "array")
  target_compile_definitions(lob PUBLIC LOB_PRICE_LADDER_ARRAY=1)
else()
  message(FATAL_ERROR "LOB_PRICE_LADDER must be 'tree' or 'array', got '${LOB_PRICE_LADDER}'")
endif()
message(STATUS "lob: price ladder = ${LOB_PRICE_LADDER}")

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

# Flow file records/reader/writer (WS C). It uses the frozen lob headers but
# deliberately does not link `lob`: every executable that uses it links the
# engine directly, which keeps the linker line free of duplicates.
file(GLOB LOB_FLOWFILE_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/tools/flowfile/*.cpp")
add_library(lob_flowfile STATIC ${LOB_FLOWFILE_SOURCES})
target_include_directories(lob_flowfile PUBLIC
                           "${CMAKE_CURRENT_SOURCE_DIR}"
                           "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(lob_flowfile PUBLIC ZLIB::ZLIB)

# Seeded operation generator (WS B).
file(GLOB LOB_OPGEN_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/tools/opgen/*.cpp")
add_library(lob_opgen STATIC ${LOB_OPGEN_SOURCES})
target_include_directories(lob_opgen PUBLIC
                           "${CMAKE_CURRENT_SOURCE_DIR}"
                           "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Wire protocol for Phase 3 (decision C50): message definitions, the
# byte-by-byte codec and the partial-read stream framer. It depends on the
# frozen lob types only, so it does not link the engine.
file(GLOB LOB_NET_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/net/**/*.cpp")
add_library(lob_net STATIC ${LOB_NET_SOURCES})
target_include_directories(lob_net PUBLIC
                           "${CMAKE_CURRENT_SOURCE_DIR}"
                           "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Phase 3 executables (gateway, publisher, load generator) each live in
# apps/<name>/ with its own CMakeLists.txt linking lob and lob_net; this hook
# adds every such directory, so a new app never edits this file.
file(GLOB LOB_APP_DIRS CONFIGURE_DEPENDS LIST_DIRECTORIES true
     "${CMAKE_CURRENT_SOURCE_DIR}/apps/*")
set(LOB_APP_SUBDIRECTORIES "")
foreach(lob_app_dir IN LISTS LOB_APP_DIRS)
  if(IS_DIRECTORY "${lob_app_dir}" AND EXISTS "${lob_app_dir}/CMakeLists.txt")
    get_filename_component(lob_app_name "${lob_app_dir}" NAME)
    list(APPEND LOB_APP_SUBDIRECTORIES "${lob_app_name}")
  endif()
endforeach()
# file(GLOB) already sorts; sorting again keeps the order stable if the glob
# ever stops doing so. Nothing is added while apps/ is absent or empty.
list(SORT LOB_APP_SUBDIRECTORIES)
foreach(lob_app_name IN LISTS LOB_APP_SUBDIRECTORIES)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/apps/${lob_app_name}")
endforeach()
set(LOB_APP_SUBDIRECTORIES
    "${LOB_APP_SUBDIRECTORIES}"
    CACHE INTERNAL "apps/<name> directories added by the Phase 3 hook")

# CLI: gen, replay, difftest, convert-tardis. The subcommand implementations
# live in lobsim_core so tests can call them directly; the executable is only
# main.cpp plus the dispatch table.
file(GLOB LOBSIM_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/tools/lobsim/*.cpp")
list(REMOVE_ITEM LOBSIM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/tools/lobsim/main.cpp")
add_library(lobsim_core STATIC ${LOBSIM_SOURCES})
target_include_directories(lobsim_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(lobsim_core PUBLIC lob lob_reference lob_flowfile lob_opgen)

add_executable(lobsim "${CMAKE_CURRENT_SOURCE_DIR}/tools/lobsim/main.cpp")
target_link_libraries(lobsim PRIVATE lobsim_core)

# Benchmarks.
file(GLOB LOB_BENCH_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/bench/*.cpp")
add_executable(lob_bench ${LOB_BENCH_SOURCES})
target_include_directories(lob_bench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
# The JSON context records the compiler flags this binary was built with.
string(TOUPPER "${CMAKE_BUILD_TYPE}" LOB_BENCH_BUILD_TYPE)
string(STRIP "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${LOB_BENCH_BUILD_TYPE}}"
       LOB_BENCH_BUILD_FLAGS)
target_compile_definitions(lob_bench PRIVATE
                           "LOB_BENCH_BUILD_FLAGS=\"${LOB_BENCH_BUILD_FLAGS}\"")
target_link_libraries(lob_bench PRIVATE lob benchmark::benchmark
                                       hdr_histogram::hdr_histogram_static)

# SPSC ring buffer (Epic 04). Both are standalone tools over the header-only
# ring: a TSan stress loop and an enqueue-to-callback latency harness. The
# latency harness uses the benchmark harness's timer and mode rules, so it
# includes bench/ but links only the engine, the operation generator and HDR.
add_executable(spsc_stress "${CMAKE_CURRENT_SOURCE_DIR}/tools/spsc/stress.cpp")
target_include_directories(spsc_stress PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(spsc_stress PRIVATE lob)

add_executable(spsc_latency "${CMAKE_CURRENT_SOURCE_DIR}/tools/spsc/latency.cpp")
target_include_directories(spsc_latency PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(spsc_latency PRIVATE lob lob_opgen
                                             hdr_histogram::hdr_histogram_static)

# Tests. test_alloc replaces global operator new/delete, so it is its own
# binary and stays out of lob_tests. lobsim_core publishes the engine and tool
# libraries, so linking it alone keeps duplicates off the link line.
file(GLOB LOB_TEST_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.cpp")
list(REMOVE_ITEM LOB_TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_alloc.cpp")
add_executable(lob_tests ${LOB_TEST_SOURCES})
target_include_directories(lob_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
target_link_libraries(lob_tests PRIVATE lobsim_core lob_net GTest::gtest_main)

add_executable(test_alloc "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_alloc.cpp")
target_link_libraries(test_alloc PRIVATE lob GTest::gtest_main)

enable_testing()
include(GoogleTest)
gtest_discover_tests(lob_tests DISCOVERY_MODE PRE_TEST)
gtest_discover_tests(test_alloc DISCOVERY_MODE PRE_TEST)
