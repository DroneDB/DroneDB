FROM dronedb/builder AS builder

# Set environment variables
ENV CMAKE_BUILD_TYPE=Release \
    DEBIAN_FRONTEND=noninteractive

# Copy the source code
COPY . /workspace

RUN cd /workspace && mkdir -p build && \
    cd build && \
    cmake -DVCPKG_OVERLAY_TRIPLETS=/workspace/vcpkg-triplets -DVCPKG_HOST_TRIPLET=x64-linux-release -DVCPKG_TARGET_TRIPLET=x64-linux-release -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=/workspace/vcpkg/scripts/buildsystems/vcpkg.cmake ..

# Build the project
RUN cd /workspace/build && \
    make -j$(nproc)

# Optional: build the Untwine COPC accelerator (vendor/untwine submodule).
# Failure is non-blocking; DroneDB falls back to PDAL writers.copc when
# untwine is missing.
RUN cd /workspace && \
    chmod +x scripts/build-untwine.sh && \
    ./scripts/build-untwine.sh build Release || \
    echo "WARNING: untwine build failed (non-blocking, PDAL fallback will be used)"

# build-lod Gaussian Splat LOD producer (vendor/spark submodule): MANDATORY
# dependency of Gaussian Splat builds - failure aborts the image build.
# Requires a Rust toolchain (installed above if missing).
ENV PATH="/root/.cargo/bin:${PATH}"
RUN cd /workspace && \
    (command -v cargo >/dev/null 2>&1 || \
        (curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain stable --profile minimal)) && \
    chmod +x scripts/build-buildlod.sh && \
    DDB_REQUIRE_BUILDLOD=1 ./scripts/build-buildlod.sh build Release

# Optional: download the OpenDroneMap Obj2Tiles binary (OGC 3D Tiles generator).
# AGPL-3.0, invoked only as a subprocess (see THIRD_PARTY_LICENSES.md). Failure is
# non-blocking; DroneDB skips 3D Tiles generation (Nexus still produced) when missing.
RUN cd /workspace && \
    chmod +x scripts/download-obj2tiles.sh && \
    ./scripts/download-obj2tiles.sh build || \
    echo "WARNING: Obj2Tiles download failed (non-blocking, 3D Tiles generation disabled)"

# Optional: build the LibreDWG dwg2dxf converter (DWG support) from the signed GNU
# release. GPL-3.0, invoked only as a subprocess (see THIRD_PARTY_LICENSES.md).
# Failure is non-blocking; DWG builds then fail with an actionable message.
RUN cd /workspace && \
    (command -v pkg-config >/dev/null 2>&1 && command -v gpg >/dev/null 2>&1 || \
        (apt-get update && apt-get install -y --no-install-recommends pkg-config gpg xz-utils)) && \
    bash scripts/build-libredwg.sh build || \
    echo "WARNING: dwg2dxf build failed (non-blocking, DWG support disabled)"

# Build the Debian package
RUN cd /workspace && \
    chmod +x scripts/build-debian-package.sh && \
    ./scripts/build-debian-package.sh

# The Debian package on its own. CI replaces this stage with the .deb it has
# already tested (--build-context package=<dir>), so the source build above is skipped.
FROM scratch AS package
COPY --from=builder /workspace/build/package/*.deb /

# Create a smaller runtime image
FROM ubuntu:24.04 AS runtime

# Copy the Debian package and install it with its dependencies
COPY --from=package /*.deb /app/ddb.deb

# Install the DEB package and all its dependencies, then clean up
RUN apt-get update && \
    apt-get install -y /app/ddb.deb && \
    rm /app/ddb.deb && \
    rm -rf /var/lib/apt/lists/*

# Set the working directory
WORKDIR /data

# Set the entrypoint
ENTRYPOINT ["/usr/bin/ddb"]
CMD ["--help"]
