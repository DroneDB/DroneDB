/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef TESTAREA_H
#define TESTAREA_H

#include <string>
#include "fs.h"

/**
 * @brief A named scratch folder of a test.
 *
 * Every process has a folder of its own (processRoot()), and the areas live in it: tests of
 * different processes (ctest -j runs each test in one, several ctest runs may share a temp
 * folder) never see or remove each other's files, even with the same area name. Tests of one
 * process share it, so two TestArea of the same name in a process are the same folder. The
 * folder is removed when the process ends, unless keepProcessRoot() was called (a failed run).
 */
class TestArea
{
    std::string name;

public:
    TestArea(const std::string &name, bool recreateIfExists = false);

    fs::path getPath(const fs::path &p = "");
    fs::path getFolder(const fs::path &subfolder = "");
    fs::path downloadTestAsset(const std::string &url, const std::string &filename, bool overwrite = false);

    /**
     * @brief The folder of this process's test areas: <temp>/ddb_test_areas/<random>, the same
     * for the whole life of the process, not created until an area needs it.
     */
    static fs::path processRoot();

    /**
     * @brief Keeps processRoot() when the process ends, instead of deleting it (a failed run, to
     * look at its files).
     */
    static void keepProcessRoot();

    /**
     * @brief Clears all test areas by deleting the entire ddb_test_areas folder (the roots of
     * every process, also the ones of runs that kept theirs).
     */
    static void clearAll();
};

#endif // TESTAREA_H
