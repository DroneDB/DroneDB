#include <string>
#include <iostream>
#include <fstream>
#include <sstream>
#include "include/cmdlist.h"
#include "fs.h"
#include "include/gendocs.h"

namespace cmd
{

    void generateDocs(int argc, char *argv[])
    {
        fs::path outfile = "_cli_autogen.mdx";
        for (int i = 0; i < argc; i++)
        {
            if (std::string(argv[i]) == "--outfile" && i + 1 < argc)
                outfile = fs::path(argv[i + 1]);
        }

        std::cout << "Generating docs in " << outfile.string() << std::endl;
        std::cout << "===============================" << std::endl;

        std::cout << "W\t" << outfile.string() << std::endl;
        std::ofstream f(outfile.string(), std::ios::out | std::ios::trunc);

        for (auto &cmd : commands)
        {
            f << "### " << cmd.first << std::endl
              << std::endl
              << "```" << std::endl;

            std::stringstream ss;
            cmd.second->printHelp(ss, false);
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
