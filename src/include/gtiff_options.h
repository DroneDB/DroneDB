/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef GTIFF_OPTIONS_H
#define GTIFF_OPTIONS_H

// The creation options of the GeoTIFFs and COGs that libddb writes (DQ-06): one vocabulary for
// the mask, COG, merge, align and export code, which wrote the same KEY=VALUE strings by hand
// (TILED=YES 5 times, BIGTIFF=IF_SAFER 6, COMPRESS 15, the float-predictor decision twice).
//
//   GtiffOptions co;
//   co.compress(GtiffOptions::Codec::Deflate).predictorFor(type).tiled(512).bigTiffIfSafer();
//   co.list()            KEY=VALUE strings for GDALCreate / GDALCreateCopy
//   co.appendTo(args)    "-co", "KEY=VALUE" pairs for the arguments of gdal_translate/warp/nearblack

#include <string>

#include "gdal_inc.h"

namespace ddb
{
    class GtiffOptions
    {
        CPLStringList options_;

    public:
        enum class Codec
        {
            Lzw,
            Deflate,
            Jpeg
        };

        // COMPRESS=LZW | DEFLATE | JPEG
        GtiffOptions &compress(Codec codec)
        {
            switch (codec)
            {
            case Codec::Lzw:
                return set("COMPRESS", "LZW");
            case Codec::Deflate:
                return set("COMPRESS", "DEFLATE");
            default:
                return set("COMPRESS", "JPEG");
            }
        }

        // PREDICTOR for the type of the samples: 3 (floating point) for Float32 and Float64, 2
        // (horizontal differencing) for the others. Only valid with LZW, DEFLATE, LZMA and ZSTD.
        GtiffOptions &predictorFor(GDALDataType type)
        {
            return set("PREDICTOR", (type == GDT_Float32 || type == GDT_Float64) ? "3" : "2");
        }

        // PREDICTOR=YES: GDAL chooses by the type of the samples
        GtiffOptions &predictorAuto() { return set("PREDICTOR", "YES"); }

        // TILED=YES, with square blocks of `blockSize` pixels when it is positive
        GtiffOptions &tiled(int blockSize = 0)
        {
            set("TILED", "YES");
            if (blockSize > 0)
            {
                set("BLOCKXSIZE", std::to_string(blockSize));
                set("BLOCKYSIZE", std::to_string(blockSize));
            }
            return *this;
        }

        // BIGTIFF=IF_SAFER: BigTIFF when the file could exceed 4 GiB
        GtiffOptions &bigTiffIfSafer() { return set("BIGTIFF", "IF_SAFER"); }

        // Any other creation option
        GtiffOptions &set(const std::string &key, const std::string &value)
        {
            options_.AddString((key + "=" + value).c_str());
            return *this;
        }

        // A complete KEY=VALUE option (the NUM_THREADS of parallel::gdalNumThreads)
        GtiffOptions &add(const std::string &keyValue)
        {
            options_.AddString(keyValue.c_str());
            return *this;
        }

        // The options as KEY=VALUE strings
        const CPLStringList &list() const noexcept { return options_; }

        // Appends the options to the arguments of a GDAL utility, each after "-co"
        void appendTo(CPLStringList &args) const
        {
            for (int i = 0; i < options_.Count(); i++)
            {
                args.AddString("-co");
                args.AddString(options_[i]);
            }
        }
    };
}

#endif // GTIFF_OPTIONS_H
