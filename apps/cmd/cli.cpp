/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "include/cli.h"

#include <cstdlib>
#include <exception>

#include "exceptions.h"

namespace cmd
{

    namespace
    {
        int usageFailure(Command &command, const std::string &message, std::ostream &err)
        {
            if (!message.empty())
                err << "error: " << message << std::endl << std::endl;
            command.printHelp(err);
            return EXIT_FAILURE;
        }
    }

    int execute(Command &command, int argc, char *argv[], std::ostream &err)
    {
        try
        {
            return command.run(argc, argv);
        }
        catch (const UsageError &e)
        {
            return usageFailure(command, e.what(), err);
        }
        catch (const cxxopts::exceptions::exception &e)
        {
            return usageFailure(command, e.what(), err);
        }
        catch (const ddb::InvalidArgsException &e)
        {
            return usageFailure(command, e.what(), err);
        }
        catch (const ddb::AppException &e)
        {
            err << e.what() << std::endl;
            return EXIT_FAILURE;
        }
        catch (const std::exception &e)
        {
            err << "error: " << e.what() << std::endl;
            return EXIT_FAILURE;
        }
    }

}
