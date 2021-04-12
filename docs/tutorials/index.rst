.. _tutorials:

******************************************************************************
Tutorials
******************************************************************************

You can do all sort of useful tasks related to aerial data management with DroneDB. Here are some examples:

Sharing datasets
-----------------------------------------------------------------------------

From a directory with images, simply type:

::

    ddb share *.JPG

By default the images are shared with DroneDB's Hub. You will need to `register an account <https://dronedb.app>`_ to get a username and password. But you can also run your own Registry, on your own infrastructure. We have two implementations available:

 - https://github.com/DroneDB/Registry
 - https://github.com/DroneDB/MiniReg

You can then select a different Registry by typing:

::

    ddb share *.JPG -s http://localhost:5000


At the end of the process, you will get a URL back:

::

    https://testhub1.dronedb.app/r/pierotofy/193514313aba4949ab5578b28ba1dd5b

Because we didn't set a ``tag``, DroneDB generated a random one for you. You can change the tag by visiting the URL. You can also explicitly set a tag, like so:

::

    ddb share *.JPG -t pierotofy/brighton

Or if you're running your own Registry:

::

    ddb share *.JPG -t http://localhost:5000/admin/brighton

Tags are defined as:

``[server]/organization/dataset``

With the server component being optional.

Editing datasets
-----------------------------------------------------------------------------

In DroneDB you can ``clone`` (download, or checkout) an existing dataset from a Registry, make modifications locally (offline), then sync back your changes.

Let's use this dataset: https://testhub1.dronedb.app/r/pierotofy/brighton

You can clone it via:

::

    ddb clone testhub1.dronedb.app/pierotofy/brighton

Let's add a ``README.md`` file that describes the dataset:

::

    cd brighton/
    ddb add README.md

Great! We are now ready to push the changes.

(Currently this functionality is being built, so it's probably not going to work):

::

    ddb push

Adding a README/LICENSE
-----------------------------------------------------------------------------

If a dataset contains a ``README.md`` and/or a ``LICENSE.md`` file, they will be picked up and displayed accordingly on the Registry:

::

    ddb share README.md LICENSE.md

The files can contain valid `Markdown <https://www.markdownguide.org/basic-syntax/>`_ syntax, including images, links, tables, etc.

https://testhub1.dronedb.app/r/pierotofy/license

Projecting an image onto a map
-----------------------------------------------------------------------------

.. image:: https://user-images.githubusercontent.com/1951843/68077866-001de800-fda2-11e9-895f-b5840d9d047d.png

Have you ever wondered how a picture looks like on a map? With DroneDB you can project images onto a map. This does not require an index and can be done on-the-fly.

::

    ddb geoproj images/DJI_0018.JPG -o projected/


Setting GPS location of photos
-----------------------------------------------------------------------------

Sometimes images lack the proper GPS tags. You can use DroneDB to set them:

::

    ddb setexif DJI_0018.JPG --gps 46.84260708333,-91.99455988889,198.31

Creating static tiles (XYZ/TMS)
-----------------------------------------------------------------------------

DroneDB can create static tiles for GeoTIFFs and drone images (if they can be geoprojected). It's similar to the `gdal2tiles.py <https://gdal.org/programs/gdal2tiles.html>`_ program, but it's a bit faster and can handle drone images as well! You can use these tiles in applications such as Leaflet or OpenLayers to display them on the web.

::

    ddb tile DJI_0018.JPG output_tiles/

Browsing data with QGIS
-----------------------------------------------------------------------------

You can explore the a DroneDB dataset with a program such as QGIS too. Every DroneDB dataset contains `.ddb/dbase.sqlite` file. This is a SpatiaLite database and can be imported by adding a new SpatiaLite layer in QGIS.


Extracting GPS locations/footprints to GeoJSON
-----------------------------------------------------------------------------

You can quickly extract the locations of images to GeoJSON via:

::

    ddb info *.JPG -f geojson -o gps.geojson

Or for image footprints:

::

    ddb info *.JPG -f geojson --geometry polygon -o footprint.geojson

This works with orthophotos, elevation models and point clouds as well!

::

    ddb info point_cloud.laz -f geojson --geometry polygon -o footprint.geojson

.. toctree::
   :maxdepth: 2
   :glob:
