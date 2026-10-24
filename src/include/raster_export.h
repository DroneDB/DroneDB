/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef RASTER_EXPORT_H
#define RASTER_EXPORT_H

#include <functional>
#include <string>

#include "ddb_export.h"

namespace ddb
{

    // What to export of a raster as a GeoTIFF with internal overviews: a vegetation index drawn
    // with a colormap (RGBA), or three of its bands stretched to Byte
    struct RasterExport
    {
        std::string input;
        std::string output; // a file that exists is replaced; it must not be the input

        // The rendering, all optional: with a formula the export is an RGBA rendering of the index,
        // otherwise `bands` ("1,2,3") or the band `preset` of the sensor choose what to export (the
        // first three bands by default). rescale is "min,max".
        std::string preset;
        std::string bands;
        std::string formula;
        std::string bandFilter;
        std::string colormap;
        std::string rescale;

        // Window size of a formula export in pixels (clamped to 64..8192 and a multiple of 16);
        // 0 chooses one that keeps the working set around 64 MB
        int tileSize = 0;

        // Called with the fraction done and the phase ("rendering", "converting"); returns true
        // to cancel the export
        std::function<bool(double fraction, const char *phase)> progress;
    };

    // Exports the raster block by block, so the memory used does not depend on its size. A failed or
    // canceled export leaves no output file. Throws CanceledException when the progress callback
    // asked to stop or the call was canceled (cancellation.h).
    DDB_DLL void exportRaster(const RasterExport &request);

} // namespace ddb

#endif // RASTER_EXPORT_H
