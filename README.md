![ddb-logo-banner](https://user-images.githubusercontent.com/1951843/86480474-0fcc4280-bd1c-11ea-8663-a7a37f631565.png)

![license](https://img.shields.io/badge/license-MPL--2.0-blue) ![commits](https://img.shields.io/github/commit-activity/m/DroneDB/DroneDB) ![languages](https://img.shields.io/github/languages/top/DroneDB/DroneDB) ![Docs](https://github.com/DroneDB/DroneDB/workflows/Docs/badge.svg) ![C/C++ CI](https://github.com/DroneDB/DroneDB/workflows/C/C++%20CI/badge.svg) ![NodeJS CI](https://github.com/DroneDB/DroneDB/workflows/NodeJS%20CI/badge.svg) ![.NET CI](https://github.com/DroneDB/DroneDB/workflows/.NET%20CI/badge.svg)

DroneDB is free and open source software for aerial data storage. It provides a convenient location to store images, orthophotos, digital elevation models, point clouds and vector files.

![image](https://user-images.githubusercontent.com/1951843/147839499-0c263b47-4e51-437c-adbb-cc0bea50d29f.png)

See it in action: [https://hub.dronedb.app/r/odm/waterbury](https://hub.dronedb.app/r/odm/waterbury)

## Documentation

https://docs.dronedb.app

## Project Overview

DroneDB is designed to efficiently manage, process, and share aerial and geospatial data. The project consists of several key components:

- **Core Library**: A C++ library that handles file processing, metadata extraction, and database operations
- **Command-Line Interface**: A powerful CLI tool to interact with DroneDB repositories
- **Plugins System**: Extensible architecture for adding custom functionality
- **API**: Programmatic access to DroneDB functionality

### Key Features

- Storage and indexing of various aerial data formats
- Automatic extraction of metadata and geospatial information
- Support for common formats including images, point clouds, orthophotos, vector files, and DEMs
- Multi-platform support (Windows, Linux, macOS)
- Integration with other tools in the GIS ecosystem

## Project Structure

```
├── apps/                  # Main application code and CLI interface
├── cmake/                 # Custom CMake modules
├── data/                  # Sample data and resources
├── docker/                # Docker build configurations
├── src/                   # Core library source code
├── tests/                 # Unit and integration tests
└── vendor/                # Third-party dependencies
```

## Requirements

### Windows

- Visual Studio 2019 or newer with C++ development tools
- CMake 3.16 or newer
- Git
- vcpkg package manager

### Linux

- GCC 9.0 or newer
- CMake 3.16 or newer
- Git
- vcpkg package manager
- Additional dependencies:
  - bison
  - flex
  - libreadline-dev
  - libssl-dev

## Build Instructions

### Prerequisites

Before building, make sure to set the `VCPKG_ROOT` environment variable to point to your vcpkg installation:

**Windows (PowerShell):**
```powershell
$env:VCPKG_ROOT = "C:\path\to\vcpkg"
```

**Linux/macOS:**
```bash
export VCPKG_ROOT=/path/to/vcpkg
```

### Clone Repository

```bash
git clone https://github.com/DroneDB/DroneDB.git
cd DroneDB
git submodule update --init --recursive
```

### Windows Build

```powershell
# Configure with CMake
cmake -B build -S . -A x64 -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"

# Build
cmake --build build --config Release --target ALL_BUILD -- /maxcpucount:14

# Run tests
.\build\tests\Release\ddbtest.exe --gtest_shuffle --gtest_repeat=2 --gtest_recreate_environments_when_repeating
```

For a quicker build, you can use the provided batch script:
```powershell
.\full-build-win.bat
```

### Linux Build

```bash
# Install required dependencies
sudo apt install build-essential cmake git bison flex libreadline-dev libssl-dev

# Configure with CMake
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"

# Build
cmake --build build --config Release -j$(nproc)

# Run tests
./build/tests/ddbtest --gtest_shuffle --gtest_repeat=2 --gtest_recreate_environments_when_repeating
```

### Updating Vcpkg Dependencies

If you need to update the vcpkg dependencies:

```bash
vcpkg x-update-baseline --add-initial-baseline
```
## Running Tests

Tests are built using Google Test framework:

**Windows:**
```powershell
.\build\tests\Release\ddbtest.exe --gtest_shuffle --gtest_repeat=2 --gtest_recreate_environments_when_repeating
```

**Linux:**
```bash
./build/tests/ddbtest --gtest_shuffle --gtest_repeat=2 --gtest_recreate_environments_when_repeating
```

## Contributing

Please read [CONTRIBUTING.md](CONTRIBUTING.md) for details on our code of conduct and the process for submitting pull requests.

## License

This project is licensed under the [Mozilla Public License Version 2.0 (MPL 2.0)](LICENSE.md) - see the LICENSE.md file for details.

