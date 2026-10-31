/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "include/command.h"

#include <cstdlib>

#include "exceptions.h"
#include "json.h"
#include "logger.h"
#include "stringutil.h"

namespace cmd
{

    cxxopts::Options Command::genOptions(const char *programName)
    {
        cxxopts::Options opts(programName, description() + extendedDescription());
        opts
            .show_positional_help();

        setOptions(opts);
        opts.add_options()("h,help", "Print help")("debug", "Show debug output");

        return opts;
    }

    int Command::run(int argc, char *argv[])
    {
        cxxopts::Options opts = genOptions();
        auto result = opts.parse(argc, argv);

        if (result.count("help"))
        {
            printHelp();
            return EXIT_SUCCESS;
        }

        if (result.count("debug"))
        {
            ddb::logging::setVerbose();
        }

        return run(result);
    }

    void Command::printHelp(std::ostream &out)
    {
        out << genOptions().help({""});
    }

    void Command::usageError(const std::string &message)
    {
        throw UsageError(message);
    }

    void Command::output(std::ostream &out, const json &j, ddb::OutputFormat format)
    {
        ddb::requireOutputFormat(format, {ddb::OutputFormat::Text, ddb::OutputFormat::Json});

        if (format == ddb::OutputFormat::Json)
            out << j << std::endl;
        else
            printJsonToText(out, j);
    }

    void Command::printJsonToText(std::ostream &out, const json &j)
    {
        if (j.is_array())
        {
            size_t count = 0;

            for (const auto &it : j)
            {
                for (const auto &item : it.items())
                {
                    std::string k = item.key();
                    if (k.length() > 0)
                        k[0] = ddb::str::toUpperChar(k[0]);
                    std::string v = item.value().dump();
                    if (item.value().is_string())
                        v = item.value().get<std::string>();

                    out << k << ": " << v << std::endl;
                }
                if (++count < j.size())
                    out << "--------" << std::endl;
            }
        }
        else
        {
            for (const auto &item : j.items())
            {
                std::string k = item.key();
                if (k.length() > 0)
                    k[0] = ddb::str::toUpperChar(k[0]);
                std::string v = item.value().dump();
                if (item.value().is_string())
                    v = item.value().get<std::string>();

                out << k << ": " << v << std::endl;
            }
        }
    }

}
