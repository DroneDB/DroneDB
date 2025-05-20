# Build Docker images for DroneDB using vcpkg
Write-Host "Building Ubuntu 24.04 builder image..." -ForegroundColor Green
docker build -t dronedb/builder -f docker/builders/Dockerfile-builder-24.04 docker/builders

# Build the main application image
Write-Host "Building main DroneDB application image..." -ForegroundColor Green
docker build -t dronedb/dronedb:latest -f docker/Dockerfile .

Write-Host "All images built successfully!" -ForegroundColor Green
Write-Host "You can run the DroneDB application with: docker run --rm -it dronedb/dronedb:latest" -ForegroundColor Cyan
