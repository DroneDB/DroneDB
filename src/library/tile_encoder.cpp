/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "tile_encoder.h"
#include "stringutil.h"

#include <cstring>
#include <limits>

#include "exceptions.h"
#include "gdal_raii.h"
#include "mio.h"
#include "tilers_call.h"

namespace ddb
{
    namespace
    {
        // The pixels of a RasterImage interleaved (its colour planes, then its alpha), in `buffer`
        // unless its one plane already is the image
        InterleavedImage interleave(const RasterImage &image, std::vector<uint8_t> &buffer)
        {
            const int channels = image.colorBands + (image.hasAlpha() ? 1 : 0);
            if (channels == 1)
                return {image.color.data(), image.width, image.height, 1};
            const size_t pixels = image.pixels();
            buffer.resize(pixels * static_cast<size_t>(channels));
            for (int c = 0; c < channels; c++)
            {
                const uint8_t *plane = c < image.colorBands ? image.band(c) : image.alpha.data();
                uint8_t *out = buffer.data() + c;
                for (size_t i = 0; i < pixels; i++, out += channels)
                    *out = plane[i];
            }
            return {buffer.data(), image.width, image.height, channels};
        }

        // The bytes of image encoded by ddb-tilers (released with ddbtFree)
        tilers::Bytes encodeBytes(const InterleavedImage &image, const EncodeOptions &options, int &size)
        {
            if (image.pixels == nullptr || image.width <= 0 || image.height <= 0)
                throw InvalidArgsException("Invalid image buffer");
            std::vector<uint8_t> palette;
            if (image.palette != nullptr)
                for (const auto &entry : *image.palette)
                    palette.insert(palette.end(), entry.begin(), entry.end());
            const ddbtImage view{image.pixels,
                                 static_cast<uint32_t>(image.width),
                                 static_cast<uint32_t>(image.height),
                                 static_cast<uint32_t>(image.channels),
                                 0,
                                 palette.empty() ? nullptr : palette.data(),
                                 static_cast<uint32_t>(palette.size() / 4)};
            uint8_t *raw = nullptr;
            tilers::check(
                ddbtEncodeImage(&view, imageFormatExtension(options.format), options.quality, &raw, &size));
            return tilers::Bytes(raw);
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

    RasterImage::RasterImage(int width, int height, int colorBands, bool withAlpha)
        : width(width), height(height), colorBands(colorBands)
    {
        if (width <= 0 || height <= 0 || (colorBands != 1 && colorBands != 3))
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

        std::vector<uint8_t> buffer;
        int size = 0;
        const tilers::Bytes bytes = encodeBytes(interleave(image, buffer), options, size);
        store(bytes.get(), static_cast<size_t>(size), path, outBuffer, outBufferSize);
    }

    void TileEncoder::encode(const RasterImage &image, const EncodeOptions &options, uint8_t **outBuffer,
                             int *outBufferSize)
    {
        if (outBuffer == nullptr || outBufferSize == nullptr)
            throw InvalidArgsException("Output buffer pointers are null");
        write(image, options, "", outBuffer, outBufferSize);
    }

    void TileEncoder::encode(const InterleavedImage &image, const EncodeOptions &options, uint8_t **outBuffer,
                             int *outBufferSize)
    {
        if (outBuffer == nullptr || outBufferSize == nullptr)
            throw InvalidArgsException("Output buffer pointers are null");
        int size = 0;
        const tilers::Bytes bytes = encodeBytes(image, options, size);
        store(bytes.get(), static_cast<size_t>(size), "", outBuffer, outBufferSize);
    }

    void TileEncoder::store(const uint8_t *data, size_t size, const std::string &path, uint8_t **outBuffer,
                            int *outBufferSize)
    {
        if (outBuffer != nullptr)
        {
            if (outBufferSize == nullptr)
                throw InvalidArgsException("Output buffer size pointer is null");
            if (size > static_cast<size_t>(std::numeric_limits<int>::max()))
                throw AppException("Tile of " + std::to_string(size) + " bytes is too large");
            auto *copy = static_cast<uint8_t *>(VSIMalloc(size == 0 ? 1 : size));
            if (copy == nullptr)
                throw AppException("Cannot allocate " + std::to_string(size) + " bytes");
            if (size > 0)
                std::memcpy(copy, data, size);
            *outBuffer = copy;
            *outBufferSize = static_cast<int>(size);
            return;
        }

        // A tile written to a file appears whole or not at all: the tile cache serves any file that
        // exists, and requests for the same tile run concurrently. It is published by renaming a
        // sibling onto the path (a failed write keeps the previous file).
        if (!path.starts_with("/vsimem/"))
        {
            io::writeFileAtomic(io::fromUtf8(path), std::string(reinterpret_cast<const char *>(data), size), "tile");
            return;
        }

        VsiFilePtr f(VSIFOpenL(path.c_str(), "wb"));
        if (!f)
            throw GDALException("Cannot write " + path);
        if (VSIFWriteL(data, 1, size, f.get()) != size)
            throw GDALException("Cannot write " + path);
        // The close writes what the stream still holds
        if (VSIFCloseL(f.release()) != 0)
            throw GDALException("Cannot write " + path);
    }
}
