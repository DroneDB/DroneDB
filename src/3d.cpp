/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "3d.h"
#include "logger.h"
#include "mio.h"
#include "exceptions.h"

namespace ddb{

std::string buildObj(const std::string &inputObj, const std::string &outputNxs, bool overwrite){
    std::string outFile;

    if (outputNxs.empty()){
        fs::path p(inputObj);
        outFile = p.replace_filename(p.filename().replace_extension(".nxz")).string();
    }else{
        outFile = outputNxs;
    }

    if (fs::exists(outFile)){
        if (overwrite) io::assureIsRemoved(outFile);
        else throw AppException("File " + outFile + " already exists (delete it first)");
    }

    NXSErr err = nexusBuild(inputObj.c_str(), outFile.c_str());
    if (err == NXSERR_EXCEPTION){
        throw AppException("Could not build nexus file for " + inputObj);
    }

    // TODO: identify when files are missing

    return outFile;

}

}
