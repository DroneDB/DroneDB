/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef GDAL_OPEN_POLICY_H
#define GDAL_OPEN_POLICY_H

#include <string>
#include <vector>

#include "fs.h"
#include "gdal_raii.h"

// How libddb opens a file a user supplied.
//
// GDAL identifies a file by its content, whatever its name says, and ships drivers that read
// other files than the one it was given: a VRT is a list of paths and URLs to read, an OGR VRT
// the same for vector data, and its Python pixel functions run code. A file named .tif that is
// really a VRT would make an upload publish any raster the process can read, or make it
// request a URL. So a user file is opened with the drivers its extension allows, never
// with all of them: .tif opens as a TIFF, never as a VRT, netCDF or GeoPackage.
//
// Every open of a file that may come from a user goes through openUserRaster or
// openUserVector, and only the raster helper (gdal-util) makes them: a process that did not
// declare itself that program (enableUserFileOpens) gets a std::logic_error, so a host (the
// Registry, ddbcmd, the tests) cannot parse a file of a user with GDAL by mistake. The files
// libddb wrote itself are not user files and open through raster_artefacts.h. While a user
// file opens, GDAL makes no HTTP request and its network file systems (/vsicurl/, ...) open no
// other file than the one being opened, whatever URL the file names.
namespace ddb
{
    namespace gdalopen
    {
        // The GDAL drivers that may open a raster / vector file with this extension (the path
        // or its last element: the extension is read case-insensitively), limited to those
        // present in the linked GDAL. Empty when the extension is not one libddb opens.
        std::vector<std::string> rasterDrivers(const std::string &path);
        std::vector<std::string> vectorDrivers(const std::string &path);

        // Whether a raster with this extension is a VRT, whose sources are checked before it opens
        bool isVrtPath(const std::string &path);

        // A VRT that is safe to open, or the reason it is not
        struct VrtVerdict
        {
            bool ok = false;
            std::string reason;
        };

        // Checks the VRT at path before it is opened. It must be a plain VRT dataset of sourced
        // or derived bands that holds only the elements and attributes such a dataset reads in
        // place (georeferencing, metadata, band properties, simple/complex/averaged/kernel
        // sources, a pixel function compiled into GDAL): anything else is refused, among it
        // warped, pansharpened, processed and multidimensional datasets, raw bands, transformers
        // and their DEMs or geolocation arrays, open options, inline VRTs, Python pixel functions
        // and metadata in the domains whose items name a file. Every file it names (the
        // SourceFilename of a source or of the overviews of a band) must be a relative path
        // (relativeToVRT="1"), not a /vsi* path or a URL, and resolve, symlinks followed, to a
        // file inside root; the file is identified with the drivers its own extension allows (a
        // .tif that is really a VRT is refused) and, when it is a VRT, checked the same way, down
        // to a nesting of 2. Each is then opened with those drivers and refused when its metadata
        // names another dataset (see openUserRaster).
        VrtVerdict validateVrt(const fs::path &path, const fs::path &root);

        // Checks the files GDAL opens next to the raster at path with all its drivers, not only
        // those of the raster's extension: the external overviews (.ovr) and mask (.msk) must be
        // GeoTIFFs, an ERDAS .aux an HFA file and an attribute table (.vat.dbf) a dBase table,
        // as GDAL writes them, none a symbolic link nor naming another dataset in its metadata,
        // and the sidecars of a raster sidecar are checked in turn. A VRT named .tif.ovr is
        // refused.
        // @throws std::logic_error in a process that is not the raster helper
        VrtVerdict validateSidecars(const std::string &path);

        // Whether a metadata domain lists datasets by file name or holds the name of the dataset
        // to open in one of its items: the domains a copy of a raster leaves behind. They are
        // the domains of the items openUserRaster refuses (OVERVIEWS, GEOLOCATION) and the ones
        // GDAL fills with the names of the datasets it can open from the file (SUBDATASETS,
        // DERIVED_SUBDATASETS).
        bool isFileNamingDomain(const std::string &domain);

        // Declares that this process is the one that parses user files: the raster helper
        // (gdal-util) calls it once, at start. Nothing else in the library may call it, so that
        // a host process (the Registry, ddbcmd, the tests) can be told apart from the helper;
        // scripts/check-gdal-opens.py fails when anything else does. A test that needs to open
        // a user file does it in a child process that calls it (tests/user_open_child.h).
        void enableUserFileOpens();

        // Whether enableUserFileOpens() was called in this process
        bool userFileOpensEnabled();

        // Throws std::logic_error naming `what` unless enableUserFileOpens() was called: the
        // first line of every function that opens or identifies a file of a user
        void requireUserFileOpens(const char *what);

        // While one is alive, GDAL makes no HTTP request on this thread (the requests it
        // would make fail) and its network file systems (/vsicurl/, /vsis3/, ...) open no
        // other file than `path`: the files that GDAL opens may name any URL (the schemas a
        // GML includes, the sources of a VRT), and the opens of libddb must not follow them.
        // Every open of a file by GDAL in libddb (user files and artefacts) takes one.
        class NoNetworkWhileOpening
        {
          public:
            explicit NoNetworkWhileOpening(const std::string &path);
            ~NoNetworkWhileOpening();
            NoNetworkWhileOpening(const NoNetworkWhileOpening &) = delete;
            NoNetworkWhileOpening &operator=(const NoNetworkWhileOpening &) = delete;

          private:
            bool pushed_;
            ScopedThreadConfigOption onlyThisFile_;
        };

        // Whether an opened dataset names no other dataset in its metadata (the OVERVIEWS and
        // GEOLOCATION domains, see openUserRaster): verdict.reason says which item otherwise.
        // The check of every raster libddb opens, a copy that the helper wrote included, since
        // the metadata of a copy comes from the file of the user.
        VrtVerdict validateMetadataFiles(GDALDatasetH dataset);
    } // namespace gdalopen

    // Opens a raster file of a user read-only with the drivers its extension allows.
    // @throws std::logic_error in a process that is not the raster helper. Null (as
    // GDALOpen returns it) when no allowed driver opens it, the extension is not one libddb
    // reads, a sidecar next to it is not safe (validateSidecars), it is a VRT whose sources
    // are not safe (those inside the folder of the VRT are) or its metadata names another
    // dataset GDAL would open later with all its drivers, any path or URL: the file of its
    // overviews (OVERVIEW_FILE in the OVERVIEWS domain, which a TIFF or JPEG 2000 carries) or of
    // its geolocation arrays (X_DATASET, Y_DATASET in the GEOLOCATION domain).
    GDALDatasetUniquePtr openUserRaster(const std::string &path);

    // The driver that identifies a raster file of a user among those its extension allows, null
    // when none does (nothing is opened)
    // @throws std::logic_error in a process that is not the raster helper
    GDALDriverH identifyUserRaster(const std::string &path);

    // Opens a vector file of a user read-only with the drivers its extension allows; null when
    // none opens it (an OGR VRT named .geojson is not opened)
    // @throws std::logic_error in a process that is not the raster helper
    GDALDatasetUniquePtr openUserVector(const std::string &path);
} // namespace ddb

#endif
