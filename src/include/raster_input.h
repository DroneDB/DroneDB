/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef RASTER_INPUT_H
#define RASTER_INPUT_H

#include <cstdint>
#include <memory>
#include <string>

#include "fs.h"
#include "gdal_raii.h"
#include "raster_artefacts.h"

namespace ddb
{
    // How the analyses, the exports and the tilers of the host read a raster. The host opens no file
    // of a user with GDAL (raster_artefacts.h), and keeps no copy of one: it reads with ddb-tilers
    //   - an artefact as it is (the COG a build wrote, which is what the Registry passes for a built
    //     GeoRaster);
    //   - a file of a user that ddb-tilers reads whole (isHostReadable) as it is;
    //   - any other raster (a JPEG 2000, a photo, a TIFF with a compression ddb-tilers does not
    //     decode) through a copy the raster helper writes into a request folder for this reader
    //     alone, removed when the reader closes.
    namespace raster
    {
        // Whether the host may read the raster of a user at `path` with ddb-tilers itself: ddb-tilers
        // opened it in the raster helper (the `validate` operation, confined) and decoded every block
        // of every level, the masks with them. The answer is kept for the version of the file
        // (fileversion.h) while this process runs, so a file is validated once; a changed file is
        // validated again. The statistics the validation counted on the way are adopted
        // (RasterStatsProvider::adopt), so the first value info of the file reads no pixel again.
        // A file that does not start as a TIFF or a BigTIFF is not host readable, and the helper is not
        // asked; nor is a file the helper fails on (it crashed, it hit a limit). Only in the host.
        // @throws CanceledException, HelperUnavailableException, FSException (the file cannot be read)
        bool isHostReadable(const fs::path &path);

        // Forgets every answer of isHostReadable (tests)
        void forgetHostReadable();

        // The file the host reads for the raster at `path`, and the request folder that holds it when
        // it is a copy (null otherwise), which the reader keeps until it closes the file
        struct InputFile
        {
            fs::path file;
            std::unique_ptr<RequestFolder> request;
        };

        // The file to read for the raster at `path`: `path` itself when it is an artefact
        // (isLibddbArtefact) or host readable (isHostReadable); otherwise a GeoTIFF with the bands,
        // values, no-data values, colours, metadata and blocks of the raster that the helper writes
        // (normalize) into a new request folder, checked (it must open as a GeoTIFF). Nothing of it is
        // kept once the folder goes.
        // @throws FSException when `path` cannot be read; RasterOpException when the helper cannot
        // open the raster (Unreadable, Policy); RasterNotReadException when the raster has more
        // pixels than a copy can have (Limit; the message tells to build the dataset first) or the
        // helper fails; the other exceptions of IRasterHelper::normalize; GDALException when the
        // helper wrote a file that is not a GeoTIFF; std::logic_error in the raster helper
        // (gdalopen::userFileOpensEnabled), which reads the file of its request itself
        InputFile inputFileFor(const fs::path &path);

        // How many copies inputFileFor made in this process (diagnostics: an operation that reads the
        // file of a user in the helper makes none)
        uint64_t inputCopiesMade();

        // Opens the raster of a user at `path` (UTF-8) with GDAL, in the raster helper alone: its
        // operations (a mask, a merge, an alignment...) and the code they reach (a thumbnail detects the
        // sensor of its raster) read the file of the request with openUserRaster. Null (as GDALOpen
        // returns it) when the raster cannot be opened. The host reads its rasters with ddb-tilers
        // (tilers::Raster::openInput). @throws std::logic_error in the host
        GDALDatasetUniquePtr openInputRaster(const std::string &path);
    } // namespace raster
} // namespace ddb

#endif // RASTER_INPUT_H
