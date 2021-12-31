![ddb-logo-banner](https://user-images.githubusercontent.com/1951843/86480474-0fcc4280-bd1c-11ea-8663-a7a37f631565.png)

![license](https://img.shields.io/github/license/DroneDB/DroneDB) ![commits](https://img.shields.io/github/commit-activity/m/DroneDB/DroneDB) ![languages](https://img.shields.io/github/languages/top/DroneDB/DroneDB) ![Docs](https://github.com/DroneDB/DroneDB/workflows/Docs/badge.svg) ![C/C++ CI](https://github.com/DroneDB/DroneDB/workflows/C/C++%20CI/badge.svg) ![NodeJS CI](https://github.com/DroneDB/DroneDB/workflows/NodeJS%20CI/badge.svg) ![.NET CI](https://github.com/DroneDB/DroneDB/workflows/.NET%20CI/badge.svg)

DroneDB is free and open source software for aerial data storage. It provides a convenient location to store images, orthophotos, digital elevation models, point clouds and any other file.

![image](https://user-images.githubusercontent.com/1951843/147839499-0c263b47-4e51-437c-adbb-cc0bea50d29f.png)

**Example Dataset**: https://hub.dronedb.app/r/pierotofy/brighton-beach

## Documentation

https://docs.dronedb.app

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

## Building NodeJS bindings

After you've made a successful build (see above), make sure the `build` directory is empty (remove it) and that you have `npm` installed. Then:

```
npm install
```

Should be sufficient to build the NodeJS bindindgs.

You can test that they work by issuing:

```
scripts\setup_windows_env.bat (Windows only)
npm test
```

