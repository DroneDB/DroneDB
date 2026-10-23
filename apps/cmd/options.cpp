/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "include/options.h"

namespace cmd
{

    void addWorkingDirOption(cxxopts::Options &opts)
    {
        opts.add_options()("w,working-dir", "Working directory", cxxopts::value<std::string>()->default_value("."));
    }

    void addOutputOption(cxxopts::Options &opts, const std::string &description)
    {
        opts.add_options()("o,output", description, cxxopts::value<std::string>());
    }

    void addOutputOption(cxxopts::Options &opts, const std::string &description, const std::string &defaultValue)
    {
        opts.add_options()("o,output", description, cxxopts::value<std::string>()->default_value(defaultValue));
    }

    void addFormatOption(cxxopts::Options &opts, const std::string &formats)
    {
        opts.add_options()("f,format", "Output format (" + formats + ")",
                           cxxopts::value<std::string>()->default_value("text"));
    }

    void addInsecureOption(cxxopts::Options &opts)
    {
        opts.add_options()("k,insecure", "Disable SSL certificate verification", cxxopts::value<bool>());
    }

    void addRemoteOption(cxxopts::Options &opts)
    {
        opts.add_options()("r,remote", "The remote Registry", cxxopts::value<std::string>()->default_value(""));
    }

}
