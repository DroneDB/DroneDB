/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef TILE_ENCODER_H
#define TILE_ENCODER_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>


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
    ImageFormat parseImageFormat(const std::string &format);

    /// File extension of format, without the dot ("png", "jpg", "webp")
    const char *imageFormatExtension(ImageFormat format);

    /// An 8-bit image ready to encode: 1 (grey) or 3 (RGB) colour planes (band-sequential) and an
    /// optional alpha plane. Pixels start fully transparent black.
    struct RasterImage
    {
        int width = 0;
        int height = 0;
        int colorBands = 0;
        std::vector<uint8_t> color; // colorBands planes of width * height
        std::vector<uint8_t> alpha; // width * height, empty when the image is opaque

        RasterImage() = default;
        RasterImage(int width, int height, int colorBands, bool withAlpha);

        size_t pixels() const { return static_cast<size_t>(width) * height; }
        uint8_t *band(int index) { return color.data() + pixels() * index; }
        const uint8_t *band(int index) const { return color.data() + pixels() * index; }
        bool hasAlpha() const { return !alpha.empty(); }
    };

    /// A view of an 8-bit, pixel-interleaved image (not owned): 1 channel (grey, or the indices of
    /// `palette` when it is set), 2 (grey and alpha), 3 (RGB) or 4 (RGBA), rows of width * channels
    /// bytes. A palette has 1 to 256 RGBA entries.
    struct InterleavedImage
    {
        const uint8_t *pixels = nullptr;
        int width = 0;
        int height = 0;
        int channels = 0;
        const std::vector<std::array<uint8_t, 4>> *palette = nullptr;
    };

    struct EncodeOptions
    {
        ImageFormat format = ImageFormat::Png;
        /// JPEG/WebP quality (1-100); 0 = the format default (JPEG 75, WebP 80)
        int quality = 0;
    };

    /**
     * Encodes images to PNG, JPEG or WebP with one set of rules for every producer of preview
     * images (tiles, thumbnails, map renders): the encoders of ddb-tilers (ddbtEncodeImage), as the
     * tiles of rasters and point clouds are encoded.
     *
     * - PNG at zlib level 1: tiles are previews served on every map pan, and photographic tiles
     *   hardly compress better at higher levels.
     * - An alpha channel that is fully opaque is dropped (PNG/WebP RGB instead of RGBA).
     * - JPEG has no alpha: transparent pixels are written white, as tiles always were.
     * - WebP is always RGB or RGBA (grey is expanded).
     * - A fully transparent image of a given size and format gives the same bytes every time
     *   (blank tiles outside a raster).
     */
    class TileEncoder
    {
    public:
        /// Encodes image to path (a file, which appears whole, or a /vsimem/ path), or into a new
        /// *outBuffer when it is set (free with VSIFree / DDBVSIFree; path is then not written).
        static void write(const RasterImage &image, const EncodeOptions &options,
                          const std::string &path, uint8_t **outBuffer = nullptr,
                          int *outBufferSize = nullptr);

        /// Encodes image into a new buffer (free with VSIFree / DDBVSIFree)
        static void encode(const RasterImage &image, const EncodeOptions &options,
                           uint8_t **outBuffer, int *outBufferSize);

        /// Encodes an interleaved image into a new buffer (free with VSIFree / DDBVSIFree)
        static void encode(const InterleavedImage &image, const EncodeOptions &options, uint8_t **outBuffer,
                           int *outBufferSize);

        /// Stores bytes already encoded as write stores an image: into a new *outBuffer when it is
        /// set (free with VSIFree / DDBVSIFree), else into path (a file appears whole, by a rename;
        /// a /vsimem/ path is written in place).
        static void store(const uint8_t *data, size_t size, const std::string &path, uint8_t **outBuffer = nullptr,
                          int *outBufferSize = nullptr);
    };
}

#endif // TILE_ENCODER_H
