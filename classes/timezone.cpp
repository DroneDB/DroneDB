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
#include "cctz/time_zone.h"
#include "timezone.h"
#include "../logger.h"
#include "../utils.h"
#include "exceptions.h"

bool Timezone::initialized = false;
ZoneDetect *Timezone::db = nullptr;

[[ noreturn ]] void onError(int errZD, int errNative) {
    throw TimezoneException("Timezone error: " + std::string(ZDGetErrorString(errZD)) + " (" + std::to_string(errNative) + ")");
}

void Timezone::init() {
    if (initialized) return;

    ZDSetErrorHandler(onError);
    std::cerr << "HERE! " << utils::getExeFolderPath() / "timezone21.bin" << "\n";
    db = ZDOpenDatabase((utils::getExeFolderPath() / "timezone21.bin").c_str());
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
                LOGE << "Cannot load timezone, defaulting to UTC: " << timezoneId;
            } else {
                found = true;
                break;
            }
        }

        index++;
    }

    if (!found) {
        LOGW << "Cannot find timezone for " << latitude << "," << longitude << ", defaulting to UTC";
    }

    auto time = tz.lookup(cctz::civil_second(year, month, day, hour, minute, second));
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               time.post.time_since_epoch()
           ).count();
}
