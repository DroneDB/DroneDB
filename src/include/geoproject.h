/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef GEOPROJECT_H
#define GEOPROJECT_H

#include "database.h"

namespace ddb
{

    typedef std::function<bool(const std::string &imageWritten)>
        GeoProjectCallback;

    // Places photos on the ground and writes a GeoTIFF for each, to the file `output` (one photo) or
    // to a folder (several, or when `output` is one or ends with '/' or '\', <folder>/<image
    // name>.tif; a missing folder is created). The position of a photo is read from its
    // EXIF here; the picture is read and projected by the raster helper (raster_helper.h), which
    // writes where only this call writes to. The GeoTIFF is checked and moved to its name in one
    // rename.
    // Every projected file is checked before the first is written (io::assureReplaceable): one
    // that is an image or the target of two images is refused, and one that exists unless
    // overwrite (true for the internal callers: the tile cache replaces through forceRecreate).
    // @throws InvalidArgsException for an outsize that is no positive number, an output that is
    //         an image or two images with the same target
    // @throws OutputExistsException for a projected file that exists when overwrite is false
    void geoProject(const std::vector<std::string> &images, const std::string &output, const std::string &outsize = "", bool stopOnError = false, const GeoProjectCallback &callback = nullptr,
                    bool overwrite = true);

}

#endif // GEOPROJECT_H
