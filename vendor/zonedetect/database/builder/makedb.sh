#!/bin/sh

set -e

g++ builder.cpp --std=c++11 -o builder -lshp

rm -rf out naturalearth timezone db.zip
mkdir -p out
mkdir -p out_v1
mkdir -p naturalearth
mkdir -p timezone

(
echo https://naciscdn.org/naturalearth/10m/cultural/ne_10m_admin_0_countries_lakes.zip -o /dev/null -O naturalearth/ne.zip
echo https://github.com/evansiroky/timezone-boundary-builder/releases/download/2024b/timezones-with-oceans.shapefile.zip -o /dev/null -O timezone/tz.zip
) | xargs -n5 -P2 wget

cd naturalearth
unzip ne.zip
cd ..

(
echo C "naturalearth/ne_10m_admin_0_countries_lakes ./out/country16.bin 16 \"Made with Natural Earth, placed in the Public Domain.\" 0";
echo C "naturalearth/ne_10m_admin_0_countries_lakes ./out/country21.bin 21 \"Made with Natural Earth, placed in the Public Domain.\" 0";
echo C "naturalearth/ne_10m_admin_0_countries_lakes ./out_v1/country16.bin 16 \"Made with Natural Earth, placed in the Public Domain.\" 1";
echo C "naturalearth/ne_10m_admin_0_countries_lakes ./out_v1/country21.bin 21 \"Made with Natural Earth, placed in the Public Domain.\" 1";
) | xargs -n6 -P4 ./builder

cd timezone
unzip tz.zip
cd ..

(
echo "T timezone/combined-shapefile-with-oceans ./out/timezone16.bin 16 \"Contains data from Natural Earth, placed in the Public Domain. Contains information from https://github.com/evansiroky/timezone-boundary-builder, which is made available here under the Open Database License \(ODbL\).\" 0";
echo "T timezone/combined-shapefile-with-oceans ./out/timezone21.bin 21 \"Contains data from Natural Earth, placed in the Public Domain. Contains information from https://github.com/evansiroky/timezone-boundary-builder, which is made available here under the Open Database License \(ODbL\).\" 0";
echo "T timezone/combined-shapefile-with-oceans ./out_v1/timezone16.bin 16 \"Contains data from Natural Earth, placed in the Public Domain. Contains information from https://github.com/evansiroky/timezone-boundary-builder, which is made available here under the Open Database License \(ODbL\).\" 1";
echo "T timezone/combined-shapefile-with-oceans ./out_v1/timezone21.bin 21 \"Contains data from Natural Earth, placed in the Public Domain. Contains information from https://github.com/evansiroky/timezone-boundary-builder, which is made available here under the Open Database License \(ODbL\).\" 1";
) | xargs -n 6 -P4 ./builder

exit

rm -rf timezone naturalearth

zip db.zip out/* out_v1/*
