.. _home:

DroneDB - Effortless Aerial Data Management and Sharing
=============================================================================

.. image:: ./_static/logo.svg
    :alt: DroneDB
    :align: right

`DroneDB <https://dronedb.app>`_ is a set of tools and standards for managing and sharing aerial data. Think of it as Dropbox for aerial imagery. It works mainly with images, orthophotos and digital elevation models, for which metadata is extracted and visualized on a map, but it can be used to share all kinds of files.

.. image:: https://user-images.githubusercontent.com/1951843/103560016-13848180-4e85-11eb-8177-e711da942a4a.png

DroneDB can create an index for your data and sync it to a network hub (`Registry <https://github.com/DroneDB/Registry>`_) for highly scalable and flexible data management and sharing across an organization, the internet or for your own personal data-organizing enjoyment.

`DroneDB Desktop <https://dronedb.app/dashboard>`_ is a graphical user interface for browsing and sharing aerial data (currently Windows only, but Mac/Linux clients coming soon).

`ddb <http://0.0.0.0:8000/download.html#installation>`_ provides a command line client for exploring and sharing aerial data, making the task of sharing images with others as simple as typing:

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

