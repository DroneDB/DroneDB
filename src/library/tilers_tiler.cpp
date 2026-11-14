/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "tilers_tiler.h"

#include "json.h"
#include "tilers_call.h"

namespace ddb
{
    TilersTiler::TilersTiler(const std::string &path, const std::string &tilesFolder, int size, bool tmsRows)
        : Tiler(path, tilesFolder, size, tmsRows)
    {
        char *raw = nullptr;
        tilers::check(ddbtTileInfo(path.c_str(), size, &raw));
        const tilers::String text(raw);
        const json info = json::parse(text.get());
        oMinX = info.at("minX").get<double>();
        oMinY = info.at("minY").get<double>();
        oMaxX = info.at("maxX").get<double>();
        oMaxY = info.at("maxY").get<double>();
        tMinZ = info.at("minZoom").get<int>();
        tMaxZ = info.at("maxZoom").get<int>();
        nBands = 0; // not used: ddb-tilers chooses the bands of every tile
    }

    std::string TilersTiler::tile(int tz, int tx, int ty, ImageFormat format, uint8_t **outBuffer, int *outBufferSize)
    {
        uint8_t *raw = nullptr;
        int size = 0;
        tilers::check(ddbtMemoryTileFmt(inputPath.c_str(), tz, tx, ty, &raw, &size, tileSize, tms, false, nullptr,
                                        imageFormatExtension(format)));
        const tilers::Bytes bytes(raw);
        return writeEncodedTile(bytes.get(), static_cast<size_t>(size), format, tz, tx, ty, outBuffer, outBufferSize);
    }
} // namespace ddb
