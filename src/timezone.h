/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef TIMEZONE_H
#define TIMEZONE_H

#include "cctz/time_zone.h"
#include "../vendor/zonedetect/zonedetect.h"
#include "ddb_export.h"

class Timezone{
public:
    static bool initialized;
    static ZoneDetect *db;

    DDB_DLL static void init();
    DDB_DLL static cctz::time_zone lookupTimezone(double latitude, double longitude);
    DDB_DLL static double getUTCEpoch(int year, int month, int day, int hour, int minute, int second, double msecs, const cctz::time_zone &tz);
};

#endif // TIMEZONE_H
