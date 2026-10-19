/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef POINT_RASTERIZER_H
#define POINT_RASTERIZER_H

#include <cstdint>
#include <vector>

#include "ddb_export.h"
#include "tile_encoder.h"

namespace ddb
{
    /// A chunk of points, column by column. Colours are the raw 8 or 16-bit values.
    struct PointBatch
    {
        std::vector<double> x;
        std::vector<double> y;
        std::vector<double> z;
        std::vector<uint16_t> red;
        std::vector<uint16_t> green;
        std::vector<uint16_t> blue;

        size_t size() const { return x.size(); }
        DDB_DLL void clear();
        DDB_DLL void reserve(size_t n);
    };

    /// Maps world coordinates to the pixels of a z-buffer of width x height:
    ///   px = round(((x - originX) + shiftX) * scaleX + offsetX)
    ///   py = height - 1 - round(((y - originY) + shiftY) * scaleY + offsetY)
    struct PixelMapping
    {
        double originX = 0, shiftX = 0, scaleX = 1, offsetX = 0;
        double originY = 0, shiftY = 0, scaleY = 1, offsetY = 0;
        int width = 0;
        int height = 0;
    };

    /**
     * Draws points as discs on a square image, the highest point of each z-buffer pixel
     * winning, for point-cloud tiles and thumbnails.
     *
     * The z-buffer can be larger than the image (tiles pad it by the disc radius so
     * discs centred just outside still reach the edge): a point at z-buffer pixel
     * (px, py) is drawn at image pixel (px - drawOffset, py - drawOffset).
     * Colours are kept as given; when any point added had a component above 255 the
     * cloud is taken as 16-bit and the image gets the high byte of every colour.
     */
    class PointRasterizer
    {
    public:
        DDB_DLL PointRasterizer(int imageSize, const PixelMapping &mapping, int drawOffset = 0, int radius = 2);

        /// Adds points whose x, y are in the coordinates of the mapping
        DDB_DLL void add(const PointBatch &points);

        /// Points that won their z-buffer pixel and were drawn
        size_t drawn() const { return drawn_; }

        /// The image: 3 colour bands and alpha (opaque where a disc was drawn)
        DDB_DLL RasterImage toImage() const;

    private:
        int imageSize_;
        PixelMapping mapping_;
        int drawOffset_;
        int radius_;
        std::vector<uint16_t> color_; // 3 planes of imageSize²
        std::vector<uint8_t> alpha_;
        std::vector<float> zBuffer_;
        bool sixteenBit_ = false;
        size_t drawn_ = 0;

        void drawDisc(int px, int py, uint16_t r, uint16_t g, uint16_t b);
    };
}

#endif // POINT_RASTERIZER_H
