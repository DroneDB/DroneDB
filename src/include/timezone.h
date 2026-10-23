/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef TIMEZONE_H
#define TIMEZONE_H

#include <cctz/time_zone.h>
#include "ddb_export.h"

// vendor/zonedetect/zonedetect.h: an opaque handle is all this header needs
struct ZoneDetectOpaque;
typedef struct ZoneDetectOpaque ZoneDetect;

class Timezone
{
    static bool initialized;
    static ZoneDetect *db;

public:
    DDB_DLL static void init();
    DDB_DLL static cctz::time_zone lookupTimezone(double latitude, double longitude);
    DDB_DLL static double getUTCEpoch(int year, int month, int day, int hour, int minute, int second, double msecs, const cctz::time_zone &tz);
};

#endif // TIMEZONE_H
