/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "include/clone.h"

#include <constants.h>
#include <ddb.h>
#include <mio.h>
#include <registry_ops.h>
#include <registryutils.h>

#include <fstream>
#include <iostream>

#include "dbops.h"
#include "exceptions.h"
#include "include/credentials.h"

namespace cmd
{
    void Clone::setOptions(cxxopts::Options &opts)
    {
        // clang-format off
        opts
            .positional_help("[args]")
            .custom_help("clone (tag|url) folder")
            .add_options()
            ("t,target", "Repository tag or full url", cxxopts::value<std::string>())
            ("f,folder", "Target folder", cxxopts::value<std::string>()->default_value(""));
        cmd::addInsecureOption(opts);

        // clang-format on
        opts.parse_positional({"target", "folder"});
    }

    std::string Clone::description()
    {
        return "Clone a repository into a new directory";
    }

    std::string Clone::extendedDescription()
    {
        return "\r\n\r\nClones a repository into a newly created directory.";
    }

    int Clone::run(cxxopts::ParseResult &opts)
    {
        if (opts["target"].count() != 1)
        {
            usageError();
        }

        const auto tag =
            ddb::RegistryUtils::parseTag(opts["target"].as<std::string>());

        const auto folderRaw = opts["folder"].as<std::string>();

        const auto folder = fs::absolute(ddb::io::fromUtf8(folderRaw.length() > 0 ? folderRaw : tag.dataset));

        auto sslVerify = opts["insecure"].count() == 0;

        LOGD << "Normalized folder = " << ddb::io::toGenericUtf8(folder);

        ddb::clone(tag, ddb::io::toGenericUtf8(folder), sslVerify, credentials(), std::cout, registryDeps());

        return EXIT_SUCCESS;
    }

} // namespace cmd
