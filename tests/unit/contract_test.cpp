/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// XI-09: golden contract fixtures of the libddb JSON outputs Registry consumes. Each test makes
// the contract dataset from tests/contract/inputs (committed files, fixed mtimes), calls the C
// API as Registry does, and checks the output against its JSON Schema and its golden file in
// tests/contract/fixtures (contract_fixtures.h). Any change to a field name, a type or a value
// fails here; an intended one is recorded with DDB_UPDATE_CONTRACT_FIXTURES=1 and listed in
// tests/contract/README.md. The drone image fixtures, which need a downloaded asset, are in
// tests/integration/contract_drone_image_test.cpp.

#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "contract_fixtures.h"
#include "ddb.h"
#include "gtest/gtest.h"
#include "json.h"
#include "json_schema_check.h"
#include "mio.h"
#include "test.h"
#include "testarea.h"
#include "utils.h"

namespace
{
    using contract::Fixture;

#define ASSERT_DDB_OK(call) ASSERT_EQ((call), DDBERR_NONE) << #call << ": " << DDBGetLastError()

    // The contract dataset: every file of tests/contract/inputs, indexed
    const std::vector<std::string> kInputs = {"a.txt", "cloud.las", "layer.geojson", "ortho.tif", "sub"};
    const char *kDescription = "The contract dataset: tests/contract/inputs (a.txt, cloud.las, layer.geojson, "
                               "ortho.tif, sub/img.jpg) copied to <root> with mtime 1700000000, indexed with DDBAdd";
    const char *kCollectionRoot = "https://hub.example/orgs/contract/ds/fixtures";
    const char *kCatalogRoot = "https://hub.example";

    class ContractFixtures : public ::testing::Test
    {
    protected:
        std::unique_ptr<TestArea> ta;
        fs::path root;
        json added;

        // Offline: no DSM download for the image without a relative altitude
        EnvVarGuard noDsmKey{"DDB_OPENTOPOGRAPHY_KEY"};

        void SetUp() override
        {
            unsetEnv("DDB_OPENTOPOGRAPHY_KEY");
            ta = std::make_unique<TestArea>(TEST_NAME, true);
            root = ta->getFolder("dataset");
            std::vector<fs::path> inputs;
            for (const auto &name : kInputs)
                inputs.push_back(contract::inputsDir() / name);
            added = contract::makeDataset(root, inputs);
        }

        void TearDown() override
        {
            // Close the pooled index connections, so the test area can be removed
            DDBReleaseConnections(root.string().c_str(), nullptr);
        }

        // A fixture of the contract dataset; its root is the placeholder "<root>"
        Fixture fixture(const std::string &name, const std::string &call, const std::string &schema,
                        std::vector<std::string> volatileKeys = {}) const
        {
            return {name, kDescription, call, schema, contract::pathPlaceholders(root, "<root>"),
                    std::move(volatileKeys), {}};
        }

        std::string path(const std::string &relative) const { return (root / relative).string(); }
    };

    TEST_F(ContractFixtures, Add)
    {
        contract::expectMatchesFixture(
            fixture("add", "DDBAdd(<root>, [<root>/a.txt, <root>/cloud.las, <root>/layer.geojson, <root>/ortho.tif, "
                           "<root>/sub], recursive=true)",
                    "entries.schema.json"),
            added);
    }

    TEST_F(ContractFixtures, List)
    {
        const std::string r = root.string();
        const char *paths[] = {r.c_str()};
        char *out = nullptr;
        ASSERT_DDB_OK(DDBList(r.c_str(), paths, 1, &out, "json", true, 0));
        contract::expectMatchesFixture(
            fixture("list", "DDBList(<root>, [<root>], format=\"json\", recursive=true, maxRecursionDepth=0)",
                    "entries.schema.json"),
            contract::takeJson(out));
    }

    TEST_F(ContractFixtures, ListFolder)
    {
        const std::string sub = path("sub");
        const char *paths[] = {sub.c_str()};
        char *out = nullptr;
        ASSERT_DDB_OK(DDBList(root.string().c_str(), paths, 1, &out, "json", false, 0));
        contract::expectMatchesFixture(
            fixture("list-folder", "DDBList(<root>, [<root>/sub], format=\"json\", recursive=false)",
                    "entries.schema.json"),
            contract::takeJson(out));
    }

    TEST_F(ContractFixtures, ListEntries)
    {
        char *out = nullptr;
        ASSERT_DDB_OK(DDBListEntries(root.string().c_str(), "", true, "", "", true, &out));
        contract::expectMatchesFixture(
            fixture("list-entries", "DDBListEntries(<root>, folder=\"\", recursive=true, namePattern=\"\", "
                                    "types=\"\", withDetails=true)",
                    "entries.schema.json"),
            contract::takeJson(out));
    }

    TEST_F(ContractFixtures, ListEntrySummaries)
    {
        char *out = nullptr;
        ASSERT_DDB_OK(DDBListEntries(root.string().c_str(), "", true, "", "", false, &out));
        contract::expectMatchesFixture(
            fixture("list-entry-summaries", "DDBListEntries(<root>, folder=\"\", recursive=true, namePattern=\"\", "
                                            "types=\"\", withDetails=false)",
                    "entry-summaries.schema.json"),
            contract::takeJson(out));
    }

    TEST_F(ContractFixtures, ListEntriesFiltered)
    {
        char *out = nullptr;
        ASSERT_DDB_OK(DDBListEntries(root.string().c_str(), "", true, "*.TIF", "georaster,image", false, &out));
        contract::expectMatchesFixture(
            fixture("list-entries-filtered", "DDBListEntries(<root>, folder=\"\", recursive=true, "
                                             "namePattern=\"*.TIF\", types=\"georaster,image\", withDetails=false)",
                    "entry-summaries.schema.json"),
            contract::takeJson(out));
    }

    TEST_F(ContractFixtures, Get)
    {
        for (const std::string entry : {"sub/img.jpg", "sub"})
        {
            char *out = nullptr;
            ASSERT_DDB_OK(DDBGet(root.string().c_str(), entry.c_str(), &out));
            const std::string name = entry == "sub" ? "get-folder" : "get-image";
            contract::expectMatchesFixture(fixture(name, "DDBGet(<root>, \"" + entry + "\")", "entry.schema.json"),
                                           contract::takeJson(out));
        }
    }

    // DDBInfo of each kind of input, as Registry calls it (json, withHash) before adding a file.
    // Its "depth" counts the folders of the absolute path (the temporary folder): normalised.
    TEST_F(ContractFixtures, Info)
    {
        const std::vector<std::pair<std::string, std::string>> inputs = {
            {"a.txt", "info-generic"},          {"cloud.las", "info-pointcloud"},
            {"layer.geojson", "info-vector"},   {"ortho.tif", "info-georaster"},
            {"sub/img.jpg", "info-image"},
        };
        for (const auto &[entry, name] : inputs)
        {
            const std::string p = path(entry);
            const char *paths[] = {p.c_str()};
            char *out = nullptr;
            ASSERT_DDB_OK(DDBInfo(paths, 1, &out, "json", false, 0, "auto", true, true));
            contract::expectMatchesFixture(
                fixture(name, "DDBInfo([<root>/" + entry + "], format=\"json\", recursive=false, maxRecursionDepth=0, "
                              "geometry=\"auto\", withHash=true, stopOnError=true)",
                        "entries.schema.json", {"depth"}),
                contract::takeJson(out));
        }
    }

    // Meta outputs in the order Registry's meta endpoints produce them. Meta items get a new
    // UUID ("id") and the current time ("mtime") when written: both are normalised.
    TEST_F(ContractFixtures, Meta)
    {
        const std::string r = root.string();
        const std::vector<std::string> volatileKeys = {"id", "mtime"};
        // Meta items carry the volatile keys; key lists and removal counts do not
        const auto check = [&](const std::string &name, const std::string &call, const std::string &schema,
                               char *out)
        {
            const bool items = schema == "meta-item.schema.json" || schema == "meta-items.schema.json";
            contract::expectMatchesFixture(fixture(name, call, schema, items ? volatileKeys : std::vector<std::string>{}),
                                           contract::takeJson(out));
        };
        char *out = nullptr;

        ASSERT_DDB_OK(DDBMetaAdd(r.c_str(), "", "tags", "\"survey\"", &out));
        check("meta-add", "DDBMetaAdd(<root>, \"\", \"tags\", \"\\\"survey\\\"\")", "meta-item.schema.json", out);
        ASSERT_DDB_OK(DDBMetaAdd(r.c_str(), "", "tags", "\"2024\"", &out));
        DDBFree(out);
        ASSERT_DDB_OK(DDBMetaSet(r.c_str(), "sub/img.jpg", "annotation", "{\"note\":\"tree\",\"level\":2}", &out));
        check("meta-set", "DDBMetaSet(<root>, \"sub/img.jpg\", \"annotation\", {\"note\":\"tree\",\"level\":2})",
              "meta-item.schema.json", out);
        ASSERT_DDB_OK(DDBMetaSet(r.c_str(), "", "name", "\"Contract dataset\"", &out));
        DDBFree(out);

        ASSERT_DDB_OK(DDBMetaGet(r.c_str(), "", "tags", &out));
        check("meta-get-list", "DDBMetaGet(<root>, \"\", \"tags\") (a key ending in s holds a list)",
              "meta-items.schema.json", out);
        ASSERT_DDB_OK(DDBMetaGet(r.c_str(), "sub/img.jpg", "annotation", &out));
        check("meta-get", "DDBMetaGet(<root>, \"sub/img.jpg\", \"annotation\")", "meta-item.schema.json", out);
        ASSERT_DDB_OK(DDBMetaList(r.c_str(), "", &out));
        check("meta-list", "DDBMetaList(<root>, \"\")", "meta-list.schema.json", out);
        ASSERT_DDB_OK(DDBMetaList(r.c_str(), "sub/img.jpg", &out));
        check("meta-list-entry", "DDBMetaList(<root>, \"sub/img.jpg\")", "meta-list.schema.json", out);
        ASSERT_DDB_OK(DDBMetaDump(r.c_str(), "[]", &out));
        const json dump = contract::takeJson(out);
        // The dump is ordered by id: random UUIDs
        Fixture dumpFixture = fixture("meta-dump", "DDBMetaDump(<root>, \"[]\")", "meta-dump.schema.json", volatileKeys);
        dumpFixture.sortBy = {"path", "key", "data"};
        contract::expectMatchesFixture(dumpFixture, dump);

        ASSERT_DDB_OK(DDBMetaUnset(r.c_str(), "", "name", &out));
        check("meta-unset", "DDBMetaUnset(<root>, \"\", \"name\")", "meta-count.schema.json", out);
        std::string surveyId;
        for (const auto &item : dump)
            if (item.at("data") == "\"survey\"")
                surveyId = item.at("id").get<std::string>();
        ASSERT_DDB_OK(DDBMetaRemove(r.c_str(), surveyId.c_str(), &out));
        check("meta-remove", "DDBMetaRemove(<root>, <id of the \"survey\" tag>)", "meta-count.schema.json", out);
    }

    // Build state: ortho.tif built (COG), layer.geojson buildable and not built, a.txt not
    // buildable, a path that is not indexed
    TEST_F(ContractFixtures, BuildStatus)
    {
        const std::string r = root.string();
        ASSERT_DDB_OK(DDBBuild(r.c_str(), "ortho.tif", nullptr, false, false));

        const char *paths[] = {"ortho.tif", "layer.geojson", "a.txt", "missing.tif"};
        char *out = nullptr;
        ASSERT_DDB_OK(DDBGetBuildStatus(r.c_str(), paths, 4, &out));
        contract::expectMatchesFixture(
            fixture("build-status", "DDBBuild(<root>, \"ortho.tif\"), then DDBGetBuildStatus(<root>, [\"ortho.tif\", "
                                    "\"layer.geojson\", \"a.txt\", \"missing.tif\"])",
                    "build-status.schema.json"),
            contract::takeJson(out));

        ASSERT_DDB_OK(DDBGetPendingBuildInfo(r.c_str(), &out));
        contract::expectMatchesFixture(fixture("pending-build-info-none", "DDBGetPendingBuildInfo(<root>) (nothing pending)",
                                               "pending-build-info.schema.json"),
                                       contract::takeJson(out));
    }

    // A build deferred for a missing dependency: a Shapefile without its .shx and .dbf. The
    // marker records the time of the attempt ("lastAttempt", normalised).
    TEST(ContractFixturesPending, PendingBuildInfo)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path root = ta.getFolder("dataset");
        const fs::path shp = ta.getPath("roads.shp");
        {
            std::ofstream(shp, std::ios::binary) << "not a real shapefile: its .shx and .dbf are missing";
        }
        contract::makeDataset(root, {shp});

        const std::string r = root.string();
        EXPECT_EQ(DDBBuild(r.c_str(), "roads.shp", nullptr, false, false), DDBERR_BUILDDEPMISSING) << DDBGetLastError();
        char *out = nullptr;
        ASSERT_DDB_OK(DDBGetPendingBuildInfo(r.c_str(), &out));
        contract::expectMatchesFixture(
            {"pending-build-info",
             "A dataset with roads.shp only (no .shx, .dbf), mtime 1700000000; DDBBuild deferred it",
             "DDBBuild(<root>, \"roads.shp\") -> DDBERR_BUILDDEPMISSING, then DDBGetPendingBuildInfo(<root>)",
             "pending-build-info.schema.json", contract::pathPlaceholders(root, "<root>"), {"lastAttempt"}, {}},
            contract::takeJson(out));
        DDBReleaseConnections(r.c_str(), nullptr);
    }

    // XI-04: the build manifest. ortho.tif built (COG), layer.geojson and cloud.las buildable and
    // not built, a.txt and the folder sub not buildable, a path that is not indexed
    TEST_F(ContractFixtures, BuildArtifacts)
    {
        EnvVarGuard noDiskFloor("DDB_MIN_FREE_DISK_MB", "0");
        const std::string r = root.string();
        ASSERT_DDB_OK(DDBBuild(r.c_str(), "ortho.tif", nullptr, false, false));

        const char *paths[] = {"ortho.tif", "layer.geojson", "cloud.las", "a.txt", "sub", "missing.tif"};
        char *out = nullptr;
        ASSERT_DDB_OK(DDBGetBuildArtifacts(r.c_str(), paths, 6, &out));
        const json artifacts = contract::takeJson(out);
        contract::expectMatchesFixture(
            fixture("build-artifacts",
                    "DDBBuild(<root>, \"ortho.tif\"), then DDBGetBuildArtifacts(<root>, [\"ortho.tif\", "
                    "\"layer.geojson\", \"cloud.las\", \"a.txt\", \"sub\", \"missing.tif\"])",
                    "build-artifacts.schema.json"),
            artifacts);

        DDBListEntriesOptions options{sizeof(DDBListEntriesOptions), true, false, true, 0};
        ASSERT_DDB_OK(DDBListEntriesEx(r.c_str(), "", "", "", &options, &out));
        contract::expectMatchesFixture(
            fixture("list-entries-ex",
                    "DDBBuild(<root>, \"ortho.tif\"), then DDBListEntriesEx(<root>, \"\", \"\", \"\", "
                    "{recursive=true, withDetails=false, withBuild=true})",
                    "list-entries-ex.schema.json"),
            contract::takeJson(out));

        const std::string hash = artifacts[0]["hash"].get<std::string>();
        ASSERT_DDB_OK(DDBResolveBuildFile(r.c_str(), hash.c_str(), "cog/cog.tif", &out));
        const std::string resolved = out;  // a path, not JSON
        DDBFree(out);
        contract::expectMatchesFixture(
            fixture("resolve-build-file",
                    "DDBBuild(<root>, \"ortho.tif\"), then DDBResolveBuildFile(<root>, <hash of ortho.tif>, "
                    "\"cog/cog.tif\")",
                    "build-file.schema.json"),
            json(resolved));

        // A second dataset with ortho.tif only: the build travels with the file
        const fs::path root2 = ta->getFolder("dataset2");
        contract::makeDataset(root2, {contract::inputsDir() / "ortho.tif"});
        const std::string r2 = root2.string();
        ASSERT_DDB_OK(DDBCopyBuildArtifacts(r.c_str(), r2.c_str(), hash.c_str(), &out));
        contract::expectMatchesFixture(
            fixture("copy-build-artifacts",
                    "DDBBuild(<root>, \"ortho.tif\"), then DDBCopyBuildArtifacts(<root>, <root2>, <hash of "
                    "ortho.tif>), <root2> a dataset with ortho.tif",
                    "copy-build-artifacts.schema.json"),
            contract::takeJson(out));
        DDBReleaseConnections(r2.c_str(), nullptr);
    }

    // A pending build in the manifest: a Shapefile without its .shx and .dbf ("lastAttempt",
    // normalised)
    TEST(ContractFixturesPending, BuildArtifactsPending)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path root = ta.getFolder("dataset");
        const fs::path shp = ta.getPath("roads.shp");
        {
            std::ofstream(shp, std::ios::binary) << "not a real shapefile: its .shx and .dbf are missing";
        }
        contract::makeDataset(root, {shp});

        const std::string r = root.string();
        EXPECT_EQ(DDBBuild(r.c_str(), "roads.shp", nullptr, false, false), DDBERR_BUILDDEPMISSING) << DDBGetLastError();
        const char *paths[] = {"roads.shp"};
        char *out = nullptr;
        ASSERT_DDB_OK(DDBGetBuildArtifacts(r.c_str(), paths, 1, &out));
        contract::expectMatchesFixture(
            {"build-artifacts-pending",
             "A dataset with roads.shp only (no .shx, .dbf), mtime 1700000000; DDBBuild deferred it",
             "DDBBuild(<root>, \"roads.shp\") -> DDBERR_BUILDDEPMISSING, then DDBGetBuildArtifacts(<root>, "
             "[\"roads.shp\"])",
             "build-artifacts.schema.json", contract::pathPlaceholders(root, "<root>"), {"lastAttempt"}, {}},
            contract::takeJson(out));
        DDBReleaseConnections(r.c_str(), nullptr);
    }

    // XI-04: the entry types and what libddb does with them (no dataset)
    TEST(ContractFixturesEntryTypes, EntryTypes)
    {
        char *out = nullptr;
        ASSERT_DDB_OK(DDBGetEntryTypes(&out));
        contract::expectMatchesFixture(
            {"entry-types", "No input: the registered entry types", "DDBGetEntryTypes()", "entry-types.schema.json",
             {}, {}, {}},
            contract::takeJson(out));
    }

    // DQ-15: the process configuration after a host set every setting (no dataset)
    TEST(ContractFixturesConfig, EffectiveConfig)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path cache = ta.getFolder("cache");
        const json settings = {{"cacheDir", ddb::io::toUtf8(cache)},
                               {"cpuPoolSize", 3},
                               {"ioThreads", 2},
                               {"minFreeDiskMb", 256}};
        const ScopedProcessConfig configured(settings.dump());
        char *out = nullptr;
        ASSERT_DDB_OK(DDBGetEffectiveConfig(&out));
        contract::expectMatchesFixture(
            {"effective-config", "Every setting set with DDBConfigure, cacheDir an absolute folder <cache>",
             "DDBConfigure({\"cacheDir\": \"<cache>\", \"cpuPoolSize\": 3, \"ioThreads\": 2, \"minFreeDiskMb\": "
             "256}), then DDBGetEffectiveConfig()",
             "effective-config.schema.json", contract::pathPlaceholders(cache, "<cache>"), {"hardwareThreads"}, {}},
            contract::takeJson(out));
    }

    TEST_F(ContractFixtures, Stac)
    {
        const std::string r = root.string();
        char *out = nullptr;
        ASSERT_DDB_OK(DDBStac(r.c_str(), "", kCollectionRoot, "contract", kCatalogRoot, &out));
        contract::expectMatchesFixture(
            fixture("stac-collection", std::string("DDBStac(<root>, entry=\"\", stacCollectionRoot=\"") +
                                           kCollectionRoot + "\", id=\"contract\", stacCatalogRoot=\"" + kCatalogRoot +
                                           "\")",
                    "stac-collection.schema.json"),
            contract::takeJson(out));

        for (const std::string entry : {"ortho.tif", "sub/img.jpg"})
        {
            ASSERT_DDB_OK(DDBStac(r.c_str(), entry.c_str(), kCollectionRoot, "contract", kCatalogRoot, &out));
            contract::expectMatchesFixture(
                fixture(entry == "ortho.tif" ? "stac-item-georaster" : "stac-item-image",
                        "DDBStac(<root>, entry=\"" + entry + "\", stacCollectionRoot=\"" + kCollectionRoot +
                            "\", id=\"contract\", stacCatalogRoot=\"" + kCatalogRoot + "\")",
                        "stac-item.schema.json"),
                contract::takeJson(out));
        }

        ASSERT_DDB_OK(DDBStacItemCollection(r.c_str(), kCollectionRoot, "contract", kCatalogRoot, "", "", 10, 0, &out));
        contract::expectMatchesFixture(
            fixture("stac-item-collection", std::string("DDBStacItemCollection(<root>, stacCollectionRoot=\"") +
                                                kCollectionRoot + "\", id=\"contract\", stacCatalogRoot=\"" +
                                                kCatalogRoot + "\", bbox=\"\", datetime=\"\", limit=10, offset=0)",
                    "stac-item-collection.schema.json"),
            contract::takeJson(out));
    }

    // XI-09: data-only STAC (ABI 1.1). Registry owns every STAC id and URL, so these outputs leave
    // out the members built from them
    TEST_F(ContractFixtures, StacData) {
        const std::string r = root.string();
        char* out = nullptr;
        ASSERT_DDB_OK(DDBStacData(r.c_str(), nullptr, &out));
        contract::expectMatchesFixture(fixture("stac-data-collection",
                                               "DDBStacData(<root>, entry=NULL)",
                                               "stac-data-collection.schema.json"),
                                       contract::takeJson(out));

        for (const std::string entry : {"ortho.tif", "sub/img.jpg"}) {
            ASSERT_DDB_OK(DDBStacData(r.c_str(), entry.c_str(), &out));
            contract::expectMatchesFixture(
                fixture(entry == "ortho.tif" ? "stac-data-item-georaster" : "stac-data-item-image",
                        "DDBStacData(<root>, entry=\"" + entry + "\")",
                        "stac-data-item.schema.json"),
                contract::takeJson(out));
        }

        ASSERT_DDB_OK(DDBStacItemCollectionData(r.c_str(), "", "", 10, 0, &out));
        contract::expectMatchesFixture(
            fixture(
                "stac-data-item-collection",
                "DDBStacItemCollectionData(<root>, bbox=\"\", datetime=\"\", limit=10, offset=0)",
                "stac-data-item-collection.schema.json"),
            contract::takeJson(out));
    }

    // A STAC document of DDBStac/DDBStacItemCollection without the members built from a URL or
    // an ID: what the data-only exports return
    json withoutUrlsAndIds(json doc) {
        doc.erase("id");
        doc.erase("collection");
        doc.erase("links");
        if (doc.contains("assets"))
            for (auto& [name, asset] : doc["assets"].items())
                asset.erase("href");
        if (doc.contains("features"))
            for (auto& feature : doc["features"])
                feature = withoutUrlsAndIds(feature);
        return doc;
    }

    // The data-only documents are the URL-building ones (same filters, id NULL) minus those
    // members, and nothing else: no field is computed twice
    TEST_F(ContractFixtures, StacDataIsStacWithoutUrlsAndIds) {
        const std::string r = root.string();
        for (const char* entry : {"", "ortho.tif", "sub/img.jpg", "layer.geojson", "cloud.las"}) {
            SCOPED_TRACE(entry);
            char* full = nullptr;
            char* data = nullptr;
            ASSERT_DDB_OK(DDBStac(r.c_str(), entry, kCollectionRoot, nullptr, kCatalogRoot, &full));
            ASSERT_DDB_OK(DDBStacData(r.c_str(), entry, &data));
            const json dataOnly = contract::takeJson(data);
            EXPECT_EQ(dataOnly, withoutUrlsAndIds(contract::takeJson(full)));
            EXPECT_EQ(dataOnly.dump().find("hub.example"), std::string::npos) << dataOnly.dump();
        }

        struct Filter {
            const char* bbox;
            const char* datetime;
            int limit;
            int offset;
        };
        for (const auto& f : {Filter{"", "", 10, 0},
                              Filter{"", "", 1, 1},
                              Filter{"-180,-90,180,90", "2023-11-01T00:00:00Z/..", 10, 0},
                              Filter{"", "../2000-01-01", 10, 0}}) {
            SCOPED_TRACE(std::string(f.bbox) + " " + f.datetime);
            char* full = nullptr;
            char* data = nullptr;
            ASSERT_DDB_OK(DDBStacItemCollection(r.c_str(),
                                                kCollectionRoot,
                                                nullptr,
                                                kCatalogRoot,
                                                f.bbox,
                                                f.datetime,
                                                f.limit,
                                                f.offset,
                                                &full));
            ASSERT_DDB_OK(
                DDBStacItemCollectionData(r.c_str(), f.bbox, f.datetime, f.limit, f.offset, &data));
            const json dataOnly = contract::takeJson(data);
            EXPECT_EQ(dataOnly, withoutUrlsAndIds(contract::takeJson(full)));
            EXPECT_EQ(dataOnly.dump().find("hub.example"), std::string::npos) << dataOnly.dump();
        }
    }

    TEST_F(ContractFixtures, StacDataErrors) {
        const std::string r = root.string();
        char* out = nullptr;

        EXPECT_EQ(DDBStacData(r.c_str(), "missing.tif", &out), DDBERR_NOTFOUND);
        EXPECT_STRNE(DDBGetLastError(), "");
        EXPECT_EQ(out, nullptr);
        // The URL-building export keeps its error code
        EXPECT_EQ(DDBStac(r.c_str(), "missing.tif", kCollectionRoot, nullptr, kCatalogRoot, &out),
                  DDBERR_EXCEPTION);

        // ABI 1.1 exports: an invalid argument is DDBERR_INVALIDARGS
        EXPECT_EQ(DDBStacData(nullptr, "", &out), DDBERR_INVALIDARGS);
        EXPECT_EQ(DDBStacData(r.c_str(), "", nullptr), DDBERR_INVALIDARGS);
        EXPECT_EQ(DDBStacItemCollectionData(nullptr, "", "", 10, 0, &out), DDBERR_INVALIDARGS);
        EXPECT_EQ(DDBStacItemCollectionData(r.c_str(), "", "", 10, 0, nullptr), DDBERR_INVALIDARGS);
        EXPECT_EQ(DDBStacItemCollectionData(r.c_str(), "1,2,3", "", 10, 0, &out), DDBERR_INVALIDARGS);
        EXPECT_EQ(DDBStacItemCollectionData(r.c_str(), "a,b,c,d", "", 10, 0, &out),
                  DDBERR_INVALIDARGS);
        EXPECT_STRNE(DDBGetLastError(), "");
        EXPECT_EQ(out, nullptr);
        // Not a dataset: not an argument error
        EXPECT_EQ(DDBStacData((root / "missing").string().c_str(), "", &out), DDBERR_EXCEPTION);
        EXPECT_EQ(out, nullptr);
    }

    TEST_F(ContractFixtures, RasterMetadata)
    {
        const std::string ortho = path("ortho.tif");
        char *out = nullptr;
        ASSERT_DDB_OK(DDBGetRasterInfo(ortho.c_str(), &out));
        contract::expectMatchesFixture(
            fixture("raster-info", "DDBGetRasterInfo(<root>/ortho.tif)", "raster-info.schema.json"),
            contract::takeJson(out));

        ASSERT_DDB_OK(DDBGetRasterMetadata(ortho.c_str(), nullptr, nullptr, &out));
        contract::expectMatchesFixture(
            fixture("raster-metadata", "DDBGetRasterMetadata(<root>/ortho.tif, formula=NULL, bandFilter=NULL)",
                    "raster-metadata.schema.json"),
            contract::takeJson(out));

        ASSERT_DDB_OK(DDBGetRasterMetadata(ortho.c_str(), "VARI", "RGB", &out));
        contract::expectMatchesFixture(
            fixture("raster-metadata-formula", "DDBGetRasterMetadata(<root>/ortho.tif, formula=\"VARI\", bandFilter=\"RGB\")",
                    "raster-metadata.schema.json"),
            contract::takeJson(out));
    }

    // XI-01: the ABI handshake Registry reads at startup. The library version changes with every
    // build ("version", normalised); a new export or error code changes the fixture on purpose.
    // The string is owned by the library: no DDBFree.
    TEST(ContractFixturesCapabilities, Capabilities)
    {
        const char *caps = DDBGetCapabilities();
        ASSERT_NE(caps, nullptr);
        contract::expectMatchesFixture({"capabilities", "No input: what the loaded library supports",
                                        "DDBGetCapabilities()", "capabilities.schema.json", {}, {"version"}, {}},
                                       json::parse(caps));
    }

    // XI-09: DDBNormalizePath, accepted and rejected (no dataset). The rules themselves are the
    // golden vectors of tests/contract/vectors/entry-paths.json (entry_path_test.cpp).
    TEST(ContractFixturesNormalizePath, NormalizePath) {
        char* out = nullptr;
        ASSERT_DDB_OK(DDBNormalizePath("photos\\2024/./a.jpg", R"([".uploads"])", &out));
        contract::expectMatchesFixture(
            {"normalize-path",
             "No dataset: a path with '\\' and '.' segments",
             R"(DDBNormalizePath("photos\\2024/./a.jpg", "[\".uploads\"]"))",
             "normalize-path.schema.json",
             {},
             {},
             {}},
            contract::takeJson(out));

        ASSERT_DDB_OK(DDBNormalizePath(".uploads/x", R"([".uploads"])", &out));
        contract::expectMatchesFixture({"normalize-path-rejected",
                                        "No dataset: a path under a reserved first segment",
                                        R"(DDBNormalizePath(".uploads/x", "[\".uploads\"]"))",
                                        "normalize-path.schema.json",
                                        {},
                                        {},
                                        {}},
                                       contract::takeJson(out));
    }
}

namespace
{
    // The committed files themselves, as Registry reads them: every fixture names an existing
    // schema and its output matches it, and every schema has a fixture
    TEST(ContractFiles, FixturesMatchTheirSchemas)
    {
        const fs::path dir = contract::contractDir();
        testutils::JsonSchemaCheck check(dir / "schemas");
        std::set<std::string> used;
        int fixtures = 0;
        for (const auto &e : fs::directory_iterator(dir / "fixtures"))
        {
            SCOPED_TRACE(e.path().filename().string());
            std::ifstream in(e.path(), std::ios::binary);
            const json fixture = json::parse(in);
            for (const auto *key : {"description", "call", "schema", "normalisation", "output"})
                EXPECT_TRUE(fixture.contains(key)) << key;
            const auto schema = fixture.at("schema").get<std::string>();
            used.insert(schema);
            const auto errors = check.validate(fixture.at("output"), schema);
            EXPECT_TRUE(errors.empty()) << json(errors).dump(2);
            fixtures++;
        }
        EXPECT_GT(fixtures, 0);
        for (const auto &e : fs::directory_iterator(dir / "schemas"))
            EXPECT_EQ(used.count(e.path().filename().string()), 1u)
                << e.path().filename() << " has no fixture";
    }
}
