.. _share_command:

********************************************************************************
share
********************************************************************************

::

    Share files and folders to a registry
    
    Usage:
      ddb share *.JPG [args]
    
      -i, --input arg     Files and directories to share
      -r, --recursive     Recursively share subdirectories
      -t, --tag arg       Tag to use (organization/dataset or
                          server[:port]/organization/dataset) (default:
                          hub.dronedb.app/<username>/<uuid>)
      -p, --password arg  Optional password to protect dataset (default: )
      -s, --server arg    Registry server to share dataset with (alias of: -t
                          <server>//)
      -q, --quiet         Do not display progress
      -h, --help          Print help
          --debug         Show debug output

.. toctree::
    :maxdepth: 2
    :glob:
