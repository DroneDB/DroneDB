# DroneDB Development Instructions for AI Agents

## Project Overview
DroneDB is a free, open-source aerial data management platform built in C++ with Node.js bindings. It provides efficient storage, processing, and sharing of geospatial data including images, orthophotos, digital elevation models, point clouds, and vector files.

## Architecture Overview

### Core Components

#### 1. **Core Library (`src/library/`)**
- **Database Layer**: SQLite with SpatiaLite extensions for spatial data
  - Main class: `Database` (inherits from `SqliteDatabase`)
  - Handles file indexing, metadata storage, and spatial queries
  - Schema includes `entries`, `entries_meta`, `passwords` tables

- **Entry Management**: File parsing and metadata extraction
  - `Entry` class represents indexed files with properties, geometry, and type
  - Supports images (EXIF), raster data (GDAL), point clouds (PDAL), vectors
  - Automatic type detection and metadata extraction

- **Geospatial Processing**:
  - Geographic coordinate transformations (UTM, WGS84)
  - Spatial geometry handling (points, polygons)
  - Integration with GDAL/OGR for raster/vector operations
  - PROJ for coordinate system transformations

- **Metadata Management**:
  - `MetaManager` for key-value metadata storage
  - JSON-based properties system
  - Support for custom metadata and tags

#### 2. **Command Line Interface (`apps/` and `cmd/`)**
- Main executable: `ddbcmd`
- Commands: init, add, remove, list, search, build, info, etc.
- Follows Unix philosophy with composable operations

#### 3. **Build System**
- Uses vcpkg for dependency management
- Cross-platform CMake configuration
- Support for Windows, Linux, macOS
- Automatic dependency resolution for GDAL, PROJ, PDAL, etc.
- To build on Windows use `full-build-win.ps1` script and to build on Linux use `full-build-linux.sh` script (skip the cmake part if the build directory already exists)

#### 4. **3D Visualization (`vendor/libnexus/`)**
- Nexus format for efficient 3D mesh streaming
- Web-based 3D viewer with progressive loading
- Point cloud and mesh visualization

### Key Dependencies
- **GDAL**: Raster and vector data I/O
- **PDAL**: Point cloud processing
- **Untwine**: COPC format for efficient streaming
- **PROJ**: Coordinate system transformations
- **SpatiaLite**: Spatial database operations
- **Exiv2**: Image metadata extraction
- **nlohmann/json**: JSON processing
- **cpr**: HTTP client for registry operations

### Data Flow
1. **Indexing**: Files are parsed, metadata extracted, and stored in database
2. **Querying**: Spatial and attribute queries on indexed data
3. **Processing**: Build operations for derived products (tiles, previews)
4. **Sharing**: Registry operations for remote collaboration

## Development Guidelines

### Code Quality Standards
- **Language**: Modern C++17 with clear, descriptive naming
- **Memory Management**: RAII, smart pointers, avoid manual memory management
- **Error Handling**: Use exceptions (`AppException`) with meaningful messages
- **Logging**: Use `LOGD`, `LOGV` macros for debug output
- **Testing**: Write unit tests for new functionality

### API Design Principles
- **Consistency**: Follow existing patterns in codebase
- **Safety**: Validate inputs, handle edge cases gracefully
- **Performance**: Minimize memory allocation, use spatial indexing
- **Extensibility**: Design for plugin architecture and future features

### Specific Patterns
- **Database Operations**: Always use prepared statements and transactions
- **File Operations**: Use `fs::path` for cross-platform compatibility
- **Spatial Operations**: Leverage SpatiaLite for complex spatial queries
- **JSON Handling**: Use nlohmann::json for structured data
- **Progress Reporting**: Implement progress callbacks for long operations

### Architecture Considerations
- **Modularity**: Keep geospatial logic separate from database logic
- **Thread Safety**: Database operations should be thread-safe
- **Resource Management**: Clean up GDAL/PROJ resources properly
- **Platform Compatibility**: Test on Windows, Linux, macOS

## Development Workflow

### Adding New Features
1. **Analysis**: Understand requirements and identify affected components
2. **Design**: Plan database schema changes, API modifications
3. **Implementation**: Write core logic with proper error handling
4. **Testing**: Add unit tests and integration tests (use TestFS and TestArea for file-based tests)
5. **Documentation**: Update API docs and user documentation

### Common Operations
- **Adding File Types**: Extend `parseEntry()` in `entry.cpp`
- **Database Schema**: Update DDL in `database.cpp`, add migration logic
- **Spatial Operations**: Use existing geo functions in `geo.cpp`
- **API Extensions**: Add to C API in `ddb.h`, Node.js bindings in `nodejs/`

### Performance Guidelines
- **Database**: Use spatial indexes, batch operations, prepared statements
- **Memory**: Stream large files, limit memory usage for big datasets
- **I/O**: Minimize file system operations, cache metadata
- **Concurrency**: Use thread pools for parallel processing where appropriate

## AI Agent Guidelines

### Code Analysis
- **Read existing code patterns** before implementing new features
- **Understand the complete data flow** from input files to database storage
- **Check for existing utilities** before writing new helper functions
- **Respect the layered architecture** - don't bypass abstraction layers

### Making Changes
- **Always ask for confirmation** before modifying code
- **Provide detailed explanations** of changes and their impact
- **Consider backwards compatibility** for API changes
- **Test changes thoroughly** including edge cases

### Problem Solving
- **Break down complex tasks** into smaller, manageable components
- **Leverage existing infrastructure** rather than reinventing solutions
- **Consider performance implications** of proposed changes
- **Think about error scenarios** and how to handle them gracefully

### Documentation
- **Comment complex algorithms** and spatial operations
- **Document API changes** with clear examples
- **Explain architectural decisions** for future maintainers
- **Keep README and docs updated** with significant changes
- **Do not** create new documentation files or guides without explicit instructions to do so

## Quality Assurance
- **Run full test suite** before proposing changes
- **Verify cross-platform compatibility**
- **Check memory leaks** with tools like Valgrind
- **Validate spatial operations** with known test datasets
- **Performance regression testing** for core operations

Remember: DroneDB handles valuable geospatial data - prioritize correctness, reliability, and data integrity in all implementations.
