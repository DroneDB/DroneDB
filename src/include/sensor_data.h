/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef SENSORDATA_H
#define SENSORDATA_H

#include <map>
#include "sqlite_database.h"

namespace ddb{

class SensorData{
    static SqliteDatabase *db;
    static std::map<std::string, double> cacheHits;
    static std::map<std::string, bool> cacheMiss;

public:
    static void checkDbInit();
    static bool contains(const std::string &sensor);
    static double getFocal(const std::string &sensor);
    static void clearCache();
};

}

#endif
