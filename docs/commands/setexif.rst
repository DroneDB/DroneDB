.. _setexif_command:

********************************************************************************
setexif
********************************************************************************

::

    Modify EXIF values in files.
    
    Usage:
      ddb setexif *.JPG [args]
    
      -i, --input arg    File(s) to modify
          --gps-alt arg  Set GPS Altitude (decimal degrees)
          --gps-lon arg  Set GPS Longitude (decimal degrees)
          --gps-lat arg  Set GPS Latitude (decimal degrees)
          --gps arg      Set GPS Latitude,Longitude,Altitude (decimal degrees,
                         comma separated)
      -h, --help         Print help
          --debug        Show debug output

.. toctree::
    :maxdepth: 2
    :glob:
