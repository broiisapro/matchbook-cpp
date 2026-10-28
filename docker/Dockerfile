# syntax=docker/dockerfile:1
#
# linux/arm64 image for the portable targets: the fast engine, the reference
# engine, the flow-file tools, the operation generator, the wire protocol and
# lobsim. The tests run during the build, so a broken Linux port fails the
# image rather than shipping a binary nobody exercised.
#
# Build from the repository root (scripts/docker_build.sh does exactly this):
#   docker build --platform linux/arm64 -f docker/Dockerfile -t matchbook-cpp:linux-arm64 .
#
# The benchmark binary is deliberately not part of this image: its timers use
# mach_absolute_time and it only makes sense on the bare-metal macOS machine
# the published numbers come from.

ARG UBUNTU_VERSION=24.04

# ---------------------------------------------------------------------------
# vcpkg at the pinned commit, in its own stage so editing the source does not
# re-clone the toolchain. The commit is the builtin-baseline in vcpkg.json.
# ---------------------------------------------------------------------------
FROM ubuntu:${UBUNTU_VERSION} AS vcpkg

ARG VCPKG_COMMIT=9e593bb18ea69cc5095e012465dcd675a822ed0d

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates curl git tar unzip zip \
    && rm -rf /var/lib/apt/lists/*

RUN git clone https://github.com/microsoft/vcpkg /opt/vcpkg \
    && git -C /opt/vcpkg checkout --detach "${VCPKG_COMMIT}" \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics

# ---------------------------------------------------------------------------
# Build stage: configure, build the portable targets and run the tests.
# ---------------------------------------------------------------------------
FROM ubuntu:${UBUNTU_VERSION} AS build

ENV DEBIAN_FRONTEND=noninteractive \
    VCPKG_DISABLE_METRICS=1 \
    VCPKG_ROOT=/opt/vcpkg

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential ca-certificates clang cmake curl git ninja-build pkg-config \
        tar unzip zip \
    && rm -rf /var/lib/apt/lists/*

COPY --from=vcpkg /opt/vcpkg /opt/vcpkg

WORKDIR /src
COPY . /src

# Configure directly instead of through CMakePresets.json: the presets pin the
# macOS triplet (arm64-osx) and the macOS toolchain paths. The build directory
# sits inside the source tree because tests locate committed fixtures
# (tests/data/) by walking up from their working directory.
#
# Two Linux-only configure flags:
#   CMAKE_CXX_SCAN_FOR_MODULES=OFF  the project uses no C++ modules and
#       Ubuntu's clang package does not ship clang-scan-deps.
#   -fsized-deallocation  include/lob/pool.hpp calls the sized aligned
#       operator delete; libstdc++ declares that overload only when the
#       compiler defines __cpp_sized_deallocation, while libc++ (macOS)
#       declares it unconditionally. Tracked as a portability fix request.
RUN cmake -S /src -B /src/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
        -DCMAKE_CXX_FLAGS=-fsized-deallocation \
        -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
        -DVCPKG_TARGET_TRIPLET=arm64-linux \
        -DLOB_PRICE_LADDER=array

# test_alloc is built too: ctest registers its tests, so leaving the binary out
# would leave the suite unable to discover them.
RUN cmake --build /src/build --target \
        lob lob_reference lob_flowfile lob_opgen lob_net lobsim lob_tests test_alloc

RUN ctest --test-dir /src/build --output-on-failure

# ---------------------------------------------------------------------------
# Runtime stage: just the CLI. `lobsim` is statically linked against zlib and
# xxHash from vcpkg and needs only the C++ runtime.
# ---------------------------------------------------------------------------
FROM ubuntu:${UBUNTU_VERSION} AS runtime

RUN apt-get update \
    && apt-get install -y --no-install-recommends libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/lobsim /usr/local/bin/lobsim

ENTRYPOINT ["/usr/local/bin/lobsim"]
CMD ["--help"]
