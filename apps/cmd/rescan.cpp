/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <iostream>
#include "include/rescan.h"
#include "dbops.h"
#include "entry_types.h"
#include "entry_filters.h"
#include "stringutil.h"

namespace cmd
{

    namespace
    {
        // The names --type takes: those of the entry handlers, which parseEntryTypeList reads
        // them with, so the help lists every type the option accepts
        std::string validTypes()
        {
            return ddb::str::join(ddb::getEntryTypeNames(), ", ");
        }
    }

    void Rescan::setOptions(cxxopts::Options &opts)
    {
        // clang-format off
        opts
            .positional_help("[args]")
            .custom_help("rescan");
        cmd::addWorkingDirOption(opts);
        opts.add_options()
            ("t,type", "Entry types to rescan (comma-separated). Valid types: " + validTypes(), cxxopts::value<std::string>()->default_value(""))
            ("e,ext", "File extensions to rescan (comma-separated, e.g. dwg,dxf). Combined with --type, an entry must match both", cxxopts::value<std::string>()->default_value(""))
            ("continue-on-error", "Continue processing if an error occurs", cxxopts::value<bool>());
        // clang-format on
    }

    std::string Rescan::description()
    {
        return "Re-process all indexed files to update metadata.";
    }

    std::string Rescan::extendedDescription()
    {
        return "\n\nUseful when upgrading DroneDB to a version that extracts more metadata "
               "or supports new file types.\n\n"
               "Valid type filters: " +
               validTypes();
    }

    int Rescan::run(cxxopts::ParseResult &opts)
    {
        const auto ddbPath = opts["working-dir"].as<std::string>();
        const auto typesStr = opts["type"].as<std::string>();
        const auto extensionsStr = opts["ext"].as<std::string>();
        const bool stopOnError = opts.count("continue-on-error") == 0;

        // Parse filters (an invalid one is a usage error: InvalidArgsException)
        const std::vector<ddb::EntryType> types = ddb::filters::parseEntryTypeList(typesStr.c_str());
        const std::vector<std::string> extensions = ddb::filters::parseExtensionList(extensionsStr.c_str());

        const auto db = ddb::open(ddbPath, true);

        int processed = 0;
        int errors = 0;

        ddb::rescanIndex(db.get(), types, extensions, stopOnError,
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
        return EXIT_SUCCESS;
    }

}
