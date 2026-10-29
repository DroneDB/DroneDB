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
// openUserVector. Files libddb wrote itself (its COGs, VRTs and overviews) are not user files
// and open directly. While a user file opens, GDAL makes no HTTP request and its network file
// systems (/vsicurl/, ...) open no other file than the one being opened, whatever URL the
// file names.
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
        VrtVerdict validateSidecars(const std::string &path);

        // Declares that this process is the one that parses user files: the raster helper
        // (gdal-util) calls it once, at start. Nothing else in the library may call it, so that
        // a host process (the Registry, ddbcmd, the tests) can be told apart from the helper.
        void enableUserFileOpens();

        // Whether enableUserFileOpens() was called in this process
        bool userFileOpensEnabled();
    } // namespace gdalopen

    // Opens a raster file of a user read-only with the drivers its extension allows. Null (as
    // GDALOpen returns it) when no allowed driver opens it, the extension is not one libddb
    // reads, a sidecar next to it is not safe (validateSidecars), it is a VRT whose sources
    // are not safe (those inside the folder of the VRT are) or its metadata names another
    // dataset GDAL would open later with all its drivers, any path or URL: the file of its
    // overviews (OVERVIEW_FILE in the OVERVIEWS domain, which a TIFF or JPEG 2000 carries) or of
    // its geolocation arrays (X_DATASET, Y_DATASET in the GEOLOCATION domain).
    GDALDatasetUniquePtr openUserRaster(const std::string &path);

    // The driver that identifies a raster file of a user among those its extension allows, null
    // when none does (nothing is opened)
    GDALDriverH identifyUserRaster(const std::string &path);

    // Opens a vector file of a user read-only with the drivers its extension allows; null when
    // none opens it (an OGR VRT named .geojson is not opened)
    GDALDatasetUniquePtr openUserVector(const std::string &path);
} // namespace ddb

#endif
