FROM ddb/builder:24.04

# Set environment variables
ENV CMAKE_BUILD_TYPE=Release \
    DEBIAN_FRONTEND=noninteractive

# Copy the source code
COPY . /workspace

# Build the project
RUN cd /workspace && \
    chmod +x ./full-build-linux.sh && \
    ./full-build-linux.sh && \
    cd build && \
    # Create distributable package
    cmake --build . --target exodus_package

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
    ln -s /app/ddb /usr/local/bin/

# Set the working directory
WORKDIR /data

# Set the entrypoint
ENTRYPOINT ["/usr/local/bin/ddb"]
CMD ["--help"]
