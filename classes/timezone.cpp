/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "cctz/time_zone.h"
#include "timezone.h"
#include "../logger.h"
#include "../utils.h"
#include "exceptions.h"

using namespace ddb;

bool Timezone::initialized = false;
ZoneDetect *Timezone::db = nullptr;

[[ noreturn ]] void onError(int errZD, int errNative) {
    throw TimezoneException("Timezone error: " + std::string(ZDGetErrorString(errZD)) + " (" + std::to_string(errNative) + ")");
}

void Timezone::init() {
    if (initialized) return;

    ZDSetErrorHandler(onError);
    fs::path dbPath = utils::getDataPath("timezone21.bin");
    if (dbPath.empty()) throw TimezoneException("Cannot find timezone database timezone21.bin");
    db = ZDOpenDatabase(dbPath.string().c_str());
    if (!db) throw TimezoneException("Cannot open timezone database ./timezone21.bin");

    initialized = true;
}

long long int Timezone::getUTCEpoch(int year, int month, int day, int hour, int minute, int second, double latitude, double longitude) {
    Timezone::init();
    if (!db) return 0;

    float safezone = 0;
    ZoneDetectResult *results = ZDLookup(db, static_cast<float>(latitude), static_cast<float>(longitude), &safezone);
    if (!results) {
        ZDFreeResults(results);
        return 0;
    }

    unsigned int index = 0;
    cctz::time_zone tz = cctz::utc_time_zone();
    bool found = false;

    while(results[index].lookupResult != ZD_LOOKUP_END) {
        if(results[index].data) {
            std::string timezoneId = std::string(results[index].data[0]) + std::string(results[index].data[1]);

            if (!cctz::load_time_zone(timezoneId, &tz)) {
                LOGD << "Cannot load timezone, defaulting to UTC: " << timezoneId;
            } else {
                found = true;
                break;
            }
        }

        index++;
    }

    if (!found) {
        LOGD << "Cannot find timezone for " << latitude << "," << longitude << ", defaulting to UTC";
    }

    auto time = tz.lookup(cctz::civil_second(year, month, day, hour, minute, second));
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               time.post.time_since_epoch()
           ).count();
}
