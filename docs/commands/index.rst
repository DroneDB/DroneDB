.. _commands:

******************************************************************************
Commands
******************************************************************************

DroneDB exposes several commands via the ``ddb`` command line application.

::

	add - Add files and directories to an index.
	build - Initialize and build an index. Shorthand for running an init,add,commit command sequence.
	geoproj - Project images to georeferenced rasters
	info - Retrieve information about files and directories
	init - Initialize an index. If a directory is not specified, initializes the index in the current directory
	remove - Remove files and directories from an index. The filesystem is left unchanged (actual files and directories will not be removed)
	set - Modify EXIF values in files.
	sync - Sync files and directories in the index with changes from the filesystem
	thumbs - Generate thumbnails for images and rasters
	tile - Generate tiles for GeoTIFFs
	system - Manage ddb

.. toctree::
   :maxdepth: 2
   :glob:

   *