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
    // Files that libddb wrote itself, as opposed to the files of users. The host opens no file of
    // a user with GDAL: the raster helper (gdal-util, raster_helper.h) reads it, and the host
    // reads with ddb-tilers only a file libddb wrote or a GeoTIFF the helper found that ddb-tilers
    // reads whole (raster_input.h).
    //
    // An artefact is a file in one of these places, and in no other:
    //   - a build output of a dataset that GDAL reads (the COG of a raster, the GeoPackage of a
    //     vector, the MVT tiles), at the place the build layout gives it:
    //     "<dataset>/.ddb/build/<hash>/<output of a role>". Any other file under ".ddb/build"
    //     (the content of an extracted archive, a file somebody put there) is not one;
    //   - the folder of a request to the helper that this process made and still holds
    //     (RequestFolder): what the helper wrote for the request, which the host checks before it
    //     reads it, and which goes with the request;
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
        // Whether the file is an artefact (see above). A file that does not exist can be: the
        // place is what counts. The path is resolved first (symbolic links, "..").
        // The only "temp" libddb trusts is the folder of a live request; the other temporary
        // folders (a DXF converted from a DWG, a model converted for the 3D builds) hold files the
        // helper or the converters read, never ones the host opens with GDAL, so they are not
        // artefacts.
        bool isLibddbArtefact(const fs::path &path);

        // @throws GDALException when the file is not an artefact (the tilers and the openers
        // below read nothing else)
        void requireArtefact(const fs::path &path);

        // Opens a vector artefact, read-only, with the driver libddb writes it with: GeoPackage (the one a
        // build writes, or that libddb wrote in memory). No MVT driver: the tiles of a build are served as
        // files and no libddb code reads them with GDAL. While it opens GDAL makes no HTTP request and
        // opens no other file through its network file systems (gdalopen::NoNetworkWhileOpening). Null (as
        // GDALOpen returns it) when GDAL cannot open the file or its metadata names another dataset
        // (gdalopen::validateMetadataFiles). The host reads no raster with GDAL: its rasters are read with
        // ddb-tilers (tilers::Raster), and the raster helper reads the files of users.
        // @throws GDALException when the file is not an artefact
        GDALDatasetUniquePtr openArtefactVector(const fs::path &path);

        // Whether the raster the helper has just written for this call (the COG of a build, a
        // normalized copy, a geoprojected photo) is a regular file that libddb reads: a TIFF that
        // ddb-tilers opens and describes. The helper read a file of a user, and what it wrote is
        // then published and trusted by its place. The file is opened for the check alone and
        // closed before it returns: the caller renames it next, which Windows refuses while a
        // process holds it open.
        bool writtenRasterOpens(const fs::path &path);

        // Who writes in a private folder (makePrivateFolder)
        enum class PrivateFolderWriter
        {
            // The confined raster helper (gdal-util), at low integrity on Windows
            Helper,
            // This process, or a program it runs unconfined (bm3d)
            Host
        };

        // A new folder in `parent` that only the user of the process can enter, owned by the
        // returned object (removed with it): the place a helper writes its result, so that
        // nobody else can swap the file between the helper and the host's check. On Windows a
        // folder for the Helper has a low integrity label, so that the confined helper may write
        // in it; what is made in it inherits the label and keeps it when it is moved out. A
        // folder for the Host has none: a writer that does not run the helper publishes files
        // that a low integrity process cannot change.
        // @throws FSException
        io::ScopedTempDir makePrivateFolder(const fs::path &parent, const std::string &prefix,
                                            PrivateFolderWriter writer = PrivateFolderWriter::Helper);

        // The folder of a request to the raster helper: a private folder (makePrivateFolder) in the
        // "requests" folder of the cache, removed with this object. While it lives, the files in it are
        // artefacts of this process (isLibddbArtefact): the helper writes there, and the host checks
        // what it wrote (writtenRasterOpens) before it reads it. A folder a crash left behind is no
        // longer one, and the cleanup of the cache removes it.
        class RequestFolder
        {
          public:
            // @throws FSException
            explicit RequestFolder(const std::string &prefix);
            ~RequestFolder();
            RequestFolder(const RequestFolder &) = delete;
            RequestFolder &operator=(const RequestFolder &) = delete;

            const fs::path &path() const noexcept { return folder_.path(); }

          private:
            io::ScopedTempDir folder_;
            // The resolved place of the folder (fileversion.h cacheKeyPath), as isLibddbArtefact compares it
            std::string key_;
        };

        // The folder of the cache that holds the request folders (not created)
        fs::path requestsDir();

        // Whether `folder` is the folder of a request this process holds (RequestFolder)
        bool isLiveRequestFolder(const fs::path &folder);

        // Moves a file the helper wrote (in a folder made by makePrivateFolder) to `target`, replacing
        // what is there, in one rename: readers see the old file or the whole new one. Another
        // device than the cache's costs a copy to a sibling of `target` first, then the rename.
        // @throws FSException
        void publishArtefact(const fs::path &produced, const fs::path &target);

        // Removes the folder of the artefact cache of earlier versions ("<cache>/artefacts": the
        // normalized copies and the COGs of photos they kept), when it is there. Best effort: what
        // cannot be removed is logged and left for the next start.
        void removeLegacyArtefactCache();

    } // namespace raster
} // namespace ddb

#endif // RASTER_ARTEFACTS_H
