/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "share.h"
#include "constants.h"
#include "exceptions.h"
#include "shareservice.h"
#include "registryutils.h"
#include "utils.h"
#include "mio.h"
#include "progressbar.h"

namespace cmd {

void Share::setOptions(cxxopts::Options &opts) {
    opts
    .positional_help("[args]")
    .custom_help("share *.JPG")
    .add_options()

    ("i,input", "Files and directories to share", cxxopts::value<std::vector<std::string>>())
    ("r,recursive", "Recursively share subdirectories", cxxopts::value<bool>())
    ("t,tag", "Tag to use (organization/dataset or server[:port]/organization/dataset)", cxxopts::value<std::string>()->default_value(DEFAULT_REGISTRY "/public/<randomID>"))
    ("p,password", "Optional password to protect dataset", cxxopts::value<std::string>()->default_value(""))
    ("q,quiet", "Do not display progress", cxxopts::value<bool>());

    opts.parse_positional({"input"});
}

std::string Share::description() {
    return "Share files and folders to a registry";
}

void Share::run(cxxopts::ParseResult &opts) {
    if (!opts.count("input")) {
        printHelp();
    }

    auto input = opts["input"].as<std::vector<std::string>>();
    auto tag = opts["tag"].as<std::string>();
    auto password = opts["password"].as<std::string>();
    auto recursive = opts["recursive"].count() > 0;
    auto quiet = opts["quiet"].count() > 0;
    auto cwd = ddb::io::getCwd().string();

    ProgressBar pb;
    ddb::ShareCallback showProgress = [&pb](const std::string &file, float progress){
        pb.update(file, progress);
        return true;
    };
    if (quiet) showProgress = nullptr;

    ddb::ShareService ss;

    auto share = [&](){
        std::string url = ss.share(input, tag, password, recursive, cwd, showProgress);
        if (!quiet) pb.done();
        std::cout << url << std::endl;
    };

    try{
        share();
    }catch(const ddb::AuthException &){
        // Try logging-in
        auto username = ddb::utils::getPrompt("Username: ");
        auto password = ddb::utils::getPass("Password: ");

        ddb::Registry reg = ddb::RegistryUtils::createFromTag(tag);
        if (reg.login(username, password).length() > 0){
            // Retry
            share();
        }else{
            throw ddb::AuthException("Cannot authenticate with " + reg.getUrl());
        }
    }catch(const ddb::AppException &e){
        if (!quiet) pb.done();
        throw e;
    }
}

}


