/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef RASTER_ARTEFACTS_H
#define RASTER_ARTEFACTS_H

#include <string>

#include "fs.h"
#include "gdal_raii.h"
#include "mio.h"

namespace ddb
{
    // Files that libddb wrote itself, as opposed to the files of users. The host never opens a
    // file of a user with GDAL: the raster helper (gdal-util, raster_helper.h) reads it once and
    // writes what the host needs, and the host opens only that.
    //
    // An artefact is a file in one of these places, and in no other:
    //   - a build output of a dataset that GDAL reads (the COG of a raster, the GeoPackage of a
    //     vector, the MVT tiles), at the place the build layout gives it:
    //     "<dataset>/.ddb/build/<hash>/<output of a role>". Any other file under ".ddb/build"
    //     (the content of an extracted archive, a file somebody put there) is not one;
    //   - the "artefacts" folder of the cache (UserProfile::getCacheDir()), where the host puts
    //     the files the helper wrote on request: the COG a raster is tiled from, the GeoTIFF of
    //     a photo, and the folders the helper writes into.
    // A download is not an artefact, whatever it is named and wherever it was put. A link is
    // followed before the place is judged, so a link in an artefact place to a file elsewhere
    // is not one either.
    namespace raster
    {
        // The "artefacts" folder of the cache (created)
        fs::path artefactsDir();

        // The folder of the artefacts folder that holds the normalized copies of rasters of
        // users (artefact_cache.h), named by artefactKey (created)
        fs::path normalizedDir();

        // Whether the file is an artefact (see above). A file that does not exist can be: the
        // place is what counts. The path is resolved first (symbolic links, "..").
        bool isLibddbArtefact(const fs::path &path);

        // The name that stands for the file as it is now: CRC64 of its path, modification time
        // (to the microsecond the file system keeps) and size, in hexadecimal, so a changed file
        // gets another name.
        // @throws FSException when the file cannot be read
        std::string artefactKey(const fs::path &source);

        // Opens an artefact raster read-only with the drivers libddb writes it with (GTiff, COG)
        // and no others: a VRT, whatever its name, is not opened. While it opens GDAL makes no
        // HTTP request and opens no other file through its network file systems. Null (as GDALOpen
        // returns it) when GDAL cannot open the file.
        // @throws GDALException when the file is not an artefact
        GDALDatasetUniquePtr openArtefactRaster(const fs::path &path);

        // The same for a vector artefact: GeoPackage
        GDALDatasetUniquePtr openArtefactVector(const fs::path &path);

        // A new folder in `parent` that only the user of the process can enter, owned by the
        // returned object (removed with it): the place a helper writes its result, so that
        // nobody else can swap the file between the helper and the host's check
        // @throws FSException
        io::ScopedTempDir makePrivateFolder(const fs::path &parent, const std::string &prefix);

        // A private folder in artefactsDir(): what the helper writes there is an artefact, and
        // the host checks it with openArtefactRaster before it moves it to its name
        io::ScopedTempDir makeArtefactRequestFolder(const std::string &prefix);

        // Moves a file the helper wrote (made by makeArtefactRequestFolder) to `target`, replacing
        // what is there, in one rename: readers see the old file or the whole new one. Another
        // device than the cache's costs a copy to a sibling of `target` first, then the rename.
        // @throws FSException
        void publishArtefact(const fs::path &produced, const fs::path &target);

        // Removes the artefacts that were not touched for `maxAge` seconds: files, the normalized
        // copies, and the folders of requests that were left behind
        void cleanupArtefacts(time_t maxAge);

    } // namespace raster
} // namespace ddb

#endif // RASTER_ARTEFACTS_H
