/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "stac.h"
#include "../stac.h"
#include <iostream>

namespace cmd {

void Stac::setOptions(cxxopts::Options &opts) {
    // clang-format off
    opts
    .positional_help("[args]")
    .custom_help("stac")
    .add_options()
    ("w,working-dir", "Working directory", cxxopts::value<std::string>()->default_value("."))
    ("p,path", "Entry path to generate a STAC item for (which must be part of the DroneDB index)", cxxopts::value<std::string>()->default_value(""))
    ("stac-endpoint", "STAC Endpoint URL for STAC links", cxxopts::value<std::string>()->default_value("/stac"))
    ("download-endpoint", "STAC Download Endpoint URL for STAC assets", cxxopts::value<std::string>()->default_value("/download"))
    ("stac-catalog-root", "STAC Catalog absolute URL", cxxopts::value<std::string>()->default_value(""))
    ("stac-collection-root", "STAC Collection absolute URL", cxxopts::value<std::string>()->default_value("."))
    ("id", "Set STAC id explicitely instead of using the directory name", cxxopts::value<std::string>()->default_value(""));
    // clang-format on
}

std::string Stac::description() {
    return "Generate STAC catalogs";
}

void Stac::run(cxxopts::ParseResult &opts) {
    const auto ddbPath = opts["working-dir"].as<std::string>();
    const auto entry = opts["path"].as<std::string>();
    const auto stacEndpoint = opts["stac-endpoint"].as<std::string>();
    const auto downloadEndpoint = opts["download-endpoint"].as<std::string>();
    const auto stacCollectionRoot = opts["stac-collection-root"].as<std::string>();
    const auto stacCatalogRoot = opts["stac-catalog-root"].as<std::string>();

    const auto id = opts["id"].as<std::string>();

    std::cout << ddb::generateStac(ddbPath, entry, stacCollectionRoot, stacEndpoint, downloadEndpoint, id, stacCatalogRoot).dump(4) << std::endl;
}

}  // namespace cmd
