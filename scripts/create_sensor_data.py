#!/usr/bin/python3

import json
import sqlite3
import urllib.request
import os

SENSOR_DATA_OSFM='https://raw.githubusercontent.com/mapillary/OpenSfM/master/opensfm/data/sensor_data.json'
SENSOR_DATA_AV='https://raw.githubusercontent.com/alicevision/AliceVision/develop/src/aliceVision/sensorDB/cameraSensors.db'
db_file = os.path.join(os.path.dirname(__file__), '../ddb_data/sensor_data.sqlite')

CREATE_SENSORS_TABLE = """CREATE TABLE IF NOT EXISTS sensors (
    id TEXT PRIMARY KEY NOT NULL,
    focal REAL NOT NULL)"""

if os.path.exists(db_file):
    print("Removing old %s" % db_file)
    os.remove(db_file)

conn = sqlite3.connect(db_file)
c = conn.cursor()
c.execute(CREATE_SENSORS_TABLE)

# OpenSfM DB
sensors = {}
print("Reading %s ..." % SENSOR_DATA_OSFM)
with urllib.request.urlopen(SENSOR_DATA_OSFM) as response:
    d = json.loads(response.read())
    
    for k in d:
        focal = d[k]
        makemodel = k.lower()
        sensors[makemodel] = focal

# AliceVision DB
print("Reading %s ..." % SENSOR_DATA_AV)
with urllib.request.urlopen(SENSOR_DATA_AV) as response:
    text = response.read().decode('utf-8')
    lines = map(lambda l: l.strip().lower().split(";")[:3], text.split('\n'))
    
    for line in lines:
        if len(line) != 3:
            continue
        make, model, focal = line
        focal = float(focal)

        if model.startswith(make):
            makemodel = model
        else:            
            makemodel = "%s %s" % (make, model)

        if not makemodel in sensors:
            sensors[makemodel] = focal

c.executemany("INSERT INTO sensors (id, focal) values (?, ?)", sensors.items())
conn.commit()
conn.close()

print("Wrote %s sensors to %s" % (len(sensors), db_file))
