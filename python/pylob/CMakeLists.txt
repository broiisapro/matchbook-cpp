# Standalone CMake project for the pylob extension module. scikit-build-core
# configures it inside the isolated build environment, so it must not rely on
# the repository's vcpkg toolchain or on the root CMakeLists.txt.
cmake_minimum_required(VERSION 3.28)

project(pylob LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Python COMPONENTS Interpreter Development.Module REQUIRED)
find_package(pybind11 CONFIG REQUIRED)

# The engine, reference engine and flow-file reader live at the repository
# root. A pylob checkout is always part of the repository (the root
# pyproject.toml lists it as a uv workspace member).
get_filename_component(MATCHBOOK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# xxHash: reuse a configured package when the build happens to see one (for
# example through vcpkg or Homebrew), otherwise fetch the pinned 0.8.3 release
# and compile its single C source directly. Compiling the source ourselves
# keeps the upstream install rules (library, header, pkg-config file) out of
# the wheel.
find_package(xxHash CONFIG QUIET)
if(xxHash_FOUND)
  message(STATUS "pylob: using the configured xxHash package")
else()
  message(STATUS "pylob: downloading xxHash 0.8.3")
  include(FetchContent)
  FetchContent_Declare(
    xxhash
    URL https://github.com/Cyan4973/xxHash/archive/refs/tags/v0.8.3.tar.gz
    URL_HASH SHA256=aae608dfe8213dfd05d909a57718ef82f30722c392344583d3f39050c7f29a80
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    # An existing directory without a CMakeLists.txt makes FetchContent
    # populate the source tree without adding the upstream subproject.
    SOURCE_SUBDIR do-not-add-upstream-project
    SYSTEM
  )
  FetchContent_MakeAvailable(xxhash)
  add_library(pylob_xxhash STATIC "${xxhash_SOURCE_DIR}/xxhash.c")
  target_include_directories(pylob_xxhash SYSTEM PUBLIC "${xxhash_SOURCE_DIR}")
  add_library(xxHash::xxhash ALIAS pylob_xxhash)
endif()

# The engine sources pylob needs: the fast engine, the reference engine (so
# replay can be checked against it) and the flow-file reader/writer. The price
# ladder must be chosen explicitly, exactly as the root presets do.
add_library(pylob_engine STATIC
  "${MATCHBOOK_ROOT}/src/order_book.cpp"
  "${MATCHBOOK_ROOT}/reference/reference_book.cpp"
  "${MATCHBOOK_ROOT}/tools/flowfile/flowfile.cpp"
)
target_include_directories(pylob_engine PUBLIC
  "${MATCHBOOK_ROOT}"
  "${MATCHBOOK_ROOT}/include"
)
target_compile_definitions(pylob_engine PUBLIC LOB_PRICE_LADDER_ARRAY=1)
target_compile_options(pylob_engine PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(pylob_engine PUBLIC xxHash::xxhash)

pybind11_add_module(_core MODULE src/bindings.cpp)
target_compile_options(_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(_core PRIVATE pylob_engine)

install(TARGETS _core LIBRARY DESTINATION pylob)
