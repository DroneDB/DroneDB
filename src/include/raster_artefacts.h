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
    //     a photo, and the folders the helper writes into;
    //   - a file of GDAL's memory file system (/vsimem/), which only this process can write.
    // A download is not an artefact, whatever it is named and wherever it was put. A link is
    // followed before the place is judged, so a link in an artefact place to a file elsewhere
    // is not one either. The place of a build output is the whole proof that libddb wrote it:
    // nothing a user supplies may be written under ".ddb/build" (an add refuses the ".ddb"
    // names, a pull and an extracted archive never write there, and the archive of a 3D model
    // is extracted into the folder of its own role, which is not a readable one), and the
    // build state that lives beside the outputs could be planted as easily as an output.
    namespace raster
    {
        // The "artefacts" folder of the cache (created)
        fs::path artefactsDir();

        // The folder of the artefacts folder that holds the normalized copies of rasters of
        // users (artefact_cache.h), named by artefactKey (created)
        fs::path normalizedDir();

        // Whether the file is an artefact (see above). A file that does not exist can be: the
        // place is what counts. The path is resolved first (symbolic links, "..").
        // The only "temp" libddb has is the private folder of a request, below the artefacts
        // folder; the other temporary folders (a DXF converted from a DWG, a model converted
        // for the 3D builds) hold files the helper or the converters read, never ones the host
        // opens with GDAL, so they are not artefacts.
        bool isLibddbArtefact(const fs::path &path);

        // The name that stands for the file as it is now: CRC64 of its path, modification time
        // (to the microsecond the file system keeps) and size, in hexadecimal, so a changed file
        // gets another name.
        // @throws FSException when the file cannot be read
        std::string artefactKey(const fs::path &source);

        // How an artefact or a file libddb wrote is opened
        enum class ArtefactAccess
        {
            ReadOnly,
            Update
        };

        // Opens an artefact raster with the drivers libddb writes it with (GTiff, COG) and no
        // others: a VRT, whatever its name, is not opened (a memory dataset is not opened by a
        // path either: no caller has one). While it opens GDAL makes no HTTP request and opens
        // no other file through its network file systems (gdalopen::NoNetworkWhileOpening). Null
        // (as GDALOpen returns it) when GDAL cannot open the file or its metadata names another
        // dataset (gdalopen::validateMetadataFiles).
        // @throws GDALException when the file is not an artefact
        GDALDatasetUniquePtr openArtefactRaster(const fs::path &path, ArtefactAccess access = ArtefactAccess::ReadOnly);

        // The same for a vector artefact, read-only: GeoPackage (the one a build writes, or
        // that libddb wrote in memory). No MVT driver: the tiles of a build are served as files
        // and no libddb code reads them with GDAL.
        GDALDatasetUniquePtr openArtefactVector(const fs::path &path);

        // Opens again the raster that this call has just written (the export of a raster, the COG
        // of a build in the helper), where it was told to write it: that is a file of the user's
        // choosing, so it is no artefact by place. Only a regular file, with the drivers of the
        // artefacts and the same guards on the open. Null (as GDALOpen returns it) when the path is
        // not a regular file or GDAL cannot open it.
        GDALDatasetUniquePtr reopenWrittenRaster(const fs::path &path, ArtefactAccess access = ArtefactAccess::ReadOnly);

        // A new folder in `parent` that only the user of the process can enter, owned by the
        // returned object (removed with it): the place a helper writes its result, so that
        // nobody else can swap the file between the helper and the host's check. On Windows it
        // has a low integrity label, so that the confined helper may write in it.
        // @throws FSException
        io::ScopedTempDir makePrivateFolder(const fs::path &parent, const std::string &prefix);

        // A private folder in artefactsDir(): what the helper writes there is an artefact, and
        // the host checks it with openArtefactRaster before it moves it to its name
        io::ScopedTempDir makeArtefactRequestFolder(const std::string &prefix);

        // Moves a file the helper wrote (in a folder made by makePrivateFolder) to `target`, replacing
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
