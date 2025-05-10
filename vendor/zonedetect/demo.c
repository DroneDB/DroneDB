
/*
 * Copyright (c) 2018, Bertold Van den Bergh (vandenbergh@bertold.org)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the author nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR DISTRIBUTOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include "zonedetect.h"

void printResults(ZoneDetect *cd, ZoneDetectResult *results, float safezone)
{
    if(!results) {
        printf("No results\n");
        return;
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
    ZDFreeResults(results);

    if(index) {
        printf("Safezone: %f\n", safezone);
    }
}

void onError(int errZD, int errNative)
{
    fprintf(stderr, "ZD error: %s (0x%08X)\n", ZDGetErrorString(errZD), (unsigned)errNative);
}

int main(int argc, char *argv[])
{
    if(argc != 4) {
        printf("Usage: %s dbname lat lon\n", argv[0]);
        return 1;
    }

    ZDSetErrorHandler(onError);

    ZoneDetect *const cd = ZDOpenDatabase(argv[1]);
    if(!cd) return 2;

    const float lat = (float)atof(argv[2]);
    const float lon = (float)atof(argv[3]);

    float safezone = 0;
    ZoneDetectResult *results = ZDLookup(cd, lat, lon, &safezone);
    printResults(cd, results, safezone);

    printf("The simple string is [%s]\n", ZDHelperSimpleLookupString(cd, lat, lon));

    ZDCloseDatabase(cd);

    return 0;
}
