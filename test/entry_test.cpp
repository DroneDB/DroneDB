/* Copyright 2019 MasseranoLabs LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include "gtest/gtest.h"
#include "../libs/entry.h"

namespace {

TEST(calculateFootprint, Normal) {
    exif::SensorSize sensorSize(36, 24);
    exif::Focal focal(50, 0);
    double relAltitude = 100.0;
    exif::CameraOrientation cameraOri(-60, 0, 30);
    exif::GeoLocation geo(46.842607,-91.99456,198.31);

    EXPECT_STREQ(ddb::calculateFootprint(sensorSize, geo, focal, cameraOri, relAltitude).c_str(), "");
}

}
