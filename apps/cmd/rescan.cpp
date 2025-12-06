/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <iostream>
#include "include/rescan.h"
#include "dbops.h"
#include "entry_types.h"
#include "utils.h"

namespace cmd
{

    void Rescan::setOptions(cxxopts::Options &opts)
    {
        // clang-format off
        opts
            .positional_help("[args]")
            .custom_help("rescan")
            .add_options()
            ("w,working-dir", "Working directory", cxxopts::value<std::string>()->default_value("."))
            ("t,type", "Entry types to rescan (comma-separated). Valid types: generic, geoimage, georaster, pointcloud, image, dronedb, markdown, video, geovideo, model, panorama, geopanorama, vector", cxxopts::value<std::string>()->default_value(""))
            ("continue-on-error", "Continue processing if an error occurs", cxxopts::value<bool>());
        // clang-format on
    }

    std::string Rescan::description()
    {
        return "Re-process all indexed files to update metadata.";
    }

    std::string Rescan::extendedDescription()
    {
        std::stringstream ss;
        ss << "Useful when upgrading DroneDB to a version that extracts more metadata "
           << "or supports new file types.\n\n"
           << "Valid type filters: ";

        auto typeNames = ddb::getEntryTypeNames();
        for (size_t i = 0; i < typeNames.size(); i++)
        {
            if (i > 0)
                ss << ", ";
            ss << typeNames[i];
        }

        return ss.str();
    }

    void Rescan::run(cxxopts::ParseResult &opts)
    {
        const auto ddbPath = opts["working-dir"].as<std::string>();
        const auto typesStr = opts["type"].as<std::string>();
        const bool stopOnError = opts.count("continue-on-error") == 0;

        // Parse types
        std::vector<ddb::EntryType> types;
        try {
            types = ddb::utils::parseEntryTypeList(typesStr.c_str());
        } catch (const ddb::InvalidArgsException& e) {
            std::cerr << e.what() << std::endl;
            printHelp();
            return;
        }

        const auto db = ddb::open(ddbPath, true);

        int processed = 0;
        int errors = 0;

        ddb::rescanIndex(db.get(), types, stopOnError,
                         [&processed, &errors](const ddb::Entry &e, bool success, const std::string &error)
                         {
                             if (success)
                             {
                                 std::cout << "U\t" << e.path << std::endl;
                                 processed++;
                             }
                             else
                             {
                                 std::cerr << "E\t" << e.path << "\t" << error << std::endl;
                                 errors++;
                             }
                             return true;
                         });

        std::cout << "Rescan completed: " << processed << " updated";
        if (errors > 0)
            std::cout << ", " << errors << " errors";
        std::cout << std::endl;
    }

}
