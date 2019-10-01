#!/usr/bin/python3

import json
import urllib.request
import os

SENSOR_DATA='https://raw.githubusercontent.com/mapillary/OpenSfM/master/opensfm/data/sensor_data.json'
header_file = os.path.join(os.path.dirname(__file__), '../sensor_data.h')

print("Reading %s ..." % SENSOR_DATA)
with urllib.request.urlopen(SENSOR_DATA) as response:
    d = json.loads(response.read())
    
    with open(header_file, "w") as f:
        f.write("""
// This file is automatically generated via create_sensor_data.py
// Do not modify. All changes will be lost!
// Data license: https://raw.githubusercontent.com/mapillary/OpenSfM/master/opensfm/data/sensor_data.readme.txt
#ifndef SENSOR_DATA_H
#define SENSOR_DATA_H

#include <map>

static std::map<std::string,double> sensorData = {
""")
        for k in d:
            focal = d[k]
            f.write('{"%s", %s},\n' % (k.lower(), focal))
        f.write("""
};

#endif // SENSOR_DATA_H
""")
        print("Wrote %s sensors to %s" % (len(d), header_file))
