.. _info_command:

********************************************************************************
info
********************************************************************************

::

    Retrieve information about files and directories
    
    Usage:
      ddb info *.JPG [args]
    
      -i, --input arg     File(s) to examine
      -o, --output arg    Output file to write results to (default: stdout)
      -f, --format arg    Output format (text|json|geojson) (default: text)
      -r, --recursive     Recursively search in subdirectories
      -d, --depth arg     Max recursion depth (default: 0)
          --geometry arg  Geometry to output (for geojson format only)
                          (auto|point|polygon) (default: auto)
          --with-hash     Compute SHA256 hashes
      -h, --help          Print help
          --debug         Show debug output

.. toctree::
    :maxdepth: 2
    :glob:
