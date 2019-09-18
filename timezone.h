#ifndef TIMEZONE_H
#define TIMEZONE_H

#include <time.h>
#include "libs/zonedetect/zonedetect.h"

class Timezone{
public:
    static bool initialized;
    static ZoneDetect *db;

    static void init();
    static time_t lookupUTCOffset(double latitude, double longitude);
};

#endif // TIMEZONE_H
