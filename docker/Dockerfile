# syntax=docker/dockerfile:1
#
# linux/arm64 image for the portable targets: the fast engine, the reference
# engine, the flow-file tools, the operation generator, the wire protocol,
# lobsim and the Phase 3 services (gateway, publisher, market-data listener and
# load generator). The tests run during the build, so a broken Linux port fails
# the image rather than shipping a binary nobody exercised.
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
# Linux-only configure flags: the project uses no C++ modules and Ubuntu's
# clang package does not ship clang-scan-deps, so CMake's module scanner is
# turned off; LOB_BUILD_BENCH=OFF drops the benchmark binary and the SPSC
# latency harness, the only two targets that include the macOS timer in
# bench/timer.hpp. The runtime output directory is flattened so every
# executable, including the apps/<name>/ ones, lands in /src/build for the COPY
# below.
RUN cmake -S /src -B /src/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
        -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=/src/build \
        -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
        -DVCPKG_TARGET_TRIPLET=arm64-linux \
        -DLOB_PRICE_LADDER=array \
        -DLOB_BUILD_BENCH=OFF

# The default target set: with the benchmark option off, `all` is exactly the
# portable tree, the same set the Linux CI job builds. test_alloc is part of it
# and ctest would otherwise be unable to discover its tests. The four Phase 3
# executables are what docker-compose.yml runs.
RUN cmake --build /src/build --target all

RUN ctest --test-dir /src/build --output-on-failure

# Strip the five binaries the runtime stage copies: a Release build keeps its
# symbol table, which the demo services never use. The two test binaries stay
# unstripped, so a failing in-image run still prints usable frames. binutils
# arrives with build-essential.
RUN strip --strip-unneeded \
        /src/build/lobsim \
        /src/build/lob_gateway \
        /src/build/lob_publisher \
        /src/build/lob_md_listen \
        /src/build/lob_loadgen

# ---------------------------------------------------------------------------
# Runtime stage: exactly what docker-compose.yml runs, plus the CLI — five
# binaries and the load generator's argument wrapper. Everything is statically
# linked against zlib, xxHash and the HDR histogram from vcpkg, so the only
# shared dependencies left are the C++ runtime (libstdc++, libgcc_s) and libc.
#
# Nothing is installed here. The Ubuntu base image already carries those
# runtime libraries, and it already carries the shell tools the Compose health
# checks use (bash with /dev/tcp, /proc/net and stdbuf), so an apt layer would
# fetch and unpack packages that are present at the same versions — and would
# make the runtime stage fail without network access.
# ---------------------------------------------------------------------------
FROM ubuntu:${UBUNTU_VERSION} AS runtime

COPY --from=build /src/build/lobsim /usr/local/bin/lobsim
COPY --from=build /src/build/lob_gateway /usr/local/bin/lob_gateway
COPY --from=build /src/build/lob_publisher /usr/local/bin/lob_publisher
COPY --from=build /src/build/lob_md_listen /usr/local/bin/lob_md_listen
COPY --from=build /src/build/lob_loadgen /usr/local/bin/lob_loadgen
COPY --chmod=755 docker/loadgen-entrypoint.sh /usr/local/bin/loadgen-entrypoint

ENTRYPOINT ["/usr/local/bin/lobsim"]
CMD ["--help"]
