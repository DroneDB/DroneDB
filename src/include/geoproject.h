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
    // to a folder (several, or when `output` is one). The position of a photo is read from its
    // EXIF here; the picture is read and projected by the raster helper (raster_helper.h), which
    // writes where only this call writes to. The GeoTIFF is checked and moved to its name in one
    // rename.
    // @throws InvalidArgsException for an outsize that is no positive number
    void geoProject(const std::vector<std::string> &images, const std::string &output, const std::string &outsize = "", bool stopOnError = false, const GeoProjectCallback &callback = nullptr);

}

#endif // GEOPROJECT_H
