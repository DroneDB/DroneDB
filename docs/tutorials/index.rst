.. _tutorials:

******************************************************************************
Tutorials
******************************************************************************

Indexing Files
-----------------------------------------------------------------------------

To create a new index:

::

    ddb init

To add images to an index:

::

    ddb add images/
    ddb add images/DJI_0018.JPG
    ddb add images/*.JPG

To remove images from an index:

::

    ddb rm images/DJI_0018.JPG

To sync an index after filesystem changes:

::
    
    ddb sync

Projecting an image onto a map
-----------------------------------------------------------------------------

.. image:: https://user-images.githubusercontent.com/1951843/68077866-001de800-fda2-11e9-895f-b5840d9d047d.png

You can project images onto a map. This does not require an index and can be done on-the-fly.

::

    ddb geoproj images/DJI_0018.JPG -o projected/

Browsing data with QGIS
-----------------------------------------------------------------------------

You can explore the indexed data with a program such as QGIS. The index creates a `.ddb/dbase.sqlite` file when running the `ddb init` command. This is a SpatiaLite database and can be imported by adding a new SpatiaLite layer.


Viewing Flight Path LineString
-----------------------------------------------------------------------------

.. image:: https://user-images.githubusercontent.com/1951843/66138811-3dd5f800-e5cd-11e9-816d-a0efa39ccca5.png

Create a new query similar to:

::

    SELECT AsGeoJSON(MakeLine(point_geom)) FROM (SELECT point_geom, json_extract(meta, '$.captureTime') as captureTime FROM entries ORDER by captureTime ASC);


The 'point_geom' column contains the GPS locations, while 'polygon_geom' has the footprint extents. Footprint extents can be calculated only if gimbal orientation and camera parameters are present.


Extracting GPS locations/footprints to GeoJSON
-----------------------------------------------------------------------------

You can quickly extract the locations of images to GeoJSON via:

::

    ddb info *.JPG -f geojson -o gps.geojson

Or for image footprints:

::

    ddb info *.JPG -f geojson --geometry polygon -o footprint.geojson


.. toctree::
   :maxdepth: 2
   :glob:
