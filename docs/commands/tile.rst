.. _tile_command:

********************************************************************************
tile
********************************************************************************

Generate tiles for GeoTIFFs

::

    $ ddb tile geo.tif [output directory] [args]

::

  -i, --input arg   File to tile
  -o, --output arg  Output directory where to store tiles (default: {filename}_tiles/)
  -f, --format arg  Output format (text|json) (default: text)
  -z, arg           Zoom levels, either a single zoom level "N" or a range "min-max" or "auto" to generate all zoom levels (default: auto)
  -x, arg           Generate a single tile with the specified coordinate (XYZ, unless --tms is used). Must be used with -y (default: auto)
  -y, arg           Generate a single tile with the specified coordinate (XYZ, unless --tms is used). Must be used with -x (default: auto)
  -s, --size arg    Tile size (default: 256)
      --tms         Generate TMS tiles instead of XYZ

.. toctree::
   :maxdepth: 2
   :glob: