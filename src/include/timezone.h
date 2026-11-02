/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef TIMEZONE_H
#define TIMEZONE_H

#include <cctz/time_zone.h>

// vendor/zonedetect/zonedetect.h: an opaque handle is all this header needs
struct ZoneDetectOpaque;
typedef struct ZoneDetectOpaque ZoneDetect;

class Timezone
{
    static bool initialized;
    static ZoneDetect *db;

public:
    static void init();
    static cctz::time_zone lookupTimezone(double latitude, double longitude);
    // The epoch (milliseconds) of the local civil time in tz, plus msecs
    static double getUTCEpoch(const cctz::civil_second &local, double msecs, const cctz::time_zone &tz);
};

#endif // TIMEZONE_H
