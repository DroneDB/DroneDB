#!/bin/bash

sudo apt update && sudo apt install -y --fix-missing --no-install-recommends build-essential
sudo add-apt-repository ppa:ubuntugis/ubuntugis-unstable

# Check if node is installed
hash node 2>/dev/null || not_found=true 
if [[ $not_found ]]; then
    curl -sL https://deb.nodesource.com/setup_14.x | sudo -E bash -
    sudo apt install -y nodejs
fi

# Check if cmake-js is installed
hash cmake-js 2>/dev/null || not_found=true 
if [[ $not_found ]]; then
    sudo npm install -g cmake-js
fi

sudo apt install -y --fix-missing --no-install-recommends ca-certificates cmake git sqlite3 spatialite-bin exiv2 libexiv2-dev libgeos-dev libgdal-dev g++-8 gcc-8
sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-8 800 --slave /usr/bin/g++ g++ /usr/bin/g++-8
