/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef TIMEZONE_H
#define TIMEZONE_H

#include <time.h>
#include "../vendor/zonedetect/zonedetect.h"

class Timezone{
public:
    static bool initialized;
    static ZoneDetect *db;

    static void init();
    static long long int getUTCEpoch(int year, int month, int day, int hour, int minute, int second, double latitude, double longitude);
};

#endif // TIMEZONE_H
