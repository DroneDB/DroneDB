/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "cloud_sample.h"

#include <bit>
#include <cstring>
#include <fstream>
#include <string>

#include "cancellation.h"
#include "ddbt/ddbt.h"
#include "exceptions.h"
#include "gdal_open_policy.h"
#include "json.h"
#include "mio.h"
#include "tilers_call.h"

// The sample file is written from memory as it is
static_assert(std::endian::native == std::endian::little, "the cloud sample assumes a little-endian host");

namespace ddb
{
    namespace
    {
        constexpr char Magic[16] = {'D', 'D', 'B', 'C', 'L', 'O', 'U', 'D', 'S', 'A', 'M', 'P', 'L', 'E', '1', '\0'};
        constexpr size_t RecordBytes = 3 * sizeof(double) + 3 * sizeof(uint16_t);

        // What ddbtReadPoints hands over goes to the sampler, the points read to the progress
        struct Reading
        {
            GridSampler sampler;
            std::array<double, 3> shift;
            uint64_t total = 0;
            uint64_t read = 0;
            const std::function<void(double)> *progress = nullptr;

            static int batch(uint64_t count, const double *xyz, const uint16_t *rgb, void *user)
            {
                auto &self = *static_cast<Reading *>(user);
                if (cancel::isCanceled())
                    return 1;
                for (uint64_t i = 0; i < count; ++i)
                {
                    const double *p = xyz + 3 * i;
                    const uint16_t r = rgb != nullptr ? rgb[3 * i] : 0, g = rgb != nullptr ? rgb[3 * i + 1] : 0,
                                   b = rgb != nullptr ? rgb[3 * i + 2] : 0;
                    self.sampler.add(p[0] - self.shift[0], p[1] - self.shift[1], p[2] - self.shift[2], r, g, b);
                }
                self.read += count;
                if (*self.progress && self.total > 0)
                    (*self.progress)(std::min(1.0, static_cast<double>(self.read) / static_cast<double>(self.total)));
                return 0;
            }
        };
    }

    std::vector<SampledPoint> sampleCloudFile(const fs::path &path, const CloudSampleParams &params,
                                              const std::function<void(double)> &progress)
    {
        gdalopen::requireUserFileOpens("sampleCloudFile");
        const std::string utf8 = io::toUtf8(path);
        char *info = nullptr;
        tilers::check(ddbtPointCloudInfo(utf8.c_str(), false, &info));
        const tilers::String ownedInfo(info);

        Reading reading{GridSampler(params.limit), params.shift};
        reading.total = json::parse(info).at("pointCount").get<uint64_t>();
        reading.progress = &progress;
        const ddbtErr rc = ddbtReadPoints(utf8.c_str(), &Reading::batch, &reading);
        cancel::throwIfCanceled();
        tilers::check(rc);
        return reading.sampler.points();
    }

    void writeCloudSample(const fs::path &out, const std::vector<SampledPoint> &points)
    {
        std::ofstream f(out, std::ios::binary | std::ios::trunc);
        if (!f)
            throw FSException("Cannot write " + io::toUtf8(out));
        f.write(Magic, sizeof Magic);
        const uint64_t count = points.size();
        f.write(reinterpret_cast<const char *>(&count), sizeof count);
        for (const auto &p : points)
        {
            const double xyz[3] = {p.x, p.y, p.z};
            const uint16_t rgb[3] = {p.r, p.g, p.b};
            f.write(reinterpret_cast<const char *>(xyz), sizeof xyz);
            f.write(reinterpret_cast<const char *>(rgb), sizeof rgb);
        }
        f.close();
        if (!f)
            throw FSException("Cannot write " + io::toUtf8(out));
    }

    std::vector<SampledPoint> readCloudSample(const fs::path &in)
    {
        std::ifstream f(in, std::ios::binary);
        char magic[sizeof Magic];
        uint64_t count = 0;
        if (!f || !f.read(magic, sizeof magic) || std::memcmp(magic, Magic, sizeof Magic) != 0 ||
            !f.read(reinterpret_cast<char *>(&count), sizeof count))
            throw AppException(io::toUtf8(in) + " is not a cloud sample");
        std::error_code ec;
        const uintmax_t size = fs::file_size(in, ec);
        const uintmax_t header = sizeof Magic + sizeof count;
        if (ec || size < header || (size - header) / RecordBytes != count || (size - header) % RecordBytes != 0)
            throw AppException(io::toUtf8(in) + " is not a whole cloud sample of " + std::to_string(count) + " points");
        std::vector<SampledPoint> points(static_cast<size_t>(count));
        for (auto &p : points)
        {
            double xyz[3];
            uint16_t rgb[3];
            f.read(reinterpret_cast<char *>(xyz), sizeof xyz);
            f.read(reinterpret_cast<char *>(rgb), sizeof rgb);
            p = {xyz[0], xyz[1], xyz[2], rgb[0], rgb[1], rgb[2]};
        }
        if (!f)
            throw AppException("Cannot read the cloud sample " + io::toUtf8(in));
        return points;
    }
}
