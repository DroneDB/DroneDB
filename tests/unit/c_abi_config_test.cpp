/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// DQ-15 (XI-06): the C ABI of the process configuration, DDBConfigure and DDBGetEffectiveConfig
// (exports new in 2.0: an invalid argument is DDBERR_INVALIDARGS, outputs are written on
// DDBERR_NONE only). The output shape is pinned by the effective-config contract fixture
// (contract_test.cpp); that DDBConfigure works before DDBRegisterProcess is tested in its own
// process (tests/helpers/config_probe.cpp, integration).

#include <cstdlib>
#include <string>

#ifndef _WIN32
#include <sys/wait.h>
#endif

#include "contract_fixtures.h"
#include "ddb.h"
#include "gtest/gtest.h"
#include "json.h"
#include "json_schema_check.h"
#include "mio.h"
#include "parallel.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace
{
    using namespace ddb;

#define ASSERT_DDB_OK(call) ASSERT_EQ((call), DDBERR_NONE) << #call << ": " << DDBGetLastError()

    json effectiveConfig()
    {
        char *out = nullptr;
        EXPECT_EQ(DDBGetEffectiveConfig(&out), DDBERR_NONE) << DDBGetLastError();
        return contract::takeJson(out);
    }

    TEST(CAbiConfig, ConfigureRejectsInvalidArguments)
    {
        const json before = effectiveConfig();
        const char *invalid[] = {
            nullptr,
            "",
            "{",
            "[]",
            "42",
            R"({"cpuPoolSize": 2, "noSuchSetting": 1})",
            R"({"ioThreads": 0})",
            R"({"minFreeDiskMb": -1})",
            R"({"cacheDir": "not/absolute"})",
        };
        for (const char *config : invalid)
        {
            EXPECT_EQ(DDBConfigure(config), DDBERR_INVALIDARGS) << (config ? config : "NULL");
            EXPECT_STRNE(DDBGetLastError(), "") << (config ? config : "NULL");
            EXPECT_EQ(effectiveConfig(), before) << (config ? config : "NULL");
        }

        // The message names the unknown setting and the known ones
        EXPECT_EQ(DDBConfigure(R"({"cpuPoolSzie": 2})"), DDBERR_INVALIDARGS);
        const std::string message = DDBGetLastError();
        EXPECT_NE(message.find("cpuPoolSzie"), std::string::npos) << message;
        EXPECT_NE(message.find("cpuPoolSize"), std::string::npos) << message;

        // Nothing to change is fine, and clears the last error
        ASSERT_DDB_OK(DDBConfigure("{}"));
        EXPECT_STREQ(DDBGetLastError(), "");
        EXPECT_EQ(effectiveConfig(), before);
    }

    TEST(CAbiConfig, GetEffectiveConfig)
    {
        EXPECT_EQ(DDBGetEffectiveConfig(nullptr), DDBERR_INVALIDARGS);
        EXPECT_STRNE(DDBGetLastError(), "");

        const json config = effectiveConfig();
        const auto problems = testutils::JsonSchemaCheck(contract::contractDir() / "schemas")
                                  .validate(config, "effective-config.schema.json");
        EXPECT_TRUE(problems.empty()) << json(problems).dump(2);
        EXPECT_EQ(config["hardwareThreads"], parallel::hardwareThreads());
    }

    TEST(CAbiConfig, ConfigureWinsOverTheEnvironmentAndNullResets)
    {
        EnvVarGuard pool("DDB_CPU_POOL_SIZE", "5");
        const ScopedProcessConfig resetOnExit(R"({"cpuPoolSize": null, "ioThreads": null})");
        EXPECT_EQ(effectiveConfig()["settings"]["cpuPoolSize"], 5);
        EXPECT_EQ(effectiveConfig()["sources"]["cpuPoolSize"], "environment");

        ASSERT_DDB_OK(DDBConfigure(R"({"cpuPoolSize": 2})"));
        EXPECT_EQ(effectiveConfig()["settings"]["cpuPoolSize"], 2);
        EXPECT_EQ(effectiveConfig()["sources"]["cpuPoolSize"], "configure");
        EXPECT_EQ(parallel::tryAcquireThreads(10).count(), 2u);

        // Settings not named keep their value
        ASSERT_DDB_OK(DDBConfigure(R"({"ioThreads": 6})"));
        EXPECT_EQ(effectiveConfig()["settings"]["cpuPoolSize"], 2);

        ASSERT_DDB_OK(DDBConfigure(R"({"cpuPoolSize": null, "ioThreads": null})"));
        EXPECT_EQ(effectiveConfig()["settings"]["cpuPoolSize"], 5);
        EXPECT_EQ(effectiveConfig()["sources"]["cpuPoolSize"], "environment");
        EXPECT_EQ(effectiveConfig()["sources"]["ioThreads"], "default");
    }

    // The settings DDBGetEffectiveConfig returns are an object DDBConfigure accepts
    TEST(CAbiConfig, EffectiveSettingsRoundTrip)
    {
        TestArea ta(TEST_NAME, true);
        const json settings = {{"cacheDir", io::toUtf8(ta.getPath("cache"))},
                               {"cpuPoolSize", 3},
                               {"ioThreads", 2},
                               {"minFreeDiskMb", 1}};
        const ScopedProcessConfig configured(settings.dump());
        const json effective = effectiveConfig();
        EXPECT_EQ(effective["settings"], settings);
        for (const auto &[name, source] : effective["sources"].items())
            EXPECT_EQ(source, "configure") << name;

        ASSERT_DDB_OK(DDBConfigure(effective["settings"].dump().c_str()));
        EXPECT_EQ(effectiveConfig(), effective);
    }

    // A host configures libddb before DDBRegisterProcess, and the settings outlive it
    // (tests/helpers/config_probe.cpp, its own process: this one registered in main)
    TEST(CAbiConfig, ConfigureBeforeRegisterProcess)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path probe = io::getExeFolderPath() / DDB_CONFIG_PROBE_NAME;
        ASSERT_TRUE(fs::exists(probe)) << probe;
        const EnvVarGuard cacheDir("DDB_PROBE_CACHE_DIR", ta.getPath("probe cache").string());
        const int rc = std::system(("\"" + probe.string() + "\"").c_str());
#ifdef _WIN32
        EXPECT_EQ(rc, 0) << "see the probe's stderr";
#else
        ASSERT_TRUE(WIFEXITED(rc)) << "probe did not exit normally: " << rc;
        EXPECT_EQ(WEXITSTATUS(rc), 0) << "see the probe's stderr";
#endif
    }

} // namespace
