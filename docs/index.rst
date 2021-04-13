.. _home:

DroneDB - Effortless Aerial Data Management and Sharing
=============================================================================

.. image:: ./_static/logo.svg
    :alt: DroneDB
    :align: right

`DroneDB <https://dronedb.app>`_ is a set of tools and standards designed to make the process of sharing and managing images, orthophotos, digital elevation models and point clouds effortless. Think of it as Dropbox for aerial imagery!

.. image:: https://user-images.githubusercontent.com/1951843/103560016-13848180-4e85-11eb-8177-e711da942a4a.png

At its core, DroneDB can create an index for your data and sync it to a network hub (`Registry <https://github.com/DroneDB/Registry>`_). The network hub provides a user interface and API to interact with the data.

`DroneDB Desktop <https://dronedb.app/dashboard>`_ is a graphical user interface for browsing and sharing aerial data (currently Windows only, but Mac/Linux clients coming soon). It builds a DroneDB index in "real time" while you browse local files on your computer.

`ddb <http://0.0.0.0:8000/download.html#installation>`_ provides a command line client for exploring and sharing aerial data, exposing all of the function of DroneDB. To share images with a Registry, for example you can type:

::

    ddb share *.JPG

DroneDB is also available as a library (``libddb``), with bindings for `Node.JS <https://github.com/DroneDB/DroneDB/tree/master/nodejs>`_ and `.NET <https://github.com/DroneDB/DDB.Bindings>`_. Python bindings are coming soon.

*DroneDB is in early development stages and is targeted at GIS geeks and early adopters. Try it out!*

We welcome contributions! To contribute to the project, please see the `contributing guidelines <https://github.com/uav4geo/DroneDB/blob/master/CONTRIBUTING.md>`_.

.. toctree::
   :maxdepth: 2
   
   about
   download
   commands/index
   tutorials/index
   copyright

