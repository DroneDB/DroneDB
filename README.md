![ddb-logo-banner](https://user-images.githubusercontent.com/1951843/86480474-0fcc4280-bd1c-11ea-8663-a7a37f631565.png)

[![License: MPL 2.0](https://img.shields.io/badge/license-MPL--2.0-blue)](LICENSE.md)
[![GitHub commits](https://img.shields.io/github/commit-activity/m/DroneDB/DroneDB)](https://github.com/DroneDB/DroneDB/commits)
[![C/C++ CI](https://github.com/DroneDB/DroneDB/actions/workflows/c-cpp.yml/badge.svg)](https://github.com/DroneDB/DroneDB/actions/workflows/c-cpp.yml)

**DroneDB** is a free, open-source platform for efficient aerial data management and sharing. Store, index, process, and share images, orthophotos, digital elevation models, point clouds, and vector files with ease.

![DroneDB Screenshot](https://user-images.githubusercontent.com/1951843/147839499-0c263b47-4e51-437c-adbb-cc0bea50d29f.png)

**See it in action:** [hub.dronedb.app/r/odm/waterbury](https://hub.dronedb.app/r/odm/waterbury)

---

## Key Features

- **Smart Indexing** - Automatic metadata extraction from images (EXIF), raster data (GDAL), point clouds (PDAL), and vector files
- **Geospatial Support** - Built-in coordinate transformations (UTM, WGS84), spatial queries with SpatiaLite
- **Thumbnails & Tiles** - Generate previews and map tiles from your data
- **Cloud Sync** - Push and pull datasets to/from DroneDB Hub for collaboration
- **STAC Catalog** - Export datasets as SpatioTemporal Asset Catalogs
- **Password Protection** - Secure your datasets with encrypted passwords
- **Cross-Platform** - Works on Windows and Linux

---

## Quick Start

### Installation

#### Linux (Debian/Ubuntu)
```bash
# Download and install from the latest release
sudo apt install ./ddb_X.Y.Z_amd64.deb
```

#### Windows
Download the latest release from [GitHub Releases](https://github.com/DroneDB/DroneDB/releases) and run the installer.

### Basic Usage

```bash
# Initialize a new DroneDB repository
ddb init

# Add files to the repository
ddb add *.jpg orthophoto.tif

# List indexed files with metadata
ddb list

# Get detailed information about files
ddb info image.jpg

# Generate thumbnails
ddb thumbs *.jpg

# Search for files
ddb search --type image

# Build COG (Cloud-Optimized GeoTIFF) from raster
ddb cog input.tif output.tif

# Share your dataset to DroneDB Hub
ddb share . --tag myproject/dataset
```

### Available Commands

| Command | Description |
|---------|-------------|
| `init` | Initialize a new DroneDB repository |
| `add` | Add files to the index |
| `remove` | Remove files from the index |
| `list` | List indexed files |
| `info` | Display file information and metadata |
| `search` | Search files by criteria |
| `build` | Build derivative products (tiles, previews) |
| `thumbs` | Generate thumbnails |
| `tile` | Generate map tiles |
| `cog` | Create Cloud-Optimized GeoTIFFs |
| `ept` | Create Entwine Point Tiles from point clouds |
| `nxs` | Create Nexus 3D mesh format |
| `stac` | Export as STAC catalog |
| `clone` | Clone a remote repository |
| `push` / `pull` | Sync with DroneDB Hub |
| `share` | Share datasets online |
| `meta` | Manage metadata |
| `password` | Set/manage repository password |

Run `ddb <command> --help` for detailed usage of each command.

---

## Documentation

Full documentation is available at **[docs.dronedb.app](https://docs.dronedb.app)**

---

## Architecture

DroneDB consists of:

- **Core Library** - C++ library for file processing, metadata extraction, and SQLite/SpatiaLite database operations
- **CLI Tool (`ddb`)** - Command-line interface for all operations

---

## Building from Source

DroneDB uses **vcpkg** for dependency management and CMake for building.

### Prerequisites

- **C++17 compiler** (GCC 9+, Clang 9+, MSVC 2019+)
- **CMake 3.21+**
- **Python 3.x**
- **Git**
- **Visual Studio 2019+** (Windows only, with C++ desktop development workload)

### Quick Build

**Windows (PowerShell):**
```powershell
git clone https://github.com/microsoft/vcpkg.git; cd vcpkg; .\bootstrap-vcpkg.bat
$env:VCPKG_ROOT = $(Get-Location).Path
cd ..; git clone https://github.com/DroneDB/DroneDB.git; cd DroneDB
.\full-build-win.ps1
```

**Linux:**
```bash
git clone https://github.com/microsoft/vcpkg.git && cd vcpkg && ./bootstrap-vcpkg.sh
export VCPKG_ROOT=$(pwd)
cd .. && git clone https://github.com/DroneDB/DroneDB.git && cd DroneDB
./full-build-linux.sh
```

### Windows Build Script

DroneDB provides a robust PowerShell build script that automatically detects Visual Studio, configures CMake, and builds using Ninja.

#### Basic Usage

```powershell
# Debug build (default)
.\full-build-win.ps1

# Release build
.\full-build-win.ps1 -BuildType Release

# Clean build
.\full-build-win.ps1 -Clean

# Custom configuration
.\full-build-win.ps1 -BuildType Release -Clean -Jobs 8
```

#### Script Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `-BuildType` | String | `Debug` | Build configuration: `Debug`, `Release`, `RelWithDebInfo`, or `MinSizeRel` |
| `-Clean` | Switch | `false` | Remove CMake cache and build artifacts before building |
| `-SkipTests` | Switch | `false` | Disable test compilation |
| `-Jobs` | Int | CPU cores | Number of parallel build jobs |

#### Build Output

After successful build, find executables in the `build/` directory:
- **ddbcmd.exe** - Command-line tool
- **ddb.dll** - Core library
- **ddbtest.exe** - Test suite

#### Troubleshooting

**Visual Studio Not Found**
- The script automatically searches for VS 2019-2025 using `vswhere.exe`
- Install Visual Studio with "Desktop development with C++" workload
- Or set `VSINSTALLDIR` environment variable

**CMake Not Found**
- Install from [cmake.org/download](https://cmake.org/download/)
- Or via Visual Studio Installer (Individual Components → CMake)

**Build Failures**
- Try a clean build: `.\full-build-win.ps1 -Clean`
- Manually bootstrap vcpkg: `cd vcpkg; .\bootstrap-vcpkg.bat`
- Check prerequisites are installed and in PATH

### Docker Build

```bash
./build-docker.sh
docker run --rm -it -v $(pwd):/data ddb/app:latest
```

### Manual Build Steps

<details>
<summary>Click to expand manual build instructions</summary>

#### Set VCPKG_ROOT

```bash
# Linux/macOS
export VCPKG_ROOT=/path/to/vcpkg

# Windows PowerShell
$env:VCPKG_ROOT = "C:\path\to\vcpkg"
```

#### Build Commands

```bash
git clone https://github.com/DroneDB/DroneDB.git
cd DroneDB
git submodule update --init --recursive

# Configure
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"

# Build
cmake --build build --config Release -j$(nproc)  # Linux
cmake --build build --config Release -- /maxcpucount:14  # Windows
```

#### Windows Manual Build (Visual Studio Developer Command Prompt)

```powershell
mkdir build && cd build
cmake .. -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="..\vcpkg\scripts\buildsystems\vcpkg.cmake"
cmake --build . -- -j16
```

#### Run Tests

```bash
# Linux
cd build && ./ddbtest --gtest_shuffle

# Windows
cd build && .\ddbtest.exe --gtest_shuffle
```

</details>

---

## Contributing

Contributions are welcome! Please read [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines on how to submit pull requests.

## License

This project is licensed under the [Mozilla Public License 2.0 (MPL-2.0)](LICENSE.md).

---

<p align="center">
  Made with ❤️ by <a href="https://digipa.tech">Digipa</a>
</p>