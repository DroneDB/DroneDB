.. _registry:

Registry
=============================================================================

Registry is a simple, user-friendly aerial data management and
storage application. It features JWT authentication and implements a
full REST API.

Combined with `Hub <https://github.com/DroneDB/Hub>`__, it provides a
simple, fast and reliable platform for hosting and sharing geospatial
images and data. It also allows you to view orthophotos and point clouds
easily and effortlessly directly in the browser.

**Orthophoto and flight path**

.. figure:: https://user-images.githubusercontent.com/7868983/152324827-d16949b8-dd96-4d3a-b5c5-a732e999f070.png
   :alt: orthophoto

**Files with previews**

.. figure:: https://user-images.githubusercontent.com/7868983/152324902-abfe0910-6115-46c5-b561-59bc5a417dda.png
   :alt: files

**Point cloud interactive view**

.. figure:: https://user-images.githubusercontent.com/7868983/152324757-4ee73f71-bf8e-4c72-9910-7073a68daee3.png
   :alt: point-cloud

Getting started
---------------

To get started, you need to install the following applications (if they
are not installed already):

-  `Docker <https://www.docker.com/>`__
-  `Docker-compose <https://docs.docker.com/compose/install/>`__

Single command startup:

**Linux**

.. code:: bash

   mkdir ddb-registry && cd ddb-registry && \ 
     curl -O docker-compose.yml https://raw.githubusercontent.com/DroneDB/Registry/master/docker/testing/docker-compose.yml && \
     curl -O appsettings-testing.json https://raw.githubusercontent.com/DroneDB/Registry/master/docker/testing/appsettings-testing.json && \
     curl -O initialize.sql https://raw.githubusercontent.com/DroneDB/Registry/master/docker/testing/initialize.sql && \
     docker-compose up

**Windows**

.. code:: powershell

   mkdir ddb-registry; cd ddb-registry; `
   curl -O docker-compose.yml https://raw.githubusercontent.com/DroneDB/Registry/master/docker/testing/docker-compose.yml; `
   curl -O appsettings-testing.json https://raw.githubusercontent.com/DroneDB/Registry/master/docker/testing/appsettings-testing.json; `
   curl -O initialize.sql https://raw.githubusercontent.com/DroneDB/Registry/master/docker/testing/initialize.sql; `
   docker-compose up -d

This command will start a new stack composed by

-  MariaDB database
-  PHPMyAdmin, exposed on port `8080 <http://localhost:8080>`__
-  Registry, exposed on port `5000 <http://localhost:5000>`__

Default username and password are ``admin`` and ``password``. After
logging in you can check the health of the application by visiting
`http://localhost:5000/status <http://localhost:5000/health>`__.

Registry supports Swagger API documentation on
`http://localhost:5000/swagger/ <http://localhost:5000/swagger/>`__ and
Hangfire as task runner on
`http://localhost:5000/hangfire/ <http://localhost:5000/hangfire/>`__.

   **NOTE:** This configuration is for local testing only: **DO NOT USE
   IT IN PRODUCTION**. If you want to use the application in production
   check the following section.

Running in production
---------------------

You will need `Git <https://git-scm.com/downloads>`__. Clone the repo
and initialize submodules:

.. code:: bash

   git clone https://github.com/DroneDB/Registry
   cd Registry
   git submodule update --init --recursive

And then run the following commands:

**Linux**

.. code:: bash

   cd docker/production
   chmod +x run.sh
   ./run.sh

**Windows**

.. code:: bash

   cd docker/production
   run.bat

Check that everything is running smoothly:

.. code:: bash

   docker-compose ps
   docker-compose logs -f

When all the containers are running, you can then open
http://localhost:5000 in your browser, use ``admin:password`` as default
credentials.

You can stop the application by issuing:

.. code:: bash

   docker-compose down

The ``run.sh`` / ``run.bat`` script will create the default
``appsettings.json`` file, the database initialization script and start
the Docker containers. It is possible to customize the startup settings
by creating a ``.env`` file in the same folder. Here it is an example:

**Linux (quotes are important)**

.. code:: bash

   MYSQL_ROOT_PASSWORD="default-root-password"
   MYSQL_PASSWORD="default-mysql-password"
   REGISTRY_ADMIN_MAIL="test@test.it"
   REGISTRY_ADMIN_PASSWORD="password"
   REGISTRY_SECRET="longandrandomsecrettobegeneratedusingcryptographicallystrongrandomnumbergenerator"
   EXTERNAL_URL=""
   CONTROL_SWITCH='$controlSwitch'

**Windows (values without quotes)**

.. code:: batch

   MYSQL_ROOT_PASSWORD=default-root-password
   MYSQL_PASSWORD=default-mysql-password"
   REGISTRY_ADMIN_MAIL=test@test.it
   REGISTRY_ADMIN_PASSWORD=password
   REGISTRY_SECRET=longandrandomsecrettobegeneratedusingcryptographicallystrongrandomnumbergenerator
   EXTERNAL_URL=
   CONTROL_SWITCH=$controlSwitch

If you want to reduce the log verbosity, you can change
``"Information"`` to ``"Warning"`` in ``appsettings.json``:

.. code:: json

       "LevelSwitches": {
           "$CONTROL_SWITCH": "Warning"
       },

then run

::

   docker-compose restart registry

Standalone installation with docker (only for testing)
------------------------------------------------------

The following steps start a new instance of ``registry`` with the
default configuration and ``SQLite`` as backend database. They work both
on linux and windows (powershell):

.. code:: bash

   wget -O appsettings.json https://raw.githubusercontent.com/DroneDB/Registry/master/Registry.Web/appsettings-default.json

   docker run -it --rm -p 5000:5000 -v ${PWD}/registry-data:/Registry/App_Data -v ${PWD}/appsettings.json:/Registry/appsettings.json dronedb/registry:latest

..

   ``Registry`` can use ``SQLite``, ``MySQL`` (``MariaDB``) or
   ``SQL Server`` as a database. Nevertheless, the application is
   primarily designed to be used with ``MariaDB``. There are no
   migration scripts for the other databases, so you have to manually
   upgrade the schema between versions. The above steps are for test
   only, and should not be used in production.

Build Docker image
------------------

If you want to build the image from scratch, you can use the following
commands:

.. code:: bash

   git clone https://github.com/DroneDB/Registry
   cd Registry
   git submodule update --init --recursive
   docker build . -t dronedb/registry

Notes: - ``ddb`` commands must use the ``127.0.0.1`` syntax, not
``localhost``

Native installation
-------------------

**Building**

``Registry`` is written in C# on .NET Core 6 platform and runs natively
on both Linux and Windows. To install the latest .NET SDK see the
`official download
page <https://dotnet.microsoft.com/en-us/download/dotnet/6.0>`__. Before
building registry ensure you have ``ddblib`` in your path, if not
download the `latest
release <https://github.com/DroneDB/DroneDB/releases>`__ and add it to
``PATH``.

Clone the repository:

.. code:: bash

   git clone https://github.com/DroneDB/Registry
   cd Registry
   git submodule update --init --recursive

Build the solution from the command line:

.. code:: bash

   dotnet build

Run the tests to make sure the project is working correctly:

.. code:: bash

   dotnet test

Then build the Hub interface (need `NodeJS
14+ <https://nodejs.org/download/release/v14.18.3/>`__):

.. code:: bash

   cd Registry.Web/ClientApp
   npm install -g webpack@4
   npm install
   webpack

**Running**

On the first start ``Registry`` will create ``appsettings.json`` file
with default values. Feel free to modify it to your needs following the
`documentation <https://docs.dronedb.app/registry>`__.

.. code:: bash

   dotnet run --project Registry.Web

It will start a web server listening on two endpoints:
``https://localhost:5001`` and ``http://localhost:5000``. You can change
the endpoints using the ``urls`` parameter:

.. code:: bash

   dotnet run --project Registry.Web --urls="http://0.0.0.0:6000;https://0.0.0.0:6001"

Project architecture
--------------------

.. figure:: https://user-images.githubusercontent.com/7868983/151846022-891685f7-ef47-4b93-8199-d4ac4e788c5d.png
   :alt: dronedb-registry-architecture

   dronedb-registry-architecture


Configuration Reference
-----------------------

The configuration file is ``appsettings.json``, if not present it will be created with default values (``appsettings-default.json``).

AuthCookieName
   The name of the authorization cookie.

   .. note::

         The default value is ``jwtToken``.

AuthProvider
   The authentication provider, supported values:
      - ``Sqlite``: SQLite database
      - ``Mysql``: MySQL or MariaDB, (`compatibility <https://github.com/PomeloFoundation/Pomelo.EntityFrameworkCore.MySql#supported-database-servers-and-versions>`__)
      - ``Mssql``: Microsoft SQL Server

   .. note::

         The default value is ``Sqlite``
  
   The ``IdentityConnection`` connection string should be changed accordingly

BatchTokenLength
   The length of the token generated in the share endpoint.

   .. note::

         The default value is ``32``.

CachePath
   The path to the cache folder. This is used to store the generated tiles and thumbnails.

   .. note::

         The default value is ``./App_Data/cache``.

CacheProvider
   The additional cache provider, supported values:
   
      - ``InMemory``: In-memory cache provider. Example value:
         
         .. code:: json

               { "type": "InMemory" }

      - ``Redis``: Redis cache provider. Example value: 
        
         .. code:: json

               {
                  "type": "Redis", 
                  "settings": { 
                     "InstanceAddress": "localhost:5002", 
                     "InstanceName": "registry" 
                  } 
               }
   .. note::

         The default value is ``null`` (``InMemory`` is used).

ClearCacheInterval
   The interval to clear the file cache (TimeSpan).

   .. note::

         The default value is ``01:00:00`` (1 hour).

DefaultAdmin
   The default admin user.

   .. note::

         The default value is:

         .. code:: json

            {
               "Email": "admin@example.com",
               "UserName": "admin",
               "Password": "password"
            },

EnableStorageLimiter
   Enable the storage limiter. Registry will limit the storage usage of the user based on its metadata (``maxStorageMB`` key).   

   .. note::

         The default value is ``false``.

ExternalAuthUrl
   The URL of the external authentication provider.

   .. note::

         The default value is ``null``.

ExternalUrlOverride
   The external URL of Registry. This is used when the application is behind a reverse proxy.

   .. note::

         The default value is ``null``.

HangfireProvider
   The Hangfire provider, supported values:

      - ``InMemory``: In-memory provider. Example value:  
      - ``Mysql``: MySQL or MariaDB

   .. note::

         The default value is ``InMemory``
  
   The ``HangfireConnection`` connection string should be changed accordingly

MaxRequestBodySize
   The maximum request body size. It sets the ``MultipartBodyLengthLimit`` of the kestrel ``FormOptions``.

   .. note::

         The default value is ``null`` (default).

RandomDatasetNameLength
   The length of the random dataset name, generated when calling the share endpoint.

   .. note::

      The default value is ``16``.

RegistryProvider
   The Registry database provider, supported values:
      - ``Sqlite``: SQLite database
      - ``Mysql``: MySQL or MariaDB, (`compatibility <https://github.com/PomeloFoundation/Pomelo.EntityFrameworkCore.MySql#supported-database-servers-and-versions>`__)
      - ``Mssql``: Microsoft SQL Server

   .. note::

         The default value is ``Sqlite``
  
   The ``RegistryConnection`` connection string should be changed accordingly

RevokedTokens
   The list of revoked JWT tokens.

Secret
  The secret used as key to generate the JWT tokens.

StoragePath
   The path to the storage folder. This is used to store all the uploaded datasets.

   .. note::

         The default value is ``./App_Data/datasets``

ThumbnailsCacheExpiration
   The expiration time of the thumbnails cache (TimeSpan).

   .. note::

         The default value is ``00:30:00`` (30 minutes).

TilesCacheExpiration
   The expiration time of the tiles cache (TimeSpan).

   .. note::

         The default value is ``00:30:00`` (30 minutes).
         
TokenExpirationInDays
   The number of days after which the JWT tokens will expire.

   .. note::

         The default value is 30 days.

UploadBatchTimeout
   The timeout for the share upload endpoint. It is the maximum time allowed between the uploads.

   .. note::

         The default value is ``01:00:00`` (1 hour).
         
WorkerThreads
   The number of worker threads used by the application.

   .. note::

      The default value is ``0`` (default)

Note
----

DroneDB Registry is under development and is targeted at GIS developers
and tech enthusiasts. To contribute to the project, please see the
`contributing guidelines <CONTRIBUTING.md>`__.

.. |commits| image:: https://img.shields.io/github/commit-activity/m/DroneDB/registry
.. |languages| image:: https://img.shields.io/github/languages/top/DroneDB/registry
.. |.NET Core| image:: https://github.com/DroneDB/Registry/workflows/.NET%20Core/badge.svg?branch=master


.. toctree::
   :maxdepth: 2
   :glob: