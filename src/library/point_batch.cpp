/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "point_batch.h"

#include "pdal_inc.h"
#include <pdal/PointRef.hpp>
#include <pdal/PointTable.hpp>
#include <pdal/Stage.hpp>

namespace ddb
{
    namespace
    {
        // A table that hands each chunk of streamed points to a sink before PDAL reuses it
        class BatchTable : public pdal::FixedPointTable
        {
            const std::function<void(PointBatch &)> &sink_;
            PointBatch batch_;

        public:
            BatchTable(pdal::point_count_t capacity, const std::function<void(PointBatch &)> &sink)
                : pdal::FixedPointTable(capacity), sink_(sink)
            {
                batch_.reserve(capacity);
            }

        protected:
            void reset() override
            {
                using pdal::Dimension::Id;
                const bool hasColor = layout()->hasDim(Id::Red) && layout()->hasDim(Id::Green) &&
                                      layout()->hasDim(Id::Blue);

                batch_.clear();
                pdal::PointRef point(*this, 0);
                for (pdal::PointId idx = 0; idx < numPoints(); idx++)
                {
                    if (skip(idx))
                        continue;
                    point.setPointId(idx);
                    batch_.x.push_back(point.getFieldAs<double>(Id::X));
                    batch_.y.push_back(point.getFieldAs<double>(Id::Y));
                    batch_.z.push_back(point.getFieldAs<double>(Id::Z));
                    batch_.red.push_back(hasColor ? point.getFieldAs<uint16_t>(Id::Red) : 0);
                    batch_.green.push_back(hasColor ? point.getFieldAs<uint16_t>(Id::Green) : 0);
                    batch_.blue.push_back(hasColor ? point.getFieldAs<uint16_t>(Id::Blue) : 0);
                }
                if (batch_.size() > 0)
                    sink_(batch_);

                pdal::FixedPointTable::reset();
            }
        };
    }

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

    void forEachPointBatch(pdal::Stage &tail, const std::function<void(PointBatch &)> &sink, size_t chunkSize)
    {
        BatchTable table(chunkSize, sink);
        tail.prepare(table);
        tail.execute(table);
    }
}
