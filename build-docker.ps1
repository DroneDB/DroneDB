# Build Docker images for DroneDB using vcpkg

# Build the builder images
Write-Host "Building Ubuntu 22.04 builder image..." -ForegroundColor Green
docker build -t ddb/builder:22.04 -f docker/builders/Dockerfile-builder-22.04 docker/builders

Write-Host "Building Ubuntu 24.04 builder image..." -ForegroundColor Green
docker build -t ddb/builder:24.04 -f docker/builders/Dockerfile-builder-24.04 docker/builders

# Build the main application image
Write-Host "Building main DroneDB application image..." -ForegroundColor Green
docker build -t ddb/app:latest -f docker/Dockerfile .

Write-Host "All images built successfully!" -ForegroundColor Green
Write-Host "You can run the DroneDB application with: docker run --rm -it ddb/app:latest" -ForegroundColor Cyan
