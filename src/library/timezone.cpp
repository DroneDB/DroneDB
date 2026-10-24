/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "timezone.h"
#include "../../vendor/zonedetect/library/zonedetect.h"

#include <cstdlib>
#include <memory>
#include <vector>
#include <mutex>

#include "exceptions.h"
#include "logger.h"
#include "mio.h"

using namespace ddb;

bool Timezone::initialized = false;
ZoneDetect* Timezone::db = nullptr;

namespace {
// init() runs its body once per process. A throwing body leaves the flag unset, so the
// next caller retries, as before; callers never read db while another thread writes it.
std::once_flag timezoneInitFlag;
}

[[noreturn]] void onError(int errZD, int errNative) {
    throw TimezoneException("Timezone error: " + std::string(ZDGetErrorString(errZD)) + " (" +
                            std::to_string(errNative) + ")");
}

void Timezone::init() {
    std::call_once(timezoneInitFlag, [] {
        ZDSetErrorHandler(onError);
        fs::path dbPath = io::getDataPath("timezone21.bin");
        if (dbPath.empty())
            throw TimezoneException("Cannot find timezone database timezone21.bin");
#ifdef _WIN32
        // ZoneDetect opens a file name with CreateFileA (ANSI code page): the database is read
        // here and kept for the process (db is never closed)
        static std::vector<uint8_t> database;
        try {
            database = io::readAllBytes(dbPath);
        } catch (const FSException &e) {
            throw TimezoneException(std::string("Cannot open timezone database: ") + e.what());
        }
        db = ZDOpenDatabaseFromMemory(database.data(), database.size());
#else
        db = ZDOpenDatabase(dbPath.c_str());
#endif
        if (!db)
            throw TimezoneException("Cannot open timezone database ./timezone21.bin");

#ifdef _WIN32
        // On Windows, set TZDIR environment variable to help cctz find timezone data. cctz
        // opens it with fopen, which follows the CRT locale (UTF-8 after DDBRegisterProcess).
        fs::path zoneInfoPath = io::getDataPath("zoneinfo");
        if (!zoneInfoPath.empty()) {
            LOGD << "Setting TZDIR to: " << io::toUtf8(zoneInfoPath);
            _putenv_s("TZDIR", io::toUtf8(zoneInfoPath).c_str());
        }
#endif

        initialized = true;
    });
}

cctz::time_zone Timezone::lookupTimezone(double latitude, double longitude) {
    Timezone::init();
    if (!db)
        return cctz::utc_time_zone();

    float safezone = 0;
    ZoneDetectResult* results =
        ZDLookup(db, static_cast<float>(latitude), static_cast<float>(longitude), &safezone);
    if (!results)
        return cctz::utc_time_zone();
    // ZDLookup allocates the results; they were never freed on the normal path
    const std::unique_ptr<ZoneDetectResult, void (*)(ZoneDetectResult*)> resultsOwner(results, ZDFreeResults);

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

double Timezone::getUTCEpoch(const cctz::civil_second& local, double msecs, const cctz::time_zone& tz) {
    auto time = tz.lookup(local);
    return static_cast<double>(
               std::chrono::duration_cast<std::chrono::milliseconds>(time.post.time_since_epoch())
                   .count()) +
           msecs;
}
