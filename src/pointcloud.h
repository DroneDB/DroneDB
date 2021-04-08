/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef POINTCLOUD_H
#define POINTCLOUD_H

#include <string>
#include "json.h"

struct PointCloudInfo{
    size_t pointCount;

    json toJSON();
};

bool getPointCloudInfo(const std::string &filename, PointCloudInfo &info);

#endif // POINTCLOUD_H
