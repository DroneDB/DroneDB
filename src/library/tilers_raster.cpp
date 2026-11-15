/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "tilers_raster.h"

#include <algorithm>
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
        blockWidth_ = info.at("blockWidth").get<int>();
        blockHeight_ = info.at("blockHeight").get<int>();
        const std::string mask = info.at("mask").get<std::string>();
        mask_ = mask == "perDataset" ? Mask::PerDataset
                : mask == "nodata"   ? Mask::Nodata
                : mask == "alpha"    ? Mask::Alpha
                                     : Mask::AllValid;
        alphaBand_ = info.value("alphaBand", 0);
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

    bool Raster::hasMask(int band1) const
    {
        // The NoData value and the mask of a TIFF are the file's, the same for every band; the alpha band
        // is the mask of the other bands, not its own (GDALRasterBand::GetMaskBand)
        switch (mask_)
        {
        case Mask::PerDataset:
            return true;
        case Mask::Alpha:
            return band1 != alphaBand_;
        default:
            return false;
        }
    }

    template <typename T>
    void Raster::readBandAs(int band1, int x, int y, int width, int height, int bufWidth, int bufHeight, T *out,
                            uint8_t *mask) const
    {
        const int bands[1] = {band1};
        if (mask == nullptr || !hasMask(band1))
        {
            read(x, y, width, height, bufWidth, bufHeight, bands, out);
            return;
        }
        if (mask_ == Mask::PerDataset)
        {
            read(x, y, width, height, bufWidth, bufHeight, bands, out, mask);
            return;
        }
        // The alpha band, read with the band: its samples as they are for Byte, rescaled to 0..255 for
        // UInt16 with any alpha above 0 kept above 0 (GDALRescaledAlphaBand); GDAL takes no other type
        const size_t count = static_cast<size_t>(bufWidth) * static_cast<size_t>(bufHeight);
        std::vector<T> planes(count * 2);
        const int both[2] = {band1, alphaBand_};
        read(x, y, width, height, bufWidth, bufHeight, both, planes.data());
        std::copy(planes.begin(), planes.begin() + static_cast<std::ptrdiff_t>(count), out);
        const bool rescaled = dataType_ == "UInt16";
        for (size_t i = 0; i < count; i++)
        {
            const auto alpha = static_cast<uint32_t>(planes[count + i]);
            mask[i] = static_cast<uint8_t>(rescaled ? (alpha > 0 && alpha < 257 ? 1 : alpha / 257) : alpha);
        }
    }

    void Raster::readBand(int band1, int x, int y, int width, int height, int bufWidth, int bufHeight, float *out,
                          uint8_t *mask) const
    {
        readBandAs(band1, x, y, width, height, bufWidth, bufHeight, out, mask);
    }

    void Raster::readBand(int band1, int x, int y, int width, int height, int bufWidth, int bufHeight, double *out,
                          uint8_t *mask) const
    {
        readBandAs(band1, x, y, width, height, bufWidth, bufHeight, out, mask);
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
