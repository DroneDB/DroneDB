/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CMD_OPTIONS_H
#define CMD_OPTIONS_H

#include <initializer_list>
#include <string>
#include <cxxopts.hpp>
#include "output_format.h"

namespace cmd
{

    // The options several commands share: one declaration of each, so the name, the
    // short letter, the text and the default are the same in every command's help. They add the
    // option where they are called, so the order of the help is the order of the calls.

    // -w, --working-dir: the folder of the index (default ".")
    void addWorkingDirOption(cxxopts::Options &opts);

    // -o, --output with the text of the command. Without a default value the option has none.
    void addOutputOption(cxxopts::Options &opts, const std::string &description);
    void addOutputOption(cxxopts::Options &opts, const std::string &description, const std::string &defaultValue);

    // -f, --format "Output format (<formats>)", default "text". Command::output() writes it.
    void addFormatOption(cxxopts::Options &opts, const std::string &formats = "text|json");

    // The format the user gave with -f, parsed once at the boundary of the command: an
    // InvalidArgsException "Invalid format <name>" for a name the command does not accept
    ddb::OutputFormat getFormatOption(const cxxopts::ParseResult &opts,
                                      std::initializer_list<ddb::OutputFormat> allowed = {
                                          ddb::OutputFormat::Text, ddb::OutputFormat::Json});

    // -k, --insecure: do not verify the TLS certificate of a registry
    void addInsecureOption(cxxopts::Options &opts);

    // -r, --remote: the registry to push to or pull from (default "": the dataset's tag)
    void addRemoteOption(cxxopts::Options &opts);

}

#endif // CMD_OPTIONS_H
