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
    make -j$(nproc) exodus_package

# Create a smaller runtime image
FROM ubuntu:24.04

# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    libstdc++6 \
    libc6 \
    && rm -rf /var/lib/apt/lists/*

# Copy the built executable and dependencies from builder
COPY --from=0 /workspace/build/dist/ddb-*.tgz /app/ddb.tgz

# Extract the exodus package
RUN cd /app && \
    tar -xzvf ddb.tgz && \
    rm ddb.tgz && \
    ln -s /app/exodus/bin/ddbcmd /usr/local/bin/ddb

# Set the working directory
WORKDIR /data

# Set the entrypoint
ENTRYPOINT ["/usr/local/bin/ddb"]
CMD ["--help"]
