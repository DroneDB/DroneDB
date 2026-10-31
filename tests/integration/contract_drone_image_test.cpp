/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Contract fixtures of a real drone image (DJI, from DroneDB/test_data at the pinned
// commit). Unlike the generated JPEG of tests/unit/contract_test.cpp it has the camera, gimbal
// and relative altitude that give an image its footprint (polygon_geom) and its XMP
// properties. Integration test: the asset is downloaded (or read from DDB_TEST_CACHE_DIR).

#include <string>

#include "contract_fixtures.h"
#include "ddb.h"
#include "gtest/gtest.h"
#include "json.h"
#include "test.h"
#include "testarea.h"
#include "testfs.h"
#include "utils.h"

namespace
{
#define ASSERT_DDB_OK(call) ASSERT_EQ((call), DDBERR_NONE) << #call << ": " << DDBGetLastError()

    TEST(ContractFixturesDroneImage, InfoGetStac)
    {
        // Offline apart from the asset: no DSM download (the image has a relative altitude)
        EnvVarGuard noDsmKey("DDB_OPENTOPOGRAPHY_KEY");
        unsetEnv("DDB_OPENTOPOGRAPHY_KEY");

        TestArea ta(TEST_NAME, true);
        const fs::path image = ta.downloadTestAsset(testDataUrl("images/DJI_0018.JPG"), "DJI_0018.JPG");
        const fs::path root = ta.getFolder("dataset");
        contract::makeDataset(root, {image});
        const std::string r = root.string();
        const char *description = "test_data images/DJI_0018.JPG (DJI drone image) copied to <root> with mtime "
                                  "1700000000, indexed with DDBAdd";
        const auto fixture = [&](const std::string &name, const std::string &call, const std::string &schema,
                                 std::vector<std::string> volatileKeys)
        {
            return contract::Fixture{name, description, call, schema, contract::pathPlaceholders(root, "<root>"),
                                     std::move(volatileKeys), {}};
        };

        const std::string p = (root / "DJI_0018.JPG").string();
        const char *paths[] = {p.c_str()};
        char *out = nullptr;
        ASSERT_DDB_OK(DDBInfo(paths, 1, &out, "json", false, 0, "auto", true, true));
        // "depth" counts the folders of the absolute path (the temporary folder)
        contract::expectMatchesFixture(
            fixture("info-drone-image", "DDBInfo([<root>/DJI_0018.JPG], format=\"json\", recursive=false, "
                                        "maxRecursionDepth=0, geometry=\"auto\", withHash=true, stopOnError=true)",
                    "entries.schema.json", {"depth"}),
            contract::takeJson(out));

        ASSERT_DDB_OK(DDBGet(r.c_str(), "DJI_0018.JPG", &out));
        contract::expectMatchesFixture(
            fixture("get-drone-image", "DDBGet(<root>, \"DJI_0018.JPG\")", "entry.schema.json", {}),
            contract::takeJson(out));

        ASSERT_DDB_OK(DDBStac(r.c_str(), "DJI_0018.JPG", "https://hub.example/orgs/contract/ds/fixtures", "contract",
                              "https://hub.example", &out));
        contract::expectMatchesFixture(
            fixture("stac-item-drone-image",
                    "DDBStac(<root>, entry=\"DJI_0018.JPG\", stacCollectionRoot=\"https://hub.example/orgs/contract/ds/"
                    "fixtures\", id=\"contract\", stacCatalogRoot=\"https://hub.example\")",
                    "stac-item.schema.json", {}),
            contract::takeJson(out));

        DDBReleaseConnections(r.c_str(), nullptr);
    }
}
