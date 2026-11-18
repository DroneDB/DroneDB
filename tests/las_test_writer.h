/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef LAS_TEST_WRITER_H
#define LAS_TEST_WRITER_H

// A LAS file for the tests, written by hand: LAS 1.4, point format 6, uncompressed, the coordinates
// at a scale of 0.001 (or the scale given for each axis) from an offset at their minimum, and the
// CRS as a WKT record (the WKT bit of the global encoding set), as a producer of point clouds writes
// them.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "fs.h"

namespace testutils
{
    struct LasPoint
    {
        double x, y, z;
    };

    namespace detail
    {
        template <typename T> void putLe(std::ofstream &out, T value)
        {
            out.write(reinterpret_cast<const char *>(&value), sizeof value);
        }

        inline void putText(std::ofstream &out, const std::string &text, size_t width)
        {
            std::string field(width, '\0');
            std::memcpy(field.data(), text.data(), std::min(width, text.size()));
            out.write(field.data(), static_cast<std::streamsize>(width));
        }
    }

    // The scale of a cloud in metres, and of longitudes and latitudes in degrees as Metashape writes
    // them (with heights in millimetres)
    constexpr std::array<double, 3> MetreScale = {0.001, 0.001, 0.001};
    constexpr std::array<double, 3> DegreeScale = {1e-7, 1e-7, 0.001};

    // Writes `points` to `path` with `wkt` as the CRS (none when it is empty), at `scale`
    inline void writeLas14(const fs::path &path, const std::string &wkt, const std::vector<LasPoint> &points,
                           const std::array<double, 3> &scale = MetreScale)
    {
        using detail::putLe;
        constexpr uint16_t HeaderSize = 375;
        constexpr uint16_t VlrHeaderSize = 54;
        constexpr uint16_t RecordLength = 30;
        double lo[3] = {std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
                        std::numeric_limits<double>::max()};
        double hi[3] = {std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(),
                        std::numeric_limits<double>::lowest()};
        for (const auto &p : points)
        {
            const double c[3] = {p.x, p.y, p.z};
            for (int i = 0; i < 3; ++i)
            {
                lo[i] = std::min(lo[i], c[i]);
                hi[i] = std::max(hi[i], c[i]);
            }
        }
        if (points.empty())
            for (int i = 0; i < 3; ++i)
                lo[i] = hi[i] = 0;
        const std::string wktRecord = wkt.empty() ? std::string() : wkt + std::string(1, '\0');
        const uint32_t vlrCount = wkt.empty() ? 0 : 1;
        const uint32_t pointOffset =
            HeaderSize + (wkt.empty() ? 0 : VlrHeaderSize + static_cast<uint32_t>(wktRecord.size()));

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write("LASF", 4);
        putLe<uint16_t>(out, 0);                          // file source
        putLe<uint16_t>(out, wkt.empty() ? 0 : 16);       // global encoding: the CRS is WKT
        out.write(std::string(16, '\0').data(), 16);      // GUID
        putLe<uint8_t>(out, 1);
        putLe<uint8_t>(out, 4);                           // LAS 1.4
        detail::putText(out, "ddb tests", 32);           // system
        detail::putText(out, "ddb tests", 32);           // software
        putLe<uint16_t>(out, 1);                          // day of the year
        putLe<uint16_t>(out, 2024);                       // year
        putLe<uint16_t>(out, HeaderSize);
        putLe<uint32_t>(out, pointOffset);
        putLe<uint32_t>(out, vlrCount);
        putLe<uint8_t>(out, 6);                           // point format
        putLe<uint16_t>(out, RecordLength);
        putLe<uint32_t>(out, 0);                          // legacy point count (format 6: none)
        for (int i = 0; i < 5; ++i)
            putLe<uint32_t>(out, 0);                      // legacy points by return
        for (int i = 0; i < 3; ++i)
            putLe<double>(out, scale[i]);
        for (int i = 0; i < 3; ++i)
            putLe<double>(out, lo[i]);                    // offsets at the minimum
        for (int i = 0; i < 3; ++i)
        {
            putLe<double>(out, hi[i]);
            putLe<double>(out, lo[i]);
        }
        putLe<uint64_t>(out, 0);                          // waveform data
        putLe<uint64_t>(out, 0);                          // first EVLR
        putLe<uint32_t>(out, 0);                          // EVLRs
        putLe<uint64_t>(out, points.size());
        for (int i = 0; i < 15; ++i)
            putLe<uint64_t>(out, i == 0 ? points.size() : 0);  // all first returns

        if (!wkt.empty())
        {
            putLe<uint16_t>(out, 0);
            detail::putText(out, "LASF_Projection", 16);
            putLe<uint16_t>(out, 2112);
            putLe<uint16_t>(out, static_cast<uint16_t>(wktRecord.size()));
            detail::putText(out, "OGC WKT", 32);
            out.write(wktRecord.data(), static_cast<std::streamsize>(wktRecord.size()));
        }

        for (const auto &p : points)
        {
            const double c[3] = {p.x, p.y, p.z};
            for (int i = 0; i < 3; ++i)
                putLe<int32_t>(out, static_cast<int32_t>(std::llround((c[i] - lo[i]) / scale[i])));
            putLe<uint16_t>(out, 0);     // intensity
            putLe<uint8_t>(out, 0x11);   // return 1 of 1
            putLe<uint8_t>(out, 0);      // flags
            putLe<uint8_t>(out, 0);      // class
            putLe<uint8_t>(out, 0);      // user data
            putLe<int16_t>(out, 0);      // scan angle
            putLe<uint16_t>(out, 0);     // point source
            putLe<double>(out, 0.0);     // GPS time
        }
    }
}

#endif // LAS_TEST_WRITER_H
