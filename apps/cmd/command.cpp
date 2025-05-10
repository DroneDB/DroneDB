/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "include/command.h"
#include "logger.h"
#include "exceptions.h"

namespace cmd
{

    Command::Command()
    {
    }

    cxxopts::Options Command::genOptions(const char *programName)
    {
        cxxopts::Options opts(programName, description() + extendedDescription());
        opts
            .show_positional_help();

        setOptions(opts);
        opts.add_options()("h,help", "Print help")("debug", "Show debug output");

        return opts;
    }

    void Command::run(int argc, char *argv[])
    {
        cxxopts::Options opts = genOptions(argv[0]);

        try
        {
            auto result = opts.parse(argc, argv);

            if (result.count("help"))
            {
                printHelp();
            }

            if (result.count("debug"))
            {
                set_logger_verbose();
            }

            run(result);
        }
        catch (const cxxopts::exceptions::no_such_option &)
        {
            printHelp();
        }
        catch (const cxxopts::exceptions::incorrect_argument_type &)
        {
            printHelp();
        }
        catch (const cxxopts::exceptions::option_requires_argument &)
        {
            printHelp();
        }
        catch (const ddb::AppException &e)
        {
            std::cerr << e.what() << std::endl;
            exit(EXIT_FAILURE);
        }
    }

    void Command::printHelp(std::ostream &out, bool exitAfterPrint)
    {
        out << genOptions().help({""});
        if (exitAfterPrint)
            exit(0);
    }

    void Command::output(std::ostream &out, const json &j, const std::string &format)
    {
        if (format == "json")
            out << j << std::endl;
        else if (format == "text")
            printJsonToText(out, j);
        else
            throw ddb::InvalidArgsException("Invalid format " + format);
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
                        k[0] = std::toupper(k[0]);
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
                    k[0] = std::toupper(k[0]);
                std::string v = item.value().dump();
                if (item.value().is_string())
                    v = item.value().get<std::string>();

                out << k << ": " << v << std::endl;
            }
        }
    }

}
