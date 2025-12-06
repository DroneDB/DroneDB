/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <iostream>
#include <sstream>
#include "include/rescan.h"
#include "dbops.h"
#include "entry_types.h"

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
        if (!typesStr.empty())
        {
            std::stringstream ss(typesStr);
            std::string item;
            while (std::getline(ss, item, ','))
            {
                // Trim whitespace
                size_t start = item.find_first_not_of(" \t");
                size_t end = item.find_last_not_of(" \t");
                if (start != std::string::npos && end != std::string::npos)
                    item = item.substr(start, end - start + 1);

                auto t = ddb::typeFromHuman(item);
                if (t == ddb::EntryType::Undefined)
                {
                    std::cerr << "Unknown type: " << item << std::endl;
                    printHelp();
                }
                if (t == ddb::EntryType::Directory)
                {
                    std::cerr << "Cannot rescan directories" << std::endl;
                    printHelp();
                }
                types.push_back(t);
            }
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
