/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "point_rasterizer.h"

#include <cmath>

#include "exceptions.h"

namespace ddb
{
    void PointBatch::clear()
    {
        x.clear();
        y.clear();
        z.clear();
        red.clear();
        green.clear();
        blue.clear();
    }

    void PointBatch::reserve(size_t n)
    {
        x.reserve(n);
        y.reserve(n);
        z.reserve(n);
        red.reserve(n);
        green.reserve(n);
        blue.reserve(n);
    }

    PointRasterizer::PointRasterizer(int imageSize, const PixelMapping &mapping, int drawOffset, int radius)
        : imageSize_(imageSize), mapping_(mapping), drawOffset_(drawOffset), radius_(radius)
    {
        if (imageSize <= 0 || mapping.width <= 0 || mapping.height <= 0 || radius < 0)
            throw InvalidArgsException("Invalid point rasterizer size");

        const size_t pixels = static_cast<size_t>(imageSize) * imageSize;
        color_.assign(pixels * 3, 0);
        alpha_.assign(pixels, 0);
        zBuffer_.assign(static_cast<size_t>(mapping.width) * mapping.height, -99999.0f);
    }

    void PointRasterizer::add(const PointBatch &points)
    {
        const size_t n = points.size();
        if (points.y.size() != n || points.z.size() != n || points.red.size() != n || points.green.size() != n ||
            points.blue.size() != n)
            throw InvalidArgsException("Point batch columns differ in size");

        const auto &m = mapping_;
        for (size_t i = 0; i < n; i++)
        {
            if (points.red[i] > 255 || points.green[i] > 255 || points.blue[i] > 255)
                sixteenBit_ = true;

            const double x = points.x[i];
            const double y = points.y[i];
            if (!std::isfinite(x) || !std::isfinite(y))
                continue; // Not transformable

            const int px = static_cast<int>(std::round(((x - m.originX) + m.shiftX) * m.scaleX + m.offsetX));
            const int py = m.height - 1 - static_cast<int>(std::round(((y - m.originY) + m.shiftY) * m.scaleY + m.offsetY));
            if (px < 0 || px >= m.width || py < 0 || py >= m.height)
                continue;

            float &depth = zBuffer_[static_cast<size_t>(py) * m.width + px];
            if (depth < points.z[i])
            {
                depth = static_cast<float>(points.z[i]);
                drawDisc(px - drawOffset_, py - drawOffset_, points.red[i], points.green[i], points.blue[i]);
                drawn_++;
            }
        }
    }

    void PointRasterizer::drawDisc(int px, int py, uint16_t r, uint16_t g, uint16_t b)
    {
        // Same footprint as the original drawCircle: offsets -radius..radius-1 within radius
        const size_t pixels = static_cast<size_t>(imageSize_) * imageSize_;
        const int r2 = radius_ * radius_;
        const int rr = radius_ << 1;
        const int area = r2 << 2;

        for (int i = 0; i < area; i++)
        {
            const int tx = (i % rr) - radius_;
            const int ty = (i / rr) - radius_;
            if (tx * tx + ty * ty > r2)
                continue;

            const int dx = px + tx;
            const int dy = py + ty;
            if (dx < 0 || dx >= imageSize_ || dy < 0 || dy >= imageSize_)
                continue;

            const size_t idx = static_cast<size_t>(dy) * imageSize_ + dx;
            color_[idx] = r;
            color_[idx + pixels] = g;
            color_[idx + pixels * 2] = b;
            alpha_[idx] = 255;
        }
    }

    RasterImage PointRasterizer::toImage() const
    {
        RasterImage image(imageSize_, imageSize_, 3, true);
        for (size_t i = 0; i < color_.size(); i++)
            image.color[i] = sixteenBit_ ? static_cast<uint8_t>(color_[i] >> 8) : static_cast<uint8_t>(color_[i]);
        image.alpha = alpha_;
        return image;
    }
}
