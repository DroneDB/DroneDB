/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "tilers_raster.h"

#include <stdexcept>

#include "artefact_cache.h"
#include "ddbt/ddbt.h"
#include "exceptions.h"
#include "gdal_open_policy.h"
#include "json.h"
#include "logger.h"
#include "mio.h"
#include "raster_artefacts.h"
#include "raster_ops.h"
#include "tilers_call.h"

namespace ddb::tilers
{
    namespace
    {
        std::optional<double> optionalNumber(const json &object, const char *name)
        {
            const auto it = object.find(name);
            if (it == object.end() || !it->is_number())
                return std::nullopt;
            return it->get<double>();
        }
    }

    Raster::Raster(ddbtRaster *handle) : handle_(handle)
    {
        char *text = nullptr;
        const ddbtErr rc = ddbtRasterInfo(handle_, &text);
        if (rc != DDBT_ERR_NONE)
        {
            ddbtRasterClose(handle_);
            check(rc);
        }
        const tilers::String owned(text);
        const json info = json::parse(text);
        width_ = info.at("width").get<int>();
        height_ = info.at("height").get<int>();
        dataType_ = info.at("type").get<std::string>();
        if (info.contains("geoTransform"))
            geoTransform_ = info.at("geoTransform").get<std::array<double, 6>>();
        wkt_ = info.value("wkt", std::string());
        for (const auto &b : info.at("bands"))
        {
            Band band;
            band.colorInterp = b.at("colorInterp").get<std::string>();
            band.nodata = optionalNumber(b, "nodata");
            band.scale = optionalNumber(b, "scale");
            band.offset = optionalNumber(b, "offset");
            band.unit = b.value("unit", std::string());
            band.description = b.value("description", std::string());
            bands_.push_back(std::move(band));
        }
    }

    Raster::~Raster() { ddbtRasterClose(handle_); }

    std::unique_ptr<Raster> Raster::open(const fs::path &file)
    {
        ddbtRaster *handle = nullptr;
        check(ddbtRasterOpen(io::toUtf8(file).c_str(), &handle));
        return std::unique_ptr<Raster>(new Raster(handle));
    }

    std::unique_ptr<Raster> Raster::openInput(const std::string &path)
    {
        if (gdalopen::userFileOpensEnabled())
            throw std::logic_error("tilers::Raster::openInput runs in the host: the raster helper reads the files "
                                   "of users with GDAL");
        const fs::path file = io::fromUtf8(path);
        // A raster that is not there has no copy to make
        std::error_code ec;
        if (!raster::isLibddbArtefact(file) && !fs::exists(file, ec))
            return nullptr;
        fs::path artefact;
        try
        {
            artefact = raster::artefactFor(file);
        }
        catch (const raster::RasterOpException &e)
        {
            // The raster cannot be read, or the rules refuse it, as for openInputRaster; a failure of
            // the helper itself is not that
            if (e.code() == raster::ErrorCode::Internal)
                throw;
            LOGD << "Cannot open " << path << ": " << e.what();
            return nullptr;
        }
        // The file of libddb opens, or there is no raster (as when GDAL cannot open it)
        ddbtRaster *handle = nullptr;
        if (const ddbtErr rc = ddbtRasterOpen(io::toUtf8(artefact).c_str(), &handle); rc != DDBT_ERR_NONE)
        {
            LOGD << "Cannot open " << path << ": " << ddbtLastError();
            return nullptr;
        }
        return std::unique_ptr<Raster>(new Raster(handle));
    }

    void Raster::read(int x, int y, int width, int height, int bufWidth, int bufHeight, std::span<const int> bands,
                      float *out, uint8_t *mask) const
    {
        check(ddbtRasterRead(handle_, x, y, width, height, bufWidth, bufHeight, bands.data(),
                             static_cast<int>(bands.size()), DDBT_SAMPLE_FLOAT32, out, mask));
    }

    void Raster::read(int x, int y, int width, int height, int bufWidth, int bufHeight, std::span<const int> bands,
                      double *out, uint8_t *mask) const
    {
        check(ddbtRasterRead(handle_, x, y, width, height, bufWidth, bufHeight, bands.data(),
                             static_cast<int>(bands.size()), DDBT_SAMPLE_FLOAT64, out, mask));
    }
}
