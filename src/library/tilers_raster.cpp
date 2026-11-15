/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "tilers_raster.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>

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

        // A nodata value ddb-tilers reports: a number, or null for NaN (JSON has no NaN; the member is
        // there only when there is a nodata value)
        std::optional<double> optionalNodata(const json &object)
        {
            const auto it = object.find("nodata");
            if (it == object.end())
                return std::nullopt;
            return it->is_null() ? std::numeric_limits<double>::quiet_NaN() : it->get<double>();
        }

        // A GDAL data type by its name ("Byte", "Float16"...)
        GDALDataType dataTypeOf(const std::string &type)
        {
            const GDALDataType t = GDALGetDataTypeByName(type.c_str());
            if (t == GDT_Unknown)
                throw GDALException("Unsupported data type: " + type);
            return t;
        }
    }

    Raster::Raster(ddbtRaster *handle, fs::path file) : handle_(handle), file_(std::move(file))
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
        for (const auto &o : info.at("overviews"))
            overviews_.push_back({o.at(0).get<int>(), o.at(1).get<int>()});
        if (info.contains("geoTransform"))
            geoTransform_ = info.at("geoTransform").get<std::array<double, 6>>();
        wkt_ = info.value("wkt", std::string());
        for (const auto &b : info.at("bands"))
        {
            Band band;
            band.colorInterp = b.at("colorInterp").get<std::string>();
            band.nodata = optionalNodata(b);
            band.scale = optionalNumber(b, "scale");
            band.offset = optionalNumber(b, "offset");
            band.unit = b.value("unit", std::string());
            band.description = b.value("description", std::string());
            if (b.contains("colorTable"))
                band.colorTable = b.at("colorTable").get<std::vector<std::array<uint8_t, 4>>>();
            for (const auto &item : b.value("metadata", json::array()))
            {
                const std::string entry = item.get<std::string>();
                const size_t eq = entry.find('=');
                if (eq != std::string::npos)
                    band.metadata.emplace_back(entry.substr(0, eq), entry.substr(eq + 1));
            }
            bands_.push_back(std::move(band));
        }
    }

    Raster::~Raster() { ddbtRasterClose(handle_); }

    gdalmeta::RasterMetadata Raster::metadata() const
    {
        char *text = nullptr;
        check(ddbtRasterMetadata(handle_, &text));
        const tilers::String owned(text);
        const json report = json::parse(text);
        const auto domainsOf = [](const json &domains) {
            std::vector<gdalmeta::Domain> out;
            for (const auto &d : domains)
                out.push_back(gdalmeta::Domain{d.at("name").get<std::string>(),
                                               d.at("items").get<std::vector<std::string>>()});
            return out;
        };
        gdalmeta::RasterMetadata metadata;
        metadata.domains = domainsOf(report.at("domains"));
        for (const auto &band : report.at("bands"))
        {
            std::vector<std::string> items;
            for (const auto &d : domainsOf(band.at("domains")))
                if (d.name.empty())
                    items = d.items;
            metadata.bandItems.push_back(std::move(items));
        }
        return metadata;
    }

    std::unique_ptr<Raster> Raster::openWithFlags(const fs::path &file, unsigned int flags)
    {
        ddbtRaster *handle = nullptr;
        check(ddbtRasterOpenEx(io::toUtf8(file).c_str(), flags, &handle));
        return std::unique_ptr<Raster>(new Raster(handle, file));
    }

    std::unique_ptr<Raster> Raster::open(const fs::path &file) { return openWithFlags(file, 0); }

    std::unique_ptr<Raster> Raster::openPrivate(const fs::path &file)
    {
        return openWithFlags(file, DDBT_RASTER_OPEN_PRIVATE);
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
        return std::unique_ptr<Raster>(new Raster(handle, artefact));
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

    namespace
    {
        template <typename T> constexpr ddbtSampleType sampleTypeOf()
        {
            return std::is_same_v<T, float> ? DDBT_SAMPLE_FLOAT32 : DDBT_SAMPLE_FLOAT64;
        }
    }

    template <typename T, typename Read>
    void Raster::readWithMask(std::span<const int> bands, size_t count, T *out, uint8_t *mask, const Read &read) const
    {
        const bool masked =
            mask != nullptr && std::any_of(bands.begin(), bands.end(), [this](int b) { return hasMask(b); });
        read(bands, sampleTypeOf<T>(), out, masked && mask_ == Mask::PerDataset ? mask : nullptr);
        if (!masked || mask_ == Mask::PerDataset)
            return;
        // The alpha band in its own type, read on its own (its blocks are in the block cache when they hold
        // the bands too): Byte as it is, UInt16 rescaled to 0..255 with any alpha above 0 kept above 0
        // (GDALRescaledAlphaBand); GDAL takes no other type
        const int alpha[1] = {alphaBand_};
        if (dataType_ != "UInt16")
        {
            read(alpha, DDBT_SAMPLE_NATIVE, mask, nullptr);
            return;
        }
        std::vector<uint16_t> samples(count);
        read(alpha, DDBT_SAMPLE_NATIVE, samples.data(), nullptr);
        for (size_t i = 0; i < count; i++)
        {
            const uint16_t a = samples[i];
            mask[i] = static_cast<uint8_t>(a > 0 && a < 257 ? 1 : a / 257);
        }
    }

    void Raster::readMasked(int x, int y, int width, int height, int bufWidth, int bufHeight,
                            std::span<const int> bands, float *out, uint8_t *mask) const
    {
        readWithMask(bands, static_cast<size_t>(bufWidth) * static_cast<size_t>(bufHeight), out, mask,
                     [&](std::span<const int> b, ddbtSampleType type, void *o, uint8_t *m) {
                         check(ddbtRasterRead(handle_, x, y, width, height, bufWidth, bufHeight, b.data(),
                                              static_cast<int>(b.size()), type, o, m));
                     });
    }

    void Raster::readMasked(int x, int y, int width, int height, int bufWidth, int bufHeight,
                            std::span<const int> bands, double *out, uint8_t *mask) const
    {
        readWithMask(bands, static_cast<size_t>(bufWidth) * static_cast<size_t>(bufHeight), out, mask,
                     [&](std::span<const int> b, ddbtSampleType type, void *o, uint8_t *m) {
                         check(ddbtRasterRead(handle_, x, y, width, height, bufWidth, bufHeight, b.data(),
                                              static_cast<int>(b.size()), type, o, m));
                     });
    }

    void Raster::readBand(int band1, int x, int y, int width, int height, int bufWidth, int bufHeight, float *out,
                          uint8_t *mask) const
    {
        const int bands[1] = {band1};
        readMasked(x, y, width, height, bufWidth, bufHeight, bands, out, mask);
    }

    void Raster::readBand(int band1, int x, int y, int width, int height, int bufWidth, int bufHeight, double *out,
                          uint8_t *mask) const
    {
        const int bands[1] = {band1};
        readMasked(x, y, width, height, bufWidth, bufHeight, bands, out, mask);
    }

    void Raster::readLevel(int level, int x, int y, int width, int height, std::span<const int> bands, double *out,
                           uint8_t *mask) const
    {
        readWithMask(bands, static_cast<size_t>(width) * static_cast<size_t>(height), out, mask,
                     [&](std::span<const int> b, ddbtSampleType type, void *o, uint8_t *m) {
                         check(ddbtRasterReadLevel(handle_, level, x, y, width, height, b.data(),
                                                   static_cast<int>(b.size()), type, o, m));
                     });
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

    size_t WarpedImage::sampleSize() const
    {
        return static_cast<size_t>(GDALGetDataTypeSizeBytes(dataTypeOf(dataType)));
    }

    const uint8_t *WarpedImage::band(int band1) const
    {
        return pixels.data() + static_cast<size_t>(band1 - 1) * static_cast<size_t>(width) * height * sampleSize();
    }

    template <typename T> void WarpedImage::readBand(int band1, std::vector<T> &out) const
    {
        const size_t count = static_cast<size_t>(width) * height;
        out.resize(count);
        const uint8_t *in = band(band1);
        // As GDAL reads gdalwarp's MEM dataset: GDALCopyWords from the type of the warp
        const GDALDataType from = dataTypeOf(dataType);
        const GDALDataType to = std::is_same_v<T, float> ? GDT_Float32 : GDT_Float64;
        GDALCopyWords64(in, from, GDALGetDataTypeSizeBytes(from), out.data(), to, static_cast<int>(sizeof(T)),
                        static_cast<GPtrDiff_t>(count));
    }
    template void WarpedImage::readBand<float>(int, std::vector<float> &) const;
    template void WarpedImage::readBand<double>(int, std::vector<double> &) const;

    WarpedImage warp(const fs::path &file, const WarpRequest &request)
    {
        json options = {{"targetCrs", request.targetCrs}, {"extent", request.extent},
                        {"width", request.width},         {"height", request.height},
                        {"resampling", request.nearest ? "nearest" : "bilinear"}};
        if (!request.extentCrs.empty())
            options["extentCrs"] = request.extentCrs;
        if (!request.bands.empty())
            options["bands"] = request.bands;
        if (request.dstAlpha)
            options["dstAlpha"] = true;
        if (request.dstNodata)
            options["dstNodata"] = *request.dstNodata;
        if (!request.outputType.empty())
            options["outputType"] = request.outputType;
        if (request.noSrcAlpha)
            options["noSrcAlpha"] = true;
        if (request.setColorInterp)
            options["setColorInterp"] = true;
        char *infoText = nullptr;
        uint8_t *samples = nullptr;
        size_t size = 0;
        check(ddbtWarpRaster(io::toUtf8(file).c_str(), options.dump().c_str(), &infoText, &samples, &size));
        const String ownedInfo(infoText);
        const Bytes ownedSamples(samples);
        const json info = json::parse(infoText);
        WarpedImage image;
        image.width = info.at("width").get<int>();
        image.height = info.at("height").get<int>();
        image.dataType = info.at("type").get<std::string>();
        image.geoTransform = info.at("geoTransform").get<std::array<double, 6>>();
        image.wkt = info.value("wkt", std::string());
        image.colorInterp = info.at("colorInterp").get<std::vector<std::string>>();
        image.nodata = optionalNodata(info);
        image.alphaBand = info.value("alphaBand", 0);
        if (info.contains("colorTable"))
            image.colorTable = info.at("colorTable").get<std::vector<std::array<uint8_t, 4>>>();
        image.pixels.assign(samples, samples + size);
        return image;
    }

    void writeGeoTiff(const WarpedImage &image, const std::string &crs, const fs::path &output)
    {
        // Floating-point samples (Float16 among them) with the floating-point predictor
        const bool floating = GDALDataTypeIsFloating(dataTypeOf(image.dataType)) != 0;
        json options = {{"width", image.width},
                        {"height", image.height},
                        {"type", image.dataType},
                        {"colorInterp", image.colorInterp},
                        {"geoTransform", image.geoTransform},
                        {"crs", crs},
                        {"codec", "deflate"},
                        {"predictor", floating ? 3 : 2}};
        if (image.nodata)
            options["nodata"] = std::isnan(*image.nodata) ? json("nan") : json(*image.nodata);
        if (!image.colorTable.empty())
            options["colorTable"] = image.colorTable;
        // The host fills each window from the planes of the image, pixel-interleaved
        struct Fill
        {
            const WarpedImage &image;
            static int fill(void *self, int x, int y, int width, int height, uint8_t *out, size_t stride)
            {
                const WarpedImage &im = static_cast<Fill *>(self)->image;
                const size_t ss = im.sampleSize(), bands = static_cast<size_t>(im.bandCount());
                for (int r = 0; r < height; r++)
                    for (size_t b = 0; b < bands; b++)
                    {
                        const uint8_t *in = im.band(static_cast<int>(b) + 1) +
                                            (static_cast<size_t>(y + r) * im.width + static_cast<size_t>(x)) * ss;
                        uint8_t *px = out + static_cast<size_t>(r) * stride + b * ss;
                        for (int c = 0; c < width; c++)
                            std::memcpy(px + static_cast<size_t>(c) * bands * ss, in + static_cast<size_t>(c) * ss, ss);
                    }
                return 0;
            }
        } fill{image};
        check(ddbtWriteRaster(io::toUtf8(output).c_str(), options.dump().c_str(), &Fill::fill, &fill, nullptr,
                              nullptr));
    }
}
