/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-15 probe: a host configures libddb before DDBRegisterProcess, as Registry does. Runs as
// its own process because DDBRegisterProcess initializes once per process (the test binaries
// register in main). The cache folder comes from DDB_PROBE_CACHE_DIR (an absolute path), so
// the command line needs no quoting. Exits 0 when DDBConfigure and DDBGetEffectiveConfig work
// before DDBRegisterProcess and the settings are still in effect after it, 1 otherwise (and
// says why on stderr).

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "ddb.h"

namespace
{
    // The effective configuration (compact JSON), or "" after printing the error
    std::string effectiveConfig(const char *when)
    {
        char *out = nullptr;
        if (DDBGetEffectiveConfig(&out) != DDBERR_NONE)
        {
            std::cerr << "DDBGetEffectiveConfig " << when << ": " << DDBGetLastError() << std::endl;
            return {};
        }
        std::string config = out;
        DDBFree(out);
        return config;
    }

    bool expectIn(const std::string &config, const std::string &text, const char *when)
    {
        if (config.find(text) != std::string::npos)
            return true;
        std::cerr << "Expected " << text << " " << when << " in " << config << std::endl;
        return false;
    }

    // Every configured value and its source, in the JSON of DDBGetEffectiveConfig
    bool configured(const std::string &config, const std::string &cacheJson, const char *when)
    {
        bool ok = expectIn(config, "\"cpuPoolSize\":1,", when);
        ok = expectIn(config, "\"ioThreads\":3,", when) && ok;
        ok = expectIn(config, "\"cacheDir\":" + cacheJson + ",", when) && ok;
        ok = expectIn(config, "\"sources\":{\"cacheDir\":\"configure\",\"cpuPoolSize\":\"configure\","
                              "\"gdalCacheMb\":\"default\",\"ioThreads\":\"configure\"",
                      when) && ok;
        return ok;
    }
}

int main()
{
    const char *cacheDir = std::getenv("DDB_PROBE_CACHE_DIR");
    if (cacheDir == nullptr || *cacheDir == '\0')
    {
        std::cerr << "DDB_PROBE_CACHE_DIR is not set" << std::endl;
        return 1;
    }
    // A JSON string of the folder: the test passes a path without quotes or control characters,
    // and backslashes (Windows) are escaped
    std::string cacheJson = "\"";
    for (const char *c = cacheDir; *c != '\0'; c++)
    {
        if (*c == '\\' || *c == '"')
            cacheJson += '\\';
        cacheJson += *c;
    }
    cacheJson += "\"";

    const std::string settings = "{\"cpuPoolSize\": 1, \"ioThreads\": 3, \"cacheDir\": " + cacheJson + "}";
    if (DDBConfigure(settings.c_str()) != DDBERR_NONE)
    {
        std::cerr << "DDBConfigure before DDBRegisterProcess: " << DDBGetLastError() << std::endl;
        return 1;
    }
    if (DDBConfigure("{\"noSuchSetting\": 1}") != DDBERR_INVALIDARGS)
    {
        std::cerr << "DDBConfigure accepted an unknown setting before DDBRegisterProcess" << std::endl;
        return 1;
    }
    const std::string before = effectiveConfig("before DDBRegisterProcess");
    if (before.empty() || !configured(before, cacheJson, "before DDBRegisterProcess"))
        return 1;

    if (DDBRegisterProcess(false) != DDBERR_NONE)
    {
        std::cerr << "DDBRegisterProcess: " << DDBGetLastError() << std::endl;
        return 1;
    }
    const std::string after = effectiveConfig("after DDBRegisterProcess");
    if (after.empty() || !configured(after, cacheJson, "after DDBRegisterProcess"))
        return 1;
    return 0;
}
