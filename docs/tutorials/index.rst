.. _tutorials:

******************************************************************************
Tutorials
******************************************************************************

You can do all sort operations with DroneDB. We recommend to check out the list of `Commands </commands/index.html>`_. Here are some (non-exhaustive) examples:

Sharing datasets
-----------------------------------------------------------------------------

From a directory with images, simply type:

::

    ddb share *.JPG

By default the images are shared with DroneDB's Hub. You will need to `register an account <https://dronedb.app>`_ to get a username and password. But you can also self-host your own `Registry <https://github.com/DroneDB/Registry>`_.

You can select a different Registry by typing:

::

    ddb share *.JPG -s http://localhost:5000
    --> https://localhost:5000/r/admin/193514313aba4949ab5578b28ba1dd5b

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

Using the Web UI provided by Registry is the easiest way to make changes.

In DroneDB you can also ``clone`` (download) an existing dataset from a Registry for offline use, make modifications, then sync back your changes.

Let's use this dataset: https://hub.dronedb.app/r/pierotofy/brighton-beach

You can clone it via:

::

    ddb clone pierotofy/brighton-beach

Let's add a ``README.md`` file that describes the dataset. Create a ``README.md`` file using `Markdown <https://www.markdownguide.org/cheat-sheet/>`_ syntax and save it in the ``brighton-beach`` directory. Afterwards:

::

    cd brighton-beach/
    ddb add README.md

Great! We are now ready to push the changes.

::

    ddb push

Uuups! This will trigger an error, since we don't have permission to make modifications to this dataset (it belongs to ``pierotofy``). Let's make our own copy to a different Registry server and user:

::

    ddb tag http://localhost:5000/admin/brighton-copy
    ddb push


Adding a README/LICENSE
-----------------------------------------------------------------------------

If a dataset contains a ``README.md`` and/or a ``LICENSE.md`` file, they will be picked up and displayed accordingly on the Registry:

::

    ddb share README.md LICENSE.md

The files can contain valid `Markdown <https://www.markdownguide.org/basic-syntax/>`_ syntax, including images, links, tables, etc.


Metadata Entries
-----------------------------------------------------------------------------

DroneDB supports the addition of metadata to any file or directory within the index. This can be used to store information of any kind in JSON format:

::

    ddb meta set pilot '{"name": "John Smith"}'
    ddb meta get pilot --format json
    --> {"data":{"name":"John S."},"id":"ff4f0f26-8741-4423-bde5-b445750937bb","mtime":1640985850}

::

    ddb add photo.JPG
    ddb meta add comments '{"text": "Nice one!", "author": "John S."}' -p photo.JPG
    ddb meta get comments -p photo.JPG --format json
    --> [{"data":{"author":"John S.","text":"Nice one!"},"id":"550d0b5c-108b-4996-b7e8-467b4cb87937","mtime":1640986217}]

Singular and plural metadata keys are supported. Plural keys (ending with ``s``) are treated as lists, whereas singular keys are objects. 

Metadata entries are synced on push/pull and people working on the same dataset while offline can later sync back online without conflicts.

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
