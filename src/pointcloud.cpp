/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <pdal/StageFactory.hpp>
#include "pointcloud.h"
#include "logger.h"

bool getPointCloudInfo(const std::string &filename, PointCloudInfo &info){
    pdal::StageFactory factory;
    std::string driver = factory.inferReaderDriver(filename);
    if (driver.empty()){
        LOGD << "Can't infer point cloud reader from " << filename;
        return false;
    }

    pdal::Stage *s = factory.createStage(driver);
    pdal::Options opts;
    opts.add("filename", filename);
    s->setOptions(opts);
    pdal::QuickInfo qi = s->preview();
    if (!qi.valid()){
        LOGD << "Cannot get quick info for point cloud " << filename;
        return false;
    }

    info.pointCount = qi.m_pointCount;
    return true;
}


json PointCloudInfo::toJSON(){
    json j;
    j["pointCount"] = pointCount;
    return j;
}
