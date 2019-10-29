# DroneDB

![license](https://img.shields.io/github/license/uav4geo/DroneDB) ![commits](https://img.shields.io/github/commit-activity/m/uav4geo/DroneDB) ![languages](https://img.shields.io/github/languages/top/uav4geo/DroneDB)

DroneDB is a toolset for easily managing and sharing aerial datasets. It can index and extract useful information from the EXIF/XMP tags of aerial images to display things like image footprint, flight path and image GPS location. It has timezone-aware date parsing capabilities and a camera sensor database.

![image](https://user-images.githubusercontent.com/1951843/66138811-3dd5f800-e5cd-11e9-816d-a0efa39ccca5.png)

DroneDB is in early development stages and is targeted at GIS developers and early adopters. It is not ready for mainstream use. To contribute to the project, please see the [contributing guidelines](CONTRIBUTING.md).

## Building

Requirements:
 * sqlite3
 * spatialite
 * geographiclib
 * exiv2
 * cmake
 * libgeos
 * gcc-8
 
Then:

```bash
git clone https://github.com/uav4geo/DroneDB ddb
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
