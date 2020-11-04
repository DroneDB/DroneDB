#include <string>
#include <iostream>
#include <fstream>
#include <sstream>
#include "cmd/cmdlist.h"
#include "fs.h"
#include "gendocs.h"

namespace cmd{

void generateDocs(int argc, char *argv[]){
    fs::path outdir = "./docs";
    for (int i = 0; i < argc; i++){
        if (std::string(argv[i]) == "--outdir" && i + 1 < argc) outdir = fs::path(argv[i + 1]);
    }

    std::cout << "Genrating docs in " << outdir.string() << std::endl;
    std::cout << "===============================" << std::endl;

    for (auto &cmd : commands){
        fs::path outfile = outdir / (cmd.first + ".rst");
        std::cout << "W\t" << outfile.string() << std::endl;
        std::ofstream f(outfile.string(), std::ios::out | std::ios::trunc);

        f << ".. _" << cmd.first << "_command:" << std::endl << std::endl
          << "********************************************************************************" << std::endl
          << cmd.first << std::endl
          << "********************************************************************************" << std::endl
          << std::endl
          << "::" << std::endl
          << std::endl;

        std::stringstream ss;
        cmd.second->printHelp(ss, false);
        ss.seekg(0, std::ios::beg);

        std::string line;
        while(std::getline(ss, line)){
            f << "    " << line << std::endl;
        }

        f << std::endl;

        f << ".. toctree::" << std::endl
          << "    :maxdepth: 2" << std::endl
          << "    :glob:" << std::endl;

        f.close();
    }

    // Summary file
    fs::path outfile = outdir / "commands.txt";
    std::cout << "W\t" << outfile.string() << std::endl;
    std::ofstream f(outfile.string(), std::ios::out | std::ios::trunc);
    f << "::" << std::endl << std::endl;

    for (auto &cmd : commands){
        f << "    " << cmd.first << " - " << cmd.second->description() << std::endl;
    }
    f << std::endl;

    f.close();
}

}
