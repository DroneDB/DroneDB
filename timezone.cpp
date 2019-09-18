#include "timezone.h"
#include "logger.h"
#include "exceptions.h"

bool Timezone::initialized = false;
ZoneDetect *Timezone::db = nullptr;

[[ noreturn ]] void onError(int errZD, int errNative) {
    throw TimezoneException("Timezone error: " + std::string(ZDGetErrorString(errZD)) + " (" + std::to_string(errNative) + ")");
}

void Timezone::init() {
    if (initialized) return;

    ZDSetErrorHandler(onError);
    db = ZDOpenDatabase("./timezone21.bin");
    if (!db) {
        LOGE << "Cannot open timezone database ./timezone21.bin";
    }

    initialized = true;
}

time_t Timezone::lookupUTCOffset(double latitude, double longitude) {
    Timezone::init();
    if (!db) return 0;

    float safezone = 0;
    ZoneDetectResult *results = ZDLookup(db, static_cast<float>(latitude), static_cast<float>(longitude), &safezone);
    if (!results) {
        ZDFreeResults(results);
        return 0;
    }

    unsigned int index = 0;
    while(results[index].lookupResult != ZD_LOOKUP_END) {
        printf("%s:\n", ZDLookupResultToString(results[index].lookupResult));
        printf("  meta: %u\n", results[index].metaId);
        printf("  polygon: %u\n", results[index].polygonId);
        if(results[index].data) {
            for(unsigned int i = 0; i < results[index].numFields; i++) {
                if(results[index].fieldNames[i] && results[index].data[i]) {
                    printf("  %s: %s\n", results[index].fieldNames[i], results[index].data[i]);
                }
            }
        }

        index++;
    }

    return 0;
}
