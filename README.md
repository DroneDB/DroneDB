![ddb-logo-banner](https://user-images.githubusercontent.com/1951843/86480474-0fcc4280-bd1c-11ea-8663-a7a37f631565.png)

![license](https://img.shields.io/github/license/DroneDB/DroneDB) ![commits](https://img.shields.io/github/commit-activity/m/DroneDB/DroneDB) ![languages](https://img.shields.io/github/languages/top/DroneDB/DroneDB) ![Docs](https://github.com/DroneDB/DroneDB/workflows/Docs/badge.svg) ![C/C++ CI](https://github.com/DroneDB/DroneDB/workflows/C/C++%20CI/badge.svg) ![NodeJS CI](https://github.com/DroneDB/DroneDB/workflows/NodeJS%20CI/badge.svg) ![.NET CI](https://github.com/DroneDB/DroneDB/workflows/.NET%20CI/badge.svg)

DroneDB is a toolset for easily managing and sharing aerial datasets. It can index and extract useful information from the EXIF/XMP tags of aerial images to display things like image footprint, flight path and image GPS location. It has timezone-aware date parsing capabilities, a camera sensor database and a DSM lookup system.

![image](https://user-images.githubusercontent.com/1951843/66138811-3dd5f800-e5cd-11e9-816d-a0efa39ccca5.png)

![image](https://user-images.githubusercontent.com/1951843/68077866-001de800-fda2-11e9-895f-b5840d9d047d.png)

DroneDB is in early development stages and it's currently targeted at GIS geeks and early adopters. While it's limited in functionality, it can already be used. Give it a try! To contribute to the project, please see the [contributing guidelines](CONTRIBUTING.md).

## Installation

On Linux simply run:

```bash
$ curl -fsSL https://get.dronedb.app -o get-ddb.sh
$ sh get-ddb.sh
```

On Windows simply download the precompiled binaries from https://github.com/DroneDB/DroneDB/releases/. Binaries for Linux are also published on that page.

On Windows make sure the path to **ddb.bat** program is [in your PATH environment variable](https://helpdeskgeek.com/windows-10/add-windows-path-environment-variable/).


## Documentation

For usage, tutorials and references see https://docs.dronedb.app

## Building

Requirements:
 * sqlite3
 * spatialite
 * cmake
 * libgeos
 * g++ >= 10.1.0 (very important! :warning: g++ 8 has a [bug](https://gcc.gnu.org/bugzilla/show_bug.cgi?id=90050) in the stdc++fs library that will prevent the software from running properly)
 * GDAL >= 2.1
 
On Ubuntu you can simply execute this script to install the dependencies:

```bash
scripts/ubuntu_deps.sh
```

Then:

```bash
git clone --recurse-submodules https://github.com/DroneDB/DroneDB ddb
cd ddb
mkdir build && cd build
cmake .. && make
```

On Windows you should install Visual Studio (the free Community Edition works great), Git and CMake. Then:

```
git clone --recurse-submodules https://github.com/DroneDB/DroneDB ddb
cd ddb
md build && cd build
cmake ..
cmake --build . --config Release --target ALL_BUILD -- /maxcpucount:14
```

