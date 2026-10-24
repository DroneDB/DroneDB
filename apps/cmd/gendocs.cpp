#include <string>
#include <iostream>
#include <fstream>
#include <sstream>
#include "include/cmdlist.h"
#include "fs.h"
#include "include/gendocs.h"
#include "mio.h"

namespace cmd
{

    void generateDocs(int argc, char *argv[])
    {
        fs::path outfile = "_cli_autogen.mdx";
        for (int i = 0; i < argc; i++)
        {
            if (std::string(argv[i]) == "--outfile" && i + 1 < argc)
                outfile = ddb::io::fromUtf8(argv[i + 1]);
        }

        std::cout << "Generating docs in " << ddb::io::toUtf8(outfile) << std::endl;
        std::cout << "===============================" << std::endl;

        std::cout << "W\t" << ddb::io::toUtf8(outfile) << std::endl;

        // Ensure path exists and create it if not
        if (!fs::exists(outfile.parent_path()))
        {
            std::cout << "Creating directory " << ddb::io::toUtf8(outfile.parent_path()) << std::endl;
            fs::create_directories(outfile.parent_path());
        }

        std::ofstream f(outfile, std::ios::out | std::ios::trunc);

        const auto &registry = CommandRegistry::builtin();
        for (const auto &name : registry.names())
        {
            f << "### " << name << std::endl
              << std::endl
              << "```" << std::endl;

            std::stringstream ss;
            registry.create(name)->printHelp(ss);
            ss.seekg(0, std::ios::beg);

            std::string line;
            while (std::getline(ss, line))
            {
                f << line << std::endl;
            }

            f << std::endl;

            f << "```" << std::endl
              << std::endl;
        }

        f.close();
    }

}
