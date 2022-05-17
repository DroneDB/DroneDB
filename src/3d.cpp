/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "3d.h"
#include "logger.h"
#include "mio.h"
#include "exceptions.h"
#include "utils.h"

namespace ddb{

std::string buildNexus(const std::string &inputObj, const std::string &outputNxs, bool overwrite){
    std::string outFile;
    fs::path p(inputObj);

    if (outputNxs.empty()){
        
        outFile = p.replace_filename(p.filename().replace_extension(".nxz")).string();
    }else{
        outFile = outputNxs;
    }

    if (fs::exists(outFile)){
        if (overwrite) io::assureIsRemoved(outFile);
        else throw AppException("File " + outFile + " already exists (delete it first)");
    }

    // Check that this file's dependencies are present
    auto deps = getObjDependencies(inputObj);
    for (const std::string &d : deps){
        fs::path relPath = p.parent_path() / d;
        if (!fs::exists(relPath)) {
            throw BuildDepMissingException(d + " is referenced by " + inputObj + " but it's missing");
        }
    }

    NXSErr err = nexusBuild(inputObj.c_str(), outFile.c_str());
    if (err == NXSERR_EXCEPTION){
        throw AppException("Could not build nexus file for " + inputObj);
    }

    return outFile;

}

std::vector<std::string> getObjDependencies(const std::string &obj){
    std::vector<std::string> deps;
    if (!fs::exists(obj)) throw FSException(obj + " does not exist");

    // Parse OBJ
    std::ifstream fin(obj);
    fs::path p(obj);

    std::string line;
    while(std::getline(fin, line)){
        size_t mtllibPos = line.find("mtllib");
        if (mtllibPos == 0){
            std::string mtlFilesLine = line.substr(std::string("mtllib").length(), std::string::npos);
            utils::trim(mtlFilesLine);

            auto mtlFiles = utils::split(mtlFilesLine, " ");
            for (auto &mtlFile : mtlFiles){
                utils::trim(mtlFile);
                deps.push_back(mtlFile);
                fs::path mtlRelPath = p.parent_path() / mtlFile;

                if (fs::exists(mtlRelPath)) {
                    // Parse MTL
                    std::string mtlLine;
                    std::ifstream mtlFin(mtlRelPath.string());
                    while(std::getline(mtlFin, mtlLine)){
                        if (mtlLine.find("map_") == 0){
                            auto tokens = utils::split(mtlLine, " ");
                            if (tokens.size() > 0){
                                auto mapFname = tokens[tokens.size() - 1];
                                if (mapFname.rfind(".") != std::string::npos){
                                    utils::trim(mapFname);
                                    deps.push_back(mapFname);
                                }
                            }
                        }
                    }
                }
            }
        }

        if (line.rfind("v") == 0 || line.rfind("vn") == 0 || line.rfind("f") == 0){
            break;
        }
    }

    return deps;
}

}
