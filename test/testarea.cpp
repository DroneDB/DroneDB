/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "testarea.h"
#include "logger.h"
#include "exceptions.h"
#include "mio.h"

TestArea::TestArea(const std::string &name, bool recreateIfExists)
    : name(name){
    const auto root = getFolder();
    if (name.find("..") != std::string::npos) throw FSException("Cannot use .. in name");

    if (recreateIfExists){
        if (fs::exists(root)){
            LOGD << "Removing " << root;
            LOGD << "Removed " << fs::remove_all(root) << " files/folders";
        }
    }
}

fs::path TestArea::getPath(const fs::path &p){
    return fs::temp_directory_path() / "ddb_test_areas" / fs::path(name) / p;
}

fs::path TestArea::getFolder(const fs::path &subfolder){
    const fs::path root = fs::temp_directory_path() / "ddb_test_areas" / fs::path(name);
    auto dir = root;
    if (!subfolder.empty()) dir = dir / subfolder;

    if (!fs::exists(dir)){
        io::createDirectories(dir);
        LOGD << "Created test folder " << dir;
    }
    return dir;
}

fs::path TestArea::downloadTestAsset(const std::string &url, const std::string &filename, bool overwrite){
    fs::path destination = getFolder() / fs::path(filename);

    if (fs::exists(destination)){
        if (!overwrite) return destination;
        else fs::remove(destination);
    }

    net::Request r = net::GET(url);
    r.verifySSL(false);
    r.downloadToFile(destination.string());

    return destination;
}

