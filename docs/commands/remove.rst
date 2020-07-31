.. _remove_command:

********************************************************************************
remove
********************************************************************************

Remove files and directories from an index. The filesystem is left unchanged (actual files and directories will not be removed)

::

    $ ddb rm image1.JPG image2.JPG [...] [args] [PATHS]

::

  -d, --directory arg  Working directory (default: .)
  -r, --recursive      Recursively remove subdirectories and files
  -p, --paths arg      Paths to remove from index (files or directories)

.. toctree::
   :maxdepth: 2
   :glob: