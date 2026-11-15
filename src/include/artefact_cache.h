/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef ARTEFACT_CACHE_H
#define ARTEFACT_CACHE_H

#include <string>

#include "fs.h"
#include "gdal_raii.h"

namespace ddb
{
    // How the analyses, the merges and the raster cache read a raster. The host never opens the
    // file of a user with GDAL (raster_artefacts.h): it reads an artefact, a file libddb wrote.
    // The raster of a dataset build already is one (the COG a build wrote, which is what the
    // Registry passes for a GeoRaster); any other raster, a photo or a GeoTIFF that was not
    // built, is rewritten once by the raster helper (the normalize operation) and the analysis
    // reads the copy.
    namespace raster
    {
        // The artefact to read instead of the raster at `path`:
        //   - `path` itself when it is one (isLibddbArtefact), with no request;
        //   - otherwise a GeoTIFF with the bands, values, no-data values, colours, metadata and
        //     blocks of the raster (normalizeRaster), kept in the "normalized" folder of the
        //     artefacts of the cache under the name artefactKey(path).tif, so a changed file is
        //     another copy and a file read again costs nothing. The copy is written by the
        //     helper into a folder of its own request, checked (it must open as a GeoTIFF) and
        //     moved to its name in one rename: callers that ask at the same time for the same
        //     raster each make a copy and the last one stays, readers never see half of a file.
        //     A copy that is used is touched, so cleanupArtefacts removes the ones that nobody
        //     asked for.
        // @throws FSException when `path` cannot be read; RasterOpException when the helper
        // cannot open the raster (Unreadable, Policy); RasterNotReadException when the raster has
        // more pixels than a copy can have (Limit; the message tells to build the dataset first)
        // or the helper fails; the other exceptions of IRasterHelper::normalize; GDALException
        // when the helper wrote a file that is not a GeoTIFF; std::logic_error in the raster
        // helper (gdalopen::userFileOpensEnabled), which never asks for a copy
        fs::path artefactFor(const fs::path &path);

        // Opens the raster of a user at `path` (UTF-8) with GDAL, in the raster helper alone: its
        // operations (a mask, a merge, an alignment...) and the code they reach (a thumbnail detects the
        // sensor of its raster) read the file of the request with openUserRaster, never a copy that
        // another helper would write into the cache. Null (as GDALOpen returns it) when the raster
        // cannot be opened. The host reads its rasters with ddb-tilers (tilers::Raster::openInput, on
        // the copy of artefactFor). @throws std::logic_error in the host
        GDALDatasetUniquePtr openInputRaster(const std::string &path);
    } // namespace raster
} // namespace ddb

#endif // ARTEFACT_CACHE_H
