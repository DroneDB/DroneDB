/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef SENSORDATA_H
#define SENSORDATA_H

#include <map>
#include "sqlite_database.h"
#include "ddb_export.h"

namespace ddb{

class SensorData{
    static SqliteDatabase *db;
    static std::map<std::string, double> cacheHits;
    static std::map<std::string, bool> cacheMiss;

public:
    DDB_DLL static void checkDbInit();
    DDB_DLL static bool contains(const std::string &sensor);
    DDB_DLL static double getFocal(const std::string &sensor);
    DDB_DLL static void clearCache();
};

}

#endif
