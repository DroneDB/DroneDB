#!/bin/bash

sudo apt update && sudo apt install -y --fix-missing --no-install-recommends build-essential ca-certificates cmake git sqlite3 spatialite-bin exiv2 libexiv2-dev libgeos-dev libgdal-dev g++-8 gcc-8
sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-8 800 --slave /usr/bin/g++ g++ /usr/bin/g++-8
