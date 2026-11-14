/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef POINT_BATCH_H
#define POINT_BATCH_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace pdal
{
    class Stage;
}

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
        void clear();
        void reserve(size_t n);
    };

    /// Runs the (streamable) PDAL pipeline that ends at tail without holding its points:
    /// they go to sink in batches of up to chunkSize. Batches carry the Red/Green/Blue
    /// dimensions when the pipeline has them, zeros otherwise.
    void forEachPointBatch(pdal::Stage &tail, const std::function<void(PointBatch &)> &sink,
                           size_t chunkSize = 16384);
}

#endif // POINT_BATCH_H
