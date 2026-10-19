/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef TILE_ENCODER_H
#define TILE_ENCODER_H

#include <cstdint>
#include <string>
#include <vector>

#include "ddb_export.h"

namespace ddb
{
    enum class ImageFormat
    {
        Png,
        Jpeg,
        Webp
    };

    /// Parses "png", "jpeg"/"jpg", "webp" or their MIME types (case-insensitive).
    /// Throws InvalidArgsException for anything else.
    DDB_DLL ImageFormat parseImageFormat(const std::string &format);

    /// File extension of format, without the dot ("png", "jpg", "webp")
    DDB_DLL const char *imageFormatExtension(ImageFormat format);

    /// An 8-bit image ready to encode: 1 to 3 colour planes (band-sequential) and an
    /// optional alpha plane. Pixels start fully transparent black.
    struct RasterImage
    {
        int width = 0;
        int height = 0;
        int colorBands = 0;
        std::vector<uint8_t> color; // colorBands planes of width * height
        std::vector<uint8_t> alpha; // width * height, empty when the image is opaque

        DDB_DLL RasterImage() = default;
        DDB_DLL RasterImage(int width, int height, int colorBands, bool withAlpha);

        size_t pixels() const { return static_cast<size_t>(width) * height; }
        uint8_t *band(int index) { return color.data() + pixels() * index; }
        const uint8_t *band(int index) const { return color.data() + pixels() * index; }
        bool hasAlpha() const { return !alpha.empty(); }
    };

    struct EncodeOptions
    {
        ImageFormat format = ImageFormat::Png;
        /// JPEG/WebP quality (1-100); 0 = the format default (JPEG 75, WebP 80)
        int quality = 0;
        /// PNG zlib level (1-9). Tiles are previews served on every map pan: level 1 is
        /// the cheapest, and photographic tiles hardly compress better at the default (6).
        int pngZLevel = 1;
    };

    /// GDAL creation options of the PNG, JPEG or WEBP driver for options, shared by every
    /// producer of preview images (tiles, thumbnails, map renders)
    DDB_DLL std::vector<std::string> imageCreationOptions(const EncodeOptions &options);

    /**
     * Encodes RasterImage buffers to PNG, JPEG or WebP with one set of options for
     * every tile and thumbnail producer.
     *
     * - An alpha plane that is fully opaque is dropped (PNG/WebP RGB instead of RGBA).
     * - JPEG has no alpha: transparent pixels are written white, as tiles always were.
     * - A fully transparent image of a given size and format is encoded once and its
     *   bytes are reused (blank tiles outside a raster).
     */
    class TileEncoder
    {
    public:
        /// Encodes image to path (a file or a /vsimem/ path). When outBuffer is set the
        /// result is moved out of path (which must be a /vsimem/ path) into *outBuffer,
        /// to be freed with VSIFree / DDBVSIFree.
        DDB_DLL static void write(const RasterImage &image, const EncodeOptions &options,
                                  const std::string &path, uint8_t **outBuffer = nullptr,
                                  int *outBufferSize = nullptr);

        /// Encodes image into a new buffer (free with VSIFree / DDBVSIFree)
        DDB_DLL static void encode(const RasterImage &image, const EncodeOptions &options,
                                   uint8_t **outBuffer, int *outBufferSize);
    };
}

#endif // TILE_ENCODER_H
