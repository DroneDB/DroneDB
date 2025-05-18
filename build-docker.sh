#!/bin/bash
# Build Docker images for DroneDB using vcpkg

set -e

echo "Building Ubuntu 24.04 builder image..."
docker build -t dronedb/builder -f docker/builders/Dockerfile-builder-24.04 docker/builders

# Build the main application image
echo "Building main DroneDB application image..."
docker build -t dronedb/app:latest -f docker/Dockerfile .

echo "All images built successfully!"
echo "You can run the DroneDB application with: docker run --rm -it dronedb/app:latest"
