/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "tile_encoder.h"
#include "stringutil.h"

#include <utility>
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>

#include "exceptions.h"
#include "gdal_raii.h"

namespace ddb
{
    namespace
    {
        bool allEqual(const std::vector<uint8_t> &plane, uint8_t value)
        {
            return std::all_of(plane.begin(), plane.end(), [value](uint8_t v) { return v == value; });
        }

        const char *driverName(ImageFormat format)
        {
            switch (format)
            {
            case ImageFormat::Jpeg:
                return "JPEG";
            case ImageFormat::Webp:
                return "WEBP";
            default:
                return "PNG";
            }
        }

        // Builds the MEM dataset the format can store: JPEG takes no alpha (transparent
        // pixels become white), WebP takes only RGB/RGBA (grey is expanded), an opaque
        // alpha plane is dropped.
        GDALDatasetUniquePtr toMemDataset(const RasterImage &image, ImageFormat format)
        {
            GDALDriverH memDrv = GDALGetDriverByName("MEM");
            if (memDrv == nullptr)
                throw GDALException("MEM driver not available");

            const bool keepAlpha = format != ImageFormat::Jpeg && image.hasAlpha() && !allEqual(image.alpha, 255);
            const int colorBands = format == ImageFormat::Webp ? 3 : image.colorBands;
            const int bands = colorBands + (keepAlpha ? 1 : 0);
            const size_t pixels = image.pixels();

            auto owned = adoptDataset(
                GDALCreate(memDrv, "", image.width, image.height, bands, GDT_Byte, nullptr));
            if (!owned)
                throw GDALException("Cannot create in-memory image dataset");
            GDALDatasetH ds = datasetHandle(owned);

            std::vector<uint8_t> plane;
            for (int b = 0; b < colorBands; b++)
            {
                const uint8_t *src = image.band(std::min(b, image.colorBands - 1));
                if (format == ImageFormat::Jpeg && image.hasAlpha())
                {
                    plane.assign(src, src + pixels);
                    for (size_t i = 0; i < pixels; i++)
                        if (image.alpha[i] == 0)
                            plane[i] = 255;
                    src = plane.data();
                }

                if (GDALRasterIO(GDALGetRasterBand(ds, b + 1), GF_Write, 0, 0, image.width, image.height,
                                 const_cast<uint8_t *>(src), image.width, image.height, GDT_Byte, 0, 0) != CE_None)
                    throw GDALException("Cannot write image band " + std::to_string(b + 1));
            }

            if (keepAlpha)
            {
                const GDALRasterBandH alphaBand = GDALGetRasterBand(ds, bands);
                if (GDALRasterIO(alphaBand, GF_Write, 0, 0, image.width, image.height,
                                 const_cast<uint8_t *>(image.alpha.data()), image.width, image.height,
                                 GDT_Byte, 0, 0) != CE_None)
                    throw GDALException("Cannot write image alpha");
                GDALSetRasterColorInterpretation(alphaBand, GCI_AlphaBand);
            }

            return owned;
        }

        void encodeToPath(const RasterImage &image, const EncodeOptions &options, const std::string &path)
        {
            GDALDriverH drv = GDALGetDriverByName(driverName(options.format));
            if (drv == nullptr)
                throw GDALException(std::string(driverName(options.format)) + " driver not available");

            const auto mem = toMemDataset(image, options.format);
            CPLStringList opts;
            for (const auto &o : imageCreationOptions(options))
                opts.AddString(o.c_str());
            // A failed encode leaves no partial /vsimem/ file
            VsiMemFile partial(path.starts_with("/vsimem/") ? path : std::string());
            const auto out = adoptDataset(
                GDALCreateCopy(drv, path.c_str(), datasetHandle(mem), FALSE, opts.List(), nullptr, nullptr));
            if (!out)
                throw GDALException("Cannot encode image to " + path + ": " + CPLGetLastErrorMsg());
            partial.release();
        }

        // Hands the encoded bytes of the /vsimem/ file to the caller (VSIFree); the file is gone
        void stealBuffer(const std::string &path, uint8_t **outBuffer, int *outBufferSize)
        {
            int size = 0;
            auto data = stealMemFileBuffer(path, size);
            if (!data)
                throw GDALException("Cannot read encoded image " + path);
            *outBuffer = data.release();
            *outBufferSize = size;
        }

        // Encoded bytes of fully transparent images, per format and size
        const std::vector<uint8_t> &blankImageBytes(const RasterImage &shape, const EncodeOptions &options)
        {
            static std::mutex mutex;
            static std::map<std::string, std::vector<uint8_t>> cache;

            const std::string key = std::to_string(static_cast<int>(options.format)) + ":" +
                                    std::to_string(options.quality) + ":" + std::to_string(options.pngZLevel) + ":" +
                                    std::to_string(shape.width) + "x" + std::to_string(shape.height) + "x" +
                                    std::to_string(shape.colorBands);

            std::lock_guard<std::mutex> lock(mutex);
            const auto it = cache.find(key);
            if (it != cache.end())
                return it->second;

            const VsiMemFile file =
                VsiMemFile::unique("ddb-blank-", std::string(".") + imageFormatExtension(options.format));
            // Pixels under alpha 0 are not shown: encode zeros so that the bytes are the
            // same for every blank image of this size
            // Encoded once, so at the smallest size
            EncodeOptions blankOptions = options;
            blankOptions.pngZLevel = 9;
            encodeToPath(RasterImage(shape.width, shape.height, shape.colorBands, true), blankOptions,
                         file.path());
            uint8_t *data = nullptr;
            int size = 0;
            stealBuffer(file.path(), &data, &size);
            const VsiFreePtr<uint8_t> owned(data);
            std::vector<uint8_t> bytes(data, data + size);
            return cache.emplace(key, std::move(bytes)).first->second;
        }

        void writeBytes(const std::vector<uint8_t> &bytes, const std::string &path, uint8_t **outBuffer,
                        int *outBufferSize)
        {
            if (outBuffer != nullptr)
            {
                auto *data = static_cast<uint8_t *>(VSIMalloc(bytes.size()));
                if (data == nullptr)
                    throw AppException("Cannot allocate " + std::to_string(bytes.size()) + " bytes");
                std::memcpy(data, bytes.data(), bytes.size());
                *outBuffer = data;
                *outBufferSize = static_cast<int>(bytes.size());
                return;
            }

            const VsiFilePtr f(VSIFOpenL(path.c_str(), "wb"));
            if (!f)
                throw GDALException("Cannot write " + path);
            if (VSIFWriteL(bytes.data(), 1, bytes.size(), f.get()) != bytes.size())
                throw GDALException("Cannot write " + path);
        }
    }

    ImageFormat parseImageFormat(const std::string &format)
    {
        std::string f = format;
        str::toLower(f);
        if (f.empty() || f == "png" || f == "image/png")
            return ImageFormat::Png;
        if (f == "jpeg" || f == "jpg" || f == "image/jpeg" || f == "image/jpg")
            return ImageFormat::Jpeg;
        if (f == "webp" || f == "image/webp")
            return ImageFormat::Webp;
        throw InvalidArgsException("Unsupported image format: " + format);
    }

    const char *imageFormatExtension(ImageFormat format)
    {
        switch (format)
        {
        case ImageFormat::Jpeg:
            return "jpg";
        case ImageFormat::Webp:
            return "webp";
        default:
            return "png";
        }
    }

    std::vector<std::string> imageCreationOptions(const EncodeOptions &options)
    {
        switch (options.format)
        {
        case ImageFormat::Jpeg:
            return {"QUALITY=" + std::to_string(options.quality > 0 ? std::min(options.quality, 100) : 75)};
        case ImageFormat::Webp:
            // libwebp effort 2 of 6: half the encode time of the default (4) for ~1.5 % more bytes
            return {"QUALITY=" + std::to_string(options.quality > 0 ? std::min(options.quality, 100) : 80),
                    "LOSSLESS=FALSE", "METHOD=2"};
        default:
            return {"ZLEVEL=" + std::to_string(std::clamp(options.pngZLevel, 1, 9))};
        }
    }

    RasterImage::RasterImage(int width, int height, int colorBands, bool withAlpha)
        : width(width), height(height), colorBands(colorBands)
    {
        if (width <= 0 || height <= 0 || colorBands < 1 || colorBands > 3)
            throw InvalidArgsException("Invalid image size " + std::to_string(width) + "x" +
                                       std::to_string(height) + "x" + std::to_string(colorBands));
        color.assign(pixels() * colorBands, 0);
        if (withAlpha)
            alpha.assign(pixels(), 0);
    }

    void TileEncoder::write(const RasterImage &image, const EncodeOptions &options, const std::string &path,
                            uint8_t **outBuffer, int *outBufferSize)
    {
        if (image.width <= 0 || image.height <= 0 || image.colorBands < 1 ||
            image.color.size() < image.pixels() * image.colorBands ||
            (image.hasAlpha() && image.alpha.size() < image.pixels()))
            throw InvalidArgsException("Invalid image buffer");
        if (outBuffer != nullptr && outBufferSize == nullptr)
            throw InvalidArgsException("Output buffer size pointer is null");

        if (image.hasAlpha() && allEqual(image.alpha, 0))
        {
            writeBytes(blankImageBytes(image, options), path, outBuffer, outBufferSize);
            return;
        }

        encodeToPath(image, options, path);
        if (outBuffer != nullptr)
            stealBuffer(path, outBuffer, outBufferSize);
    }

    void TileEncoder::encode(const RasterImage &image, const EncodeOptions &options, uint8_t **outBuffer,
                             int *outBufferSize)
    {
        if (outBuffer == nullptr || outBufferSize == nullptr)
            throw InvalidArgsException("Output buffer pointers are null");
        const VsiMemFile file =
            VsiMemFile::unique("ddb-enc-", std::string(".") + imageFormatExtension(options.format));
        write(image, options, file.path(), outBuffer, outBufferSize);
    }
}
