/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef TILE_TEST_SUPPORT_H
#define TILE_TEST_SUPPORT_H

#include <string>

#include "fs.h"
#include "output_format.h"
#include "tilerhelper.h"

namespace ddb
{
    // The request for tile (tz, tx, ty) of path, as the tests ask for it: tiles in folder
    // (empty: not kept on disk when a buffer is asked for)
    inline TileRequest testTile(const fs::path &path, int tz, int tx, int ty, int tileSize, bool tms,
                                bool forceRecreate, const fs::path &folder = "")
    {
        TileRequest request;
        request.source.path = path;
        request.source.tileSize = tileSize;
        request.source.tms = tms;
        request.source.forceRecreate = forceRecreate;
        request.source.outputFolder = folder;
        request.tz = tz;
        request.tx = tx;
        request.ty = ty;
        return request;
    }

    // What `ddb tile` is asked for: format and zoom levels (and, with x and y, one tile of each)
    inline TilerRun tilerRun(OutputFormat format, const std::string &zRange = "auto",
                             const std::string &x = "auto", const std::string &y = "auto")
    {
        TilerRun run;
        run.format = format;
        run.zRange = zRange;
        run.x = x;
        run.y = y;
        return run;
    }

    // The same request, with the encoded tile returned in buf/size
    inline TileRequest inMemory(TileRequest request, uint8_t **buf, int *size)
    {
        request.outBuffer = buf;
        request.outBufferSize = size;
        return request;
    }
}

#endif // TILE_TEST_SUPPORT_H
