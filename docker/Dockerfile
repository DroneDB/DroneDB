FROM dronedb/builder

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

# Optional: build the build-lod Gaussian Splat LOD producer (vendor/spark submodule).
# Requires a Rust toolchain; failure is non-blocking - Gaussian Splats are then
# served without LOD streaming (plain model.spz).
ENV PATH="/root/.cargo/bin:${PATH}"
RUN cd /workspace && \
    (command -v cargo >/dev/null 2>&1 || \
        (curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain stable --profile minimal)) && \
    chmod +x scripts/build-buildlod.sh && \
    ./scripts/build-buildlod.sh build Release || \
    echo "WARNING: build-lod build failed (non-blocking, Gaussian Splats served without LOD streaming)"

# Optional: download the OpenDroneMap Obj2Tiles binary (OGC 3D Tiles generator).
# AGPL-3.0, invoked only as a subprocess (see THIRD_PARTY_LICENSES.md). Failure is
# non-blocking; DroneDB skips 3D Tiles generation (Nexus still produced) when missing.
RUN cd /workspace && \
    chmod +x scripts/download-obj2tiles.sh && \
    ./scripts/download-obj2tiles.sh build || \
    echo "WARNING: Obj2Tiles download failed (non-blocking, 3D Tiles generation disabled)"

# Build the Debian package
RUN cd /workspace && \
    chmod +x scripts/build-debian-package.sh && \
    ./scripts/build-debian-package.sh

# Create a smaller runtime image
FROM ubuntu:24.04

# Copy the built executable and dependencies from builder
COPY --from=0 /workspace/build/package/*.deb /app/ddb.deb

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
