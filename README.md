![ddb-logo-banner](https://user-images.githubusercontent.com/1951843/86480474-0fcc4280-bd1c-11ea-8663-a7a37f631565.png)

![license](https://img.shields.io/github/license/uav4geo/DroneDB) ![commits](https://img.shields.io/github/commit-activity/m/uav4geo/DroneDB) ![languages](https://img.shields.io/github/languages/top/uav4geo/DroneDB)

DroneDB is a toolset for easily managing and sharing aerial datasets. It can index and extract useful information from the EXIF/XMP tags of aerial images to display things like image footprint, flight path and image GPS location. It has timezone-aware date parsing capabilities, a camera sensor database and a DSM lookup system.

![image](https://user-images.githubusercontent.com/1951843/66138811-3dd5f800-e5cd-11e9-816d-a0efa39ccca5.png)

![image](https://user-images.githubusercontent.com/1951843/68077866-001de800-fda2-11e9-895f-b5840d9d047d.png)

DroneDB is in early development stages and is targeted at GIS developers and early adopters. It is not ready for mainstream use. To contribute to the project, please see the [contributing guidelines](CONTRIBUTING.md).

## Building

Requirements:
 * sqlite3
 * spatialite
 * exiv2
 * cmake
 * libgeos
 * gcc-8
 * GDAL >= 2.1
 
On Ubuntu you can simply execute this script to install the dependencies:

```bash
scripts/ubuntu_deps.sh
```

Then:

```bash
git clone --recurse-submodules https://github.com/uav4geo/DroneDB ddb
cd ddb
mkdir build && cd build
cmake .. && make
```

## Usage

To create a new index:

```bash
ddb init
```

To add images to an index:

```bash
ddb add images/
ddb add images/DJI_0018.JPG
ddb add images/*.JPG
```

To remove images from an index:

```bash
ddb rm images/DJI_0018.JPG
```

To sync an index after filesystem changes:

```bash
ddb sync
```

To project an image onto a map:

```bash
ddb geoproj images/DJI_0018.JPG -o projected/
```

## Browsing data with QGIS

You can explore the indexed data with a program such as QGIS. The index creates a `.ddb/dbase.sqlite` file when running the `ddb init` command. This is a SpatiaLite database and can be imported by adding a new SpatiaLite layer.

### Viewing Flight Path LineString

Create a new query similar to:

```
SELECT AsGeoJSON(MakeLine(point_geom)) FROM (SELECT point_geom, json_extract(meta, '$.captureTime') as captureTime FROM entries ORDER by captureTime ASC);
```

The 'point_geom' column contains the GPS locations, while 'polygon_geom' has the footprint extents. Footprint extents can be calculated only if gimbal orientation and camera parameters are present.

## Roadmap

We will publish a roadmap in the upcoming months. Stay tuned.

Interested in using DroneDB? [Drop us a note](https://uav4geo.com).
