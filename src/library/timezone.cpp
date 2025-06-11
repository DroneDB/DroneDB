/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "timezone.h"

#include <mutex>

#include "exceptions.h"
#include "logger.h"
#include "mio.h"

using namespace ddb;

bool Timezone::initialized = false;
ZoneDetect* Timezone::db = nullptr;
std::mutex timezoneMutex;

[[noreturn]] void onError(int errZD, int errNative) {
    throw TimezoneException("Timezone error: " + std::string(ZDGetErrorString(errZD)) + " (" +
                            std::to_string(errNative) + ")");
}

void Timezone::init() {
    if (initialized)
        return;
    std::lock_guard<std::mutex> guard(timezoneMutex);

    ZDSetErrorHandler(onError);
    fs::path dbPath = io::getDataPath("timezone21.bin");
    if (dbPath.empty())
        throw TimezoneException("Cannot find timezone database timezone21.bin");
    db = ZDOpenDatabase(dbPath.string().c_str());
    if (!db)
        throw TimezoneException("Cannot open timezone database ./timezone21.bin");

#ifdef _WIN32
    // On Windows, set TZDIR environment variable to help cctz find timezone data
    fs::path zoneInfoPath = io::getDataPath("zoneinfo");
    if (!zoneInfoPath.empty()) {
        std::string tzdir = "TZDIR=" + zoneInfoPath.string();
        LOGD << "Setting TZDIR to: " << zoneInfoPath.string();
        _putenv(tzdir.c_str());
    }
#endif

    initialized = true;
}

cctz::time_zone Timezone::lookupTimezone(double latitude, double longitude) {
    Timezone::init();
    if (!db)
        return cctz::utc_time_zone();

    float safezone = 0;
    ZoneDetectResult* results =
        ZDLookup(db, static_cast<float>(latitude), static_cast<float>(longitude), &safezone);
    if (!results) {
        ZDFreeResults(results);
        return cctz::utc_time_zone();
    }

    unsigned int index = 0;
    cctz::time_zone tz = cctz::utc_time_zone();
    bool found = false;
    while (results[index].lookupResult != ZD_LOOKUP_END) {
        if (results[index].data) {
            std::string timezoneId =
                std::string(results[index].data[0]) + std::string(results[index].data[1]);
            LOGD << "Attempting to load timezone: " << timezoneId;

            if (!cctz::load_time_zone(timezoneId, &tz)) {
                LOGD << "Cannot load timezone " << timezoneId << ", defaulting to: " << tz.name();

                // Try some common alternatives for Windows
                std::vector<std::string> alternatives = {
                    "UTC",
                    "CET",            // Central European Time
                    "GMT+1",          // GMT offset
                    "GMT+01:00"       // ISO format
                };

                for (const auto& alt : alternatives) {
                    LOGD << "Trying alternative timezone: " << alt;
                    if (cctz::load_time_zone(alt, &tz)) {
                        LOGD << "Successfully loaded alternative timezone: " << alt;
                        found = true;
                        break;
                    }
                }

                if (found)
                    break;
            } else {
                LOGD << "Successfully loaded timezone: " << timezoneId;
                found = true;
                break;
            }
        }

        index++;
    }

    if (!found) {
        LOGD << "Cannot find timezone for " << latitude << "," << longitude
             << ", defaulting to UTC";
    }

    return tz;
}

double Timezone::getUTCEpoch(int year,
                             int month,
                             int day,
                             int hour,
                             int minute,
                             int second,
                             double msecs,
                             const cctz::time_zone& tz) {
    auto time = tz.lookup(cctz::civil_second(year, month, day, hour, minute, second));
    return static_cast<double>(
               std::chrono::duration_cast<std::chrono::milliseconds>(time.post.time_since_epoch())
                   .count()) +
           msecs;
}
