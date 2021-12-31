.. _home:

DroneDB - Effortless Aerial Data Management and Sharing
=============================================================================

.. image:: ./_static/logo.svg
    :alt: DroneDB
    :align: right

`DroneDB <https://dronedb.app>`_ is free and open source software for aerial data storage. It provides a convenient location to store images, orthophotos, digital elevation models, point clouds and any other file.

.. image:: https://user-images.githubusercontent.com/1951843/103560016-13848180-4e85-11eb-8177-e711da942a4a.png

Brighton Beach: `Example Dataset <https://hub.dronedb.app/r/pierotofy/brighton-beach>`_

DroneDB creates an index of your aerial files and exposes them via a network hub (`Registry <https://github.com/DroneDB/Registry>`_). Registry is free and open source software and provides a user interface and REST API. The DroneDB developers manage a cloud instance of Registry at https://hub.dronedb.app for people to use. You will need to `register an account <https://dronedb.app/register>`_.

The index parses the information of your aerial data automatically:

 * Image locations and flight paths are plotted on the map
 * GeoTIFFs/LAS/LAZ are displayed on the map
 * LAS/LAZ/PLY are displayed on the 3D view
 * Clicking an image location on the map displays it (via geoprojection)
 * Various properties are made available

The data is completely portable and syncable between different instances of Registry. For example one can make a copy a dataset from one Registry to another:

::

    ddb clone https://hub.dronedb.app/pierotofy/brighton-beach
    cd brighton-beach
    ddb tag http://localhost:5000/admin/my-copy
    ddb push


`DroneDB Desktop <https://dronedb.app/dashboard>`_ is a graphical user interface for browsing and sharing aerial data. It builds a DroneDB index in "real time" while you browse local files on your computer.

`ddb </download.html#client>`_ is a command line client. To share images with a Registry, for example you can type:

::

    ddb share *.JPG

We welcome contributions! To contribute to the project, please see the `contributing guidelines <https://github.com/DroneDB/DroneDB/blob/master/CONTRIBUTING.md>`_.

.. toctree::
   :maxdepth: 2
   
   download
   tutorials/index
   commands/index
   copyright

