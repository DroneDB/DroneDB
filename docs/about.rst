.. _about:

About
=============================================================================

Background
-----------------------------------------------------------------------------

In the past few years, the use of Unmanned Aerial Vehicles (UAVs) for the acquisition of high-resolution imagery has increased dramatically, due to lower costs of hardware and technology improvements. Going forward, the amount of aerial data acquired for processing and analysis is going to increase and will require new solutions to be developed to handle such data at scale.

An innovative system capable of overcoming the limitations of existing solutions in terms of storing and indexing large amounts of geospatial files, including raw aerial images, elevation models, orthophotos, point clouds and metadata such as reports, user-defined information and any other data associated with files is needed.

DroneDB is such system.

Features
-----------------------------------------------------------------------------

DroneDB can index and extract useful information from aerial data. The most common types of data are raw images captured from a drone. DroneDB can read the EXIF/XMP tags of aerial images to display and calculate things like image footprint, flight path and image GPS location. It has timezone-aware date parsing capabilities, a camera sensor database and a DSM lookup system. DroneDB is smarter than other general-purpose EXIF tools because it focuses primarely on aerial imagery. It can deal with proprietary vendor tags and better parse ambiguous data based on a camera database. It's also much better in the handling of geospatial data.

But it doesn't end here; DroneDB supports other types of data such as those used to represent orthophotos, point clouds, elevation models, plant health maps, ground control point files, flight logs and other files that are common in aerial survey workflows.

All of this aerial data can be indexed into a database, synced to a repository, augmented and managed in a distributed, scalable manner. DroneDB makes aerial data discoverable and queryable.

This vision requires a lot of work, so not all of the features listed here are available today. But there's already a lot you can do with DroneDB. See the list of commands for details.

.. toctree::
   :maxdepth: 2
   :glob: