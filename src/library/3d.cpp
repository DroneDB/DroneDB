/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "3d.h"
#include "logger.h"
#include "mio.h"
#include "exceptions.h"
#include "utils.h"
#include <regex>

namespace ddb
{

    std::string buildNexus(const std::string &inputObj, const std::string &outputNxs, bool overwrite)
    {

        fs::path p(inputObj);

        const auto outFile = outputNxs.empty() ? p.replace_filename(p.filename().replace_extension(".nxz")).string() : outputNxs;

        if (fs::exists(outFile))
        {
            if (overwrite)
                io::assureIsRemoved(outFile);
            else
                throw AppException("File " + outFile + " already exists (delete it first)");
        } 
        
        // Check that this file's dependencies are present
        auto deps = getObjDependencies(inputObj);
        std::vector<std::string> missingDeps;

        // Collect all missing dependencies
        for (const std::string &d : deps)
        {
            fs::path relPath = p.parent_path() / d;

            if (!fs::exists(relPath))
                missingDeps.push_back(d);
        }

        // If there are missing dependencies, throw exception with the complete list
        if (!missingDeps.empty())
        {
            std::string errorMessage = "Dependencies missing for " + inputObj + ": ";
            for (size_t i = 0; i < missingDeps.size(); i++)
            {
                if (i > 0)
                    errorMessage += ", ";
                errorMessage += missingDeps[i];
            }
            throw BuildDepMissingException(errorMessage, missingDeps);
        }

#ifndef NO_NEXUS
        NXSErr err = nexusBuild(inputObj.c_str(), outFile.c_str());
        if (err == NXSERR_EXCEPTION)
        {
            throw AppException("Could not build nexus file for " + inputObj);
        }
#else
        throw AppException("This version of ddb does not have the ability to generate Nexus files");
#endif

        return outFile;
    }

    std::optional<std::string> extractFileName(const std::string &input)
    {
        // Define the regex pattern for extracting file names without restricting file extension
        std::regex pattern("\"([^\"]+\\.[^\\s\"]+)\"|\\b([^\" \\t]+\\.[^\\s\"]+)\\b");

        std::smatch match;
        if (!std::regex_search(input, match, pattern))
            return std::nullopt;

        // Check which capturing group has been matched
        if (match[1].matched)
            return match[1].str(); // File name from double quotes

        if (match[2].matched)
            return match[2].str(); // Filenames not in quotes

        return std::nullopt;
    }

    std::vector<std::string> getObjDependencies(const std::string &obj)
    {
        std::vector<std::string> deps;
        if (!fs::exists(obj))
            throw FSException(obj + " does not exist");

        std::ifstream fin(obj);
        fs::path p(obj);
        fs::path parentPath = p.parent_path();

        const auto keys = {"map_Ka", "map_Kd", "map_Ks", "map_Ns", "map_d", "disp", "decal", "bump", "map_bump", "refl", "map_Pr", "map_Pm", "map_Ps", "map_Ke"};

        std::string line;
        while (std::getline(fin, line))
        {
            size_t mtllibPos = line.find("mtllib");

            // Parse the mtllib line, otherwise skip
            if (mtllibPos == std::string::npos)
                continue;

            // 6 = length of "mtllib"
            std::string mtlFile = line.substr(6 + mtllibPos, std::string::npos);
            utils::trim(mtlFile);

            // Remove double quotes if present
            if (mtlFile[0] == '"' && mtlFile[mtlFile.size() - 1] == '"')
                mtlFile = mtlFile.substr(1, mtlFile.size() - 2);

            deps.push_back(mtlFile);
            fs::path mtlRelPath = parentPath / mtlFile;

            if (!fs::exists(mtlRelPath))
                continue;

            // Parse MTL
            std::string mtlLine;
            std::ifstream mtlFin(mtlRelPath.string());
            while (std::getline(mtlFin, mtlLine))
            {

                for (const auto &key : keys)
                {
                    size_t keyPos = mtlLine.find(key);
                    if (keyPos == std::string::npos)
                        continue;

                    auto lineToParse = mtlLine.substr(keyPos + strlen(key), std::string::npos);
                    auto textureFilename = extractFileName(lineToParse);

                    if (textureFilename.has_value())
                        deps.push_back(textureFilename.value());
                }
            }
        }

        return deps;
    }

}
