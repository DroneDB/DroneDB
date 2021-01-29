/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <mutex>
#include "sensor_data.h"
#include "logger.h"
#include "exceptions.h"
#include "mio.h"

namespace ddb{

std::mutex dbInitMutex;
SqliteDatabase* SensorData::db = nullptr;
std::map<std::string, double> SensorData::cacheHits;
std::map<std::string, bool> SensorData::cacheMiss;

void SensorData::checkDbInit(){
    std::lock_guard<std::mutex> guard(dbInitMutex);

    if (db == nullptr){
        db = new SqliteDatabase();
        LOGD << "Initializing sensor database";

        fs::path dbPath = io::getDataPath("sensor_data.sqlite");
        if (dbPath.empty()) throw DBException("Cannot find sensor database sensor_data.sqlite");

        db->open(dbPath.string());
    }
}

bool SensorData::contains(const std::string &sensor){
    if (cacheHits.count(sensor) > 0) return true;
    if (cacheMiss.count(sensor) > 0) return false;

    checkDbInit();
    auto q = db->query("SELECT focal FROM sensors WHERE id = ?");
    q->bind(1, sensor);
    if (q->fetch()){
        // Store in cache
        cacheHits[sensor] = q->getDouble(0);
        return true;
    }else{
        cacheMiss[sensor] = true;
        return false;
    }
}

double SensorData::getFocal(const std::string &sensor){
    if (cacheHits.count(sensor) > 0) return cacheHits.at(sensor);
    if (cacheMiss.count(sensor) > 0) throw DBException("Cannot get focal value for " + sensor + ", no entry found");

    checkDbInit();
    auto q = db->query("SELECT focal FROM sensors WHERE id = ?");
    q->bind(1, sensor);
    if (q->fetch()){
        // Store in cache
        cacheHits[sensor] = q->getDouble(0);
        return cacheHits[sensor];
    }else{
        cacheMiss[sensor] = true;
        throw DBException("Cannot get focal value for " + sensor + ", no entry found");
    }
}

void SensorData::clearCache(){
    cacheHits.clear();
    cacheMiss.clear();
}

}
