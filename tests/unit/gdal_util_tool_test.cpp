/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The gdal-util program itself (apps/gdal-util/PROTOCOL.md), driven through process::Session the
// way a host drives it and without a client of libddb: the version line, the probe of rasters and
// vectors against what the library finds in the process, the thumbnail operation against the
// thumbnail the library writes, a request that is refused or fails without stopping the server,
// what stdout carries, pipelined requests, end of input, and the one-shot mode. The program is the
// one built next to the test binary. Waits are bounded deadlines, never sleeps used for
// synchronization.

#include <chrono>
#include <cstdint>
#include <fstream>
#include <algorithm>
#include <map>
#include <memory>
#include <regex>
#include <string>
#include <vector>

#include "contract_fixtures.h"
#include "fs.h"
#include "gdal_inc.h"
#include "gdal_raii.h"
#include "gtest/gtest.h"
#include "json.h"
#include "json_schema_check.h"
#include "mio.h"
#include "process_runner.h"
#include "cad.h"
#include "entry.h"
#include "exif_metadata.h"
#include "in_process_raster_helper.h"
#include "raster_helper.h"
#include "raster_ops.h"
#include "raster_protocol.h"
#include "raster_test_utils.h"
#include "sandbox/sandbox.h"
#include "testarea.h"
#include "thermal.h"
#include "vegetation.h"
#include "tool_locator.h"
#include "user_open_child.h"

namespace
{
    using namespace ddb;
    using process::Session;
    using std::chrono::seconds;

    constexpr seconds kPatience(60);
    constexpr const char *kToolName = "gdal-util";
    constexpr const char *kToolEnv = "DDB_GDAL_UTIL_PATH";

    Session::Clock::time_point patiently() { return Session::Clock::now() + kPatience; }

    fs::path tool() { return tools::findTool(kToolName, kToolEnv); }

    void writeFile(const fs::path &file, const std::string &content)
    {
        std::ofstream out(file, std::ios::binary);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    std::string readFile(const fs::path &file)
    {
        std::ifstream in(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    // A JPEG of 96 x 64 pixels, written with GDAL's JPEG driver: RGB, or grey with one band
    fs::path writeJpeg(const fs::path &path, int bands = 3)
    {
        GDALDriverH memory = GDALGetDriverByName("MEM");
        GDALDriverH jpeg = GDALGetDriverByName("JPEG");
        EXPECT_NE(memory, nullptr);
        EXPECT_NE(jpeg, nullptr);
        GDALDatasetH source = GDALCreate(memory, "", 96, 64, bands, GDT_Byte, nullptr);
        std::vector<uint8_t> row(96);
        for (int band = 1; band <= bands; band++)
            for (int y = 0; y < 64; y++)
            {
                for (int x = 0; x < 96; x++)
                    row[static_cast<size_t>(x)] = static_cast<uint8_t>((x * band + y) % 256);
                EXPECT_EQ(GDALRasterIO(GDALGetRasterBand(source, band), GF_Write, 0, y, 96, 1, row.data(), 96, 1, GDT_Byte, 0, 0),
                          CE_None);
            }
        GDALDatasetH copy = GDALCreateCopy(jpeg, io::toUtf8(path).c_str(), source, FALSE, nullptr, nullptr, nullptr);
        EXPECT_NE(copy, nullptr);
        GDALClose(copy);
        GDALClose(source);
        return path;
    }

    // The program, run to its end with the arguments given and nothing on stdin
    struct ToolRun
    {
        std::vector<std::string> lines;
        std::string stderrText;
        bool exited = false;
        int exitCode = -1;
    };

    ToolRun runTool(const std::vector<std::string> &args)
    {
        ToolRun run;
        Session session;
        if (!session.start(tool(), args))
        {
            ADD_FAILURE() << session.startError();
            return run;
        }
        session.closeStdin();
        for (;;)
        {
            const auto line = session.readLine(patiently(), raster::kMaxLineBytes * 2);
            if (line.status != Session::ReadStatus::Line)
            {
                EXPECT_EQ(line.status, Session::ReadStatus::Eof);
                if (!line.line.empty())
                    run.lines.push_back(line.line);
                break;
            }
            run.lines.push_back(line.line);
        }
        EXPECT_TRUE(session.wait(patiently()));
        run.exited = session.exitStatus().exited;
        run.exitCode = session.exitStatus().exitCode;
        run.stderrText = session.stderrTail();
        return run;
    }

    // A running --serve
    class Server
    {
      public:
        explicit Server(const process::SessionOptions &options = process::SessionOptions())
        {
            started_ = session_.start(tool(), {"--serve"}, options);
            EXPECT_TRUE(started_) << session_.startError();
        }

        void send(const std::string &line)
        {
            ASSERT_EQ(session_.writeLine(line, patiently()), Session::WriteStatus::Ok);
        }

        // The next line the program writes; the test fails when there is none
        std::string receive()
        {
            const auto reply = session_.readLine(patiently(), raster::kMaxLineBytes * 2);
            EXPECT_EQ(reply.status, Session::ReadStatus::Line);
            return reply.line;
        }

        // The response to a line: it must be a valid response, as the host would read it
        raster::Response ask(const std::string &line)
        {
            send(line);
            const std::string reply = receive();
            try
            {
                return raster::decodeResponse(reply);
            }
            catch (const raster::ProtocolError &e)
            {
                ADD_FAILURE() << "not a response (" << e.what() << "): " << reply.substr(0, 300);
                return raster::Response();
            }
        }

        raster::Response ask(const raster::Request &request) { return ask(raster::encodeRequest(request)); }

        Session &session() { return session_; }

      private:
        Session session_;
        bool started_ = false;
    };

    raster::Request probeRequest(int64_t id, const fs::path &path, const raster::ProbeParams &params = {})
    {
        raster::Request request;
        request.id = id;
        request.op = raster::op::kProbe;
        request.path = path;
        request.params = raster::toJson(params);
        return request;
    }

    raster::Request thumbnailRequest(int64_t id, const fs::path &path, const fs::path &out,
                                     const raster::ThumbnailParams &params)
    {
        raster::Request request;
        request.id = id;
        request.op = raster::op::kThumbnail;
        request.path = path;
        request.out = out;
        request.params = raster::toJson(params);
        return request;
    }

    raster::Request normalizeRequest(int64_t id, const fs::path &path, const fs::path &out)
    {
        raster::Request request;
        request.id = id;
        request.op = raster::op::kNormalize;
        request.path = path;
        request.out = out;
        return request;
    }

    raster::ProbeParams vectorParams()
    {
        raster::ProbeParams params;
        params.kind = raster::ProbeKind::Vector;
        return params;
    }

    // A folder with a GeoTIFF, a JPEG, a GeoJSON, a text file named like a raster and a text
    // file, and the program located
    class GdalUtilToolTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            ASSERT_FALSE(tool().empty()) << "gdal-util is built next to the test binary";
            area_ = std::make_unique<TestArea>(std::string("GdalUtil.") +
                                                   ::testing::UnitTest::GetInstance()->current_test_info()->name(),
                                               true);
            testutils::GeoRasterSpec spec;
            spec.width = 200;
            spec.height = 120;
            spec.bands = 3;
            spec.epsg = 32632;
            spec.originX = 500000.0;
            spec.originY = 5000000.0;
            spec.pixelSize = 0.5;
            spec.value = [](int band, int x, int y) { return static_cast<double>((x * band + y) % 256); };
            geotiff_ = testutils::createGeoRaster(area_->getFolder() / "ortho.tif", spec);
            jpeg_ = writeJpeg(area_->getFolder() / "photo.jpg");
            geojson_ = area_->getFolder() / "layer.geojson";
            writeFile(geojson_,
                      R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":{},)"
                      R"("geometry":{"type":"Point","coordinates":[9.1,45.2]}},{"type":"Feature","properties":{},)"
                      R"("geometry":{"type":"Point","coordinates":[9.3,45.4]}}]})");
            fakeRaster_ = area_->getFolder() / "fake.tif";
            writeFile(fakeRaster_, "this is not a raster");
            text_ = area_->getFolder() / "notes.txt";
            writeFile(text_, "plain text");
        }

        std::unique_ptr<TestArea> area_;
        fs::path geotiff_;
        fs::path jpeg_;
        fs::path geojson_;
        fs::path fakeRaster_;
        fs::path text_;
    };

    // ------------------------------------------------------------------ --version

    TEST(GdalUtilTool, versionPrintsTheVersionAndTheProtocolOfTheLibrary)
    {
        ASSERT_FALSE(tool().empty()) << "gdal-util is built next to the test binary";
        const ToolRun run = runTool({"--version"});
        ASSERT_EQ(run.lines.size(), 1u);
        EXPECT_TRUE(std::regex_match(run.lines[0], std::regex(R"(gdal-util \S+ protocol 1\.[0-9]+)"))) << run.lines[0];
        EXPECT_TRUE(run.exited);
        EXPECT_EQ(run.exitCode, 0);
        EXPECT_TRUE(run.stderrText.empty()) << run.stderrText;

        const auto version = raster::parseVersionOutput(run.lines[0]);
        ASSERT_TRUE(version.has_value());
        EXPECT_EQ(version->major, 1);
        EXPECT_TRUE(raster::isAccepted(*version));
    }

    TEST(GdalUtilTool, anArgumentThatIsNoModeIsAUsageError)
    {
        for (const auto &args : {std::vector<std::string>{}, std::vector<std::string>{"--bogus"},
                                 std::vector<std::string>{"--request"}, std::vector<std::string>{"--serve", "x"}})
        {
            const ToolRun run = runTool(args);
            EXPECT_TRUE(run.lines.empty());
            EXPECT_TRUE(run.exited);
            EXPECT_EQ(run.exitCode, 2);
            EXPECT_NE(run.stderrText.find("usage"), std::string::npos) << run.stderrText;
        }
    }

    // ------------------------------------------------------------------ probe

    TEST_F(GdalUtilToolTest, probeOfAGeoTiffGivesTheSizeTheBandsAndTheProjection)
    {
        Server server;
        const auto response = server.ask(probeRequest(11, geotiff_));
        ASSERT_TRUE(response.ok) << response.message;
        EXPECT_EQ(response.id, std::optional<int64_t>(11));

        const raster::ProbeResult result = raster::probeResultFromJson(response.result);
        EXPECT_EQ(result.type, "GeoRaster");
        EXPECT_EQ(result.properties.at("width"), 200);
        EXPECT_EQ(result.properties.at("height"), 120);
        ASSERT_EQ(result.properties.at("bands").size(), 3u);
        EXPECT_EQ(result.properties.at("bands")[0].at("type"), "Byte");
        EXPECT_EQ(result.properties.at("bands")[0].at("colorInterp"), "Red");
        EXPECT_EQ(result.properties.at("geotransform").size(), 6u);
        EXPECT_NE(result.wkt.find("32632"), std::string::npos) << result.wkt;
        EXPECT_EQ(result.polygonGeom.points.size(), 5u);
        EXPECT_EQ(result.pointGeom.points.size(), 1u);
        // Between 8 and 10 degrees east, near 45 degrees north
        EXPECT_NEAR(result.pointGeom.points[0].y, 45.1, 0.2);
        EXPECT_NEAR(result.pointGeom.points[0].x, 9.0, 1.5);
    }

    TEST_F(GdalUtilToolTest, probeOfARasterWithoutProjectionSaysSo)
    {
        const fs::path plain = area_->getFolder() / "plain.tif";
        {
            GDALDatasetH ds = GDALCreate(GDALGetDriverByName("GTiff"), io::toUtf8(plain).c_str(), 8, 8, 1, GDT_Byte, nullptr);
            ASSERT_NE(ds, nullptr);
            GDALClose(ds);
        }
        Server server;
        const auto response = server.ask(probeRequest(1, plain));
        ASSERT_TRUE(response.ok) << response.message;
        const raster::ProbeResult result = raster::probeResultFromJson(response.result);
        EXPECT_EQ(result.type, "Raster");
        EXPECT_TRUE(result.wkt.empty());
        EXPECT_TRUE(result.polygonGeom.empty());
    }

    TEST_F(GdalUtilToolTest, probeOfAFileThatIsNoRasterIsUnreadable)
    {
        Server server;
        for (const fs::path &file : {fakeRaster_, text_, area_->getFolder() / "missing.tif"})
        {
            SCOPED_TRACE(file.filename().string());
            const auto response = server.ask(probeRequest(5, file));
            EXPECT_FALSE(response.ok);
            EXPECT_EQ(response.error, raster::ErrorCode::Unreadable);
            EXPECT_EQ(response.id, std::optional<int64_t>(5));
            EXPECT_FALSE(response.message.empty());
        }
        // The server is still there
        EXPECT_TRUE(server.ask(probeRequest(6, geotiff_)).ok);
    }

    TEST_F(GdalUtilToolTest, probeOfAVectorGivesLayersAndFootprint)
    {
        Server server;
        const auto response = server.ask(probeRequest(3, geojson_, vectorParams()));
        ASSERT_TRUE(response.ok) << response.message;
        const raster::ProbeResult result = raster::probeResultFromJson(response.result);
        EXPECT_EQ(result.type, "Vector");
        const json &vector = result.properties.at("vector");
        EXPECT_FALSE(vector.at("driver").get<std::string>().empty());
        ASSERT_EQ(vector.at("layers").size(), 1u);
        EXPECT_EQ(vector.at("layers")[0].at("featureCount"), 2);
        ASSERT_EQ(result.polygonGeom.points.size(), 5u);
        EXPECT_DOUBLE_EQ(result.polygonGeom.points[0].x, 9.1);
        EXPECT_DOUBLE_EQ(result.polygonGeom.points[2].y, 45.4);
    }

    TEST_F(GdalUtilToolTest, probeOfAVectorFileThatIsNoVectorIsUnreadable)
    {
        Server server;
        const auto response = server.ask(probeRequest(4, text_, vectorParams()));
        EXPECT_FALSE(response.ok);
        EXPECT_EQ(response.error, raster::ErrorCode::Unreadable);
    }

    // The entry of a file is built from the probe: the helper must tell exactly what the library
    // finds in the process, down to the last digit of a coordinate
    USER_OPEN_TEST_F(GdalUtilToolTest, theProbeOfTheHelperEqualsTheProbeInTheProcess)
    {
        Server server;
        const std::vector<std::pair<fs::path, raster::ProbeParams>> files = {
            {geotiff_, raster::ProbeParams()}, {geojson_, vectorParams()}};
        for (const auto &[file, params] : files)
        {
            SCOPED_TRACE(file.filename().string());
            const auto response = server.ask(probeRequest(8, file, params));
            ASSERT_TRUE(response.ok) << response.message;
            EXPECT_EQ(response.result, raster::toJson(raster::probe(file, params)));
        }
    }

    TEST_F(GdalUtilToolTest, theProbeResponseMatchesItsSchema)
    {
        testutils::JsonSchemaCheck check(contract::contractDir() / "schemas");
        Server server;
        const std::vector<raster::Request> requests = {probeRequest(1, geotiff_), probeRequest(2, geojson_, vectorParams()),
                                                       probeRequest(3, fakeRaster_)};
        for (const auto &request : requests)
        {
            SCOPED_TRACE(request.path.filename().string());
            server.send(raster::encodeRequest(request));
            const json line = json::parse(server.receive());
            const auto errors = check.validate(line, "gdal-util-probe.schema.json");
            EXPECT_TRUE(errors.empty()) << json(errors).dump(2);
        }
    }

    TEST_F(GdalUtilToolTest, theSchemaTakesTheWarningOfAProbeThatDescribesTheFileOnlyInPart)
    {
        testutils::JsonSchemaCheck check(contract::contractDir() / "schemas");
        Server server;
        json line = json::parse(raster::encodeResponse(server.ask(probeRequest(1, geojson_, vectorParams()))));
        ASSERT_TRUE(check.validate(line, "gdal-util-probe.schema.json").empty());

        line["result"]["warning"] = "the coordinate system of the drawing cannot be used";
        EXPECT_TRUE(check.validate(line, "gdal-util-probe.schema.json").empty());
        line["result"]["warning"] = "";
        EXPECT_FALSE(check.validate(line, "gdal-util-probe.schema.json").empty()) << "a warning has some text";
        line["result"]["warning"] = 7;
        EXPECT_FALSE(check.validate(line, "gdal-util-probe.schema.json").empty());
    }

    TEST(GdalUtilTool, theProbeResponseOfAContractRasterIsPinned)
    {
        ASSERT_FALSE(tool().empty());
        // The raster of the contract dataset (tests/contract/inputs): the same file the
        // other contract fixtures describe
        const fs::path ortho = contract::contractDir() / "inputs" / "ortho.tif";
        Server server;
        server.send(raster::encodeRequest(probeRequest(1, fs::absolute(ortho))));
        const json response = json::parse(server.receive());
        ASSERT_TRUE(response.at("ok").get<bool>()) << response.dump();

        contract::Fixture fixture;
        fixture.name = "gdal-util-probe";
        fixture.description = "The response of gdal-util to a probe request (kind raster) of ortho.tif of the contract inputs";
        fixture.call = R"(gdal-util --serve: {"id":1,"op":"probe","path":"<ortho.tif>","params":{"kind":"raster"}})";
        fixture.schema = "gdal-util-probe.schema.json";
        contract::expectMatchesFixture(fixture, response);
    }

    // ------------------------------------------------------------------ thumbnail

    USER_OPEN_TEST_F(GdalUtilToolTest, aJpegThumbnailIsAValidWebpFileEqualToTheOneInTheProcess)
    {
        const fs::path out = area_->getFolder() / "photo.webp";
        raster::ThumbnailParams params;
        params.size = 32;

        Server server;
        const auto response = server.ask(thumbnailRequest(21, jpeg_, out, params));
        ASSERT_TRUE(response.ok) << response.message;
        EXPECT_EQ(response.result.at("bytes").get<uint64_t>(), fs::file_size(out));

        const std::string bytes = readFile(out);
        ASSERT_GT(bytes.size(), 12u);
        EXPECT_EQ(bytes.substr(0, 4), "RIFF");
        EXPECT_EQ(bytes.substr(8, 4), "WEBP");
        const testutils::DecodedImage image = testutils::decodeImageFile(out);
        EXPECT_EQ(image.width, 32);
        EXPECT_EQ(image.height, 21);

        const fs::path expected = area_->getFolder() / "expected.webp";
        raster::thumbnail(jpeg_, expected, params);
        EXPECT_EQ(bytes, readFile(expected));
    }

    USER_OPEN_TEST_F(GdalUtilToolTest, aThumbnailWithVisualisationParametersEqualsTheOneInTheProcess)
    {
        const fs::path out = area_->getFolder() / "bands.webp";
        raster::ThumbnailParams params;
        params.size = 48;
        params.vis = ThumbVisParams();
        params.vis->bands = "3,2,1";

        Server server;
        const auto response = server.ask(thumbnailRequest(22, geotiff_, out, params));
        ASSERT_TRUE(response.ok) << response.message;

        const fs::path expected = area_->getFolder() / "expected.webp";
        raster::thumbnail(geotiff_, expected, params);
        EXPECT_EQ(readFile(out), readFile(expected));

        // The plain thumbnail of the same file is another picture: the bands are not swapped
        const fs::path plain = area_->getFolder() / "plain.webp";
        raster::thumbnail(geotiff_, plain, raster::ThumbnailParams{48, std::nullopt});
        EXPECT_NE(readFile(out), readFile(plain));
    }

    // A thumbnail with visualisation parameters detects the sensor of the raster, a reading of the
    // raster by its path that the host does through a copy in its cache. The program reads the file
    // of its request itself: it starts no other program (the one it would start does not exist here)
    // and writes nothing in the cache of the user, outside the folder of the request. The sandbox is
    // off, so a write it would block is seen as written.
    TEST_F(GdalUtilToolTest, aThumbnailThatDetectsTheSensorStartsNoOtherHelperAndWritesNoCopy)
    {
        const fs::path home = area_->getFolder() / "home";
        fs::create_directories(home);
        process::SessionOptions options;
        options.envOverrides = {{"DDB_HOME", io::toUtf8(home)},
                                {"DDB_GDAL_SANDBOX", "off"},
                                {kToolEnv, io::toUtf8(area_->getFolder() / "no-such-gdal-util")}};
        Server server(options);

        testutils::GeoRasterSpec spec;
        spec.width = 120;
        spec.height = 80;
        spec.bands = 4;
        spec.type = GDT_UInt16;
        spec.value = [](int band, int x, int y) { return static_cast<double>(1000 * band + x + y); };
        const fs::path multispectral = testutils::createGeoRaster(area_->getFolder() / "multispectral.tif", spec);

        int64_t id = 31;
        for (const fs::path &raster : {geotiff_, multispectral})
        {
            const fs::path out = area_->getFolder() / (raster.stem().string() + "-vis.webp");
            const auto response = server.ask(thumbnailRequest(id++, raster, out, raster::ThumbnailParams{32, ThumbVisParams()}));
            ASSERT_TRUE(response.ok) << io::toUtf8(raster) << ": " << response.message;
            EXPECT_GT(fs::file_size(out), 0u);
        }

        for (const auto &entry : fs::recursive_directory_iterator(home))
            EXPECT_FALSE(entry.is_regular_file()) << "the helper wrote " << entry.path();
    }

    // Whether every layer of the sandbox is there to confine a worker in strict mode
    bool strictModeAvailable()
    {
        const auto layers = sandbox::probe().layers;
        return !layers.empty() &&
               std::all_of(layers.begin(), layers.end(), [](const auto &layer) { return layer.active; });
    }

    // A formula over a single-band raster of no known sensor asks whether the raster is thermal:
    // its GDAL metadata, which the program reads, and its tags, which the host reads and says in the
    // request (thermalTags). The program reads no tags: a worker in strict mode cannot start
    // metadata-util, and here there is none to start. Its thumbnails are the ones of the process.
    USER_OPEN_TEST_F(GdalUtilToolTest, aFormulaOverASingleBandRasterIsRenderedInTheSandboxWithoutReadingTags)
    {
        process::SessionOptions options;
        options.envOverrides = {{"DDB_GDAL_SANDBOX", strictModeAvailable() ? "strict" : "best-effort"},
                                {"DDB_METADATA_UTIL_PATH", io::toUtf8(area_->getFolder() / "no-such-metadata-util")}};
        Server server(options);

        testutils::GeoRasterSpec spec;
        spec.width = 60;
        spec.height = 40;
        spec.bands = 1;
        spec.type = GDT_Float32;
        spec.value = [](int, int x, int y) { return 15.0 + 0.25 * x + 0.1 * y; };
        spec.bandNames = {"surface temperature"};
        const fs::path labelled = testutils::createGeoRaster(area_->getFolder() / "labelled.tif", spec);
        spec.bandNames.clear();
        const fs::path unlabelled = testutils::createGeoRaster(area_->getFolder() / "unlabelled.tif", spec);

        const json celsius = {{"size", 32}, {"vis", {{"formula", "CELSIUS"}}}};
        json tagged = celsius;
        tagged["thermalTags"] = true;

        // Thermal by its metadata, and by its tags as the host found them
        int64_t id = 41;
        for (const auto &[raster, params] : {std::pair(labelled, celsius), std::pair(unlabelled, tagged)})
        {
            const fs::path out = area_->getFolder() / (raster.stem().string() + "-celsius.webp");
            raster::Request request = thumbnailRequest(id++, raster, out, raster::ThumbnailParams{});
            request.params = params;
            const auto response = server.ask(request);
            ASSERT_TRUE(response.ok) << io::toUtf8(raster) << ": " << response.message;

            const fs::path expected = area_->getFolder() / (raster.stem().string() + "-expected.webp");
            raster::thumbnail(raster, expected, raster::thumbnailParamsFromJson(params));
            EXPECT_EQ(readFile(out), readFile(expected)) << io::toUtf8(raster);
        }

        // The band the formula reads: the thermal one of the labelled raster, and of the unlabelled
        // one only when the tags say it is thermal
        const auto &engine = VegetationEngine::instance();
        EXPECT_EQ(engine.autoDetectFilter(io::toUtf8(labelled), false).id, "T");
        EXPECT_EQ(engine.autoDetectFilter(io::toUtf8(unlabelled), true).id, "T");
        EXPECT_EQ(engine.autoDetectFilter(io::toUtf8(unlabelled), false).id, "RGB");
    }

    // The real program, recording the thumbnail params the host sends it
    class RecordingHelper : public testutils::InProcessRasterHelper
    {
      public:
        void thumbnail(const fs::path &path, const fs::path &out, const raster::ThumbnailParams &params) override
        {
            sent.push_back(params);
            program.thumbnail(path, out, params);
        }

        std::vector<raster::ThumbnailParams> sent;
        raster::GdalUtilHelper program;
    };

    // The host reads the tags of the file it asks a thumbnail of when the rendering asks whether the
    // file is thermal (a formula whose band filter is detected), and says in the request what they
    // say: a grey JPEG whose camera is a thermal one, the same JPEG without the tag
    TEST_F(GdalUtilToolTest, theHostSendsWhatTheTagsOfTheFileSayWhenTheFormulaAsksWhetherItIsThermal)
    {
        const fs::path thermal = writeJpeg(area_->getFolder() / "thermal.jpg", 1);
        const fs::path grey = writeJpeg(area_->getFolder() / "grey.jpg", 1);
        const auto written = exif::metadataReader().write(thermal, {{"Exif.Image.Make", "FLIR"}});
        ASSERT_TRUE(written.ok) << written.message;

        RecordingHelper helper;
        const raster::ScopedRasterHelper installed(helper);
        raster::ThumbnailParams celsius{32, ThumbVisParams()};
        celsius.vis->formula = "CELSIUS";
        raster::ThumbnailParams filtered = celsius;
        filtered.vis->bandFilter = "RGB";
        for (const auto &[file, params] : {std::pair(thermal, celsius), std::pair(grey, celsius), std::pair(thermal, filtered)})
            EXPECT_EQ(raster::renderThumbnailWebp(file, params).substr(0, 4), "RIFF") << io::toUtf8(file);

        ASSERT_EQ(helper.sent.size(), 3u);
        EXPECT_TRUE(helper.sent[0].thermalTags);
        EXPECT_FALSE(helper.sent[1].thermalTags);
        // A filter that is given detects nothing: no tag is read for it
        EXPECT_FALSE(helper.sent[2].thermalTags);
        helper.program.shutdown();
    }

    TEST_F(GdalUtilToolTest, aThumbnailOfAFileThatIsNoRasterIsUnreadableAndWritesNothing)
    {
        const fs::path out = area_->getFolder() / "none.webp";
        Server server;
        const auto response = server.ask(thumbnailRequest(23, fakeRaster_, out, raster::ThumbnailParams{32, std::nullopt}));
        EXPECT_FALSE(response.ok);
        EXPECT_EQ(response.error, raster::ErrorCode::Unreadable);
        EXPECT_FALSE(fs::exists(out));
    }

    // ------------------------------------------------------------------ normalize

    TEST_F(GdalUtilToolTest, aNormalizedPhotoHoldsThePixelsOfTheJpeg)
    {
        const fs::path out = area_->getFolder() / "photo-normalized.tif";
        Server server;
        const auto response = server.ask(normalizeRequest(31, jpeg_, out));
        ASSERT_TRUE(response.ok) << response.message;

        const auto in = adoptDataset(GDALOpenEx(io::toUtf8(jpeg_).c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr));
        const auto copy = adoptDataset(GDALOpenEx(io::toUtf8(out).c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr));
        ASSERT_TRUE(in);
        ASSERT_TRUE(copy);
        EXPECT_STREQ(copy->GetDriverName(), "GTiff");
        ASSERT_EQ(copy->GetRasterCount(), in->GetRasterCount());
        const int width = in->GetRasterXSize();
        const int height = in->GetRasterYSize();
        ASSERT_EQ(copy->GetRasterXSize(), width);
        ASSERT_EQ(copy->GetRasterYSize(), height);
        for (int band = 1; band <= in->GetRasterCount(); band++)
        {
            std::vector<uint8_t> a(static_cast<size_t>(width) * height), b(a.size());
            ASSERT_EQ(in->GetRasterBand(band)->RasterIO(GF_Read, 0, 0, width, height, a.data(), width, height, GDT_Byte, 0, 0), CE_None);
            ASSERT_EQ(copy->GetRasterBand(band)->RasterIO(GF_Read, 0, 0, width, height, b.data(), width, height, GDT_Byte, 0, 0), CE_None);
            EXPECT_EQ(a, b) << band;
        }
    }

    TEST_F(GdalUtilToolTest, aFileThatIsNoRasterIsUnreadableToNormalizeAndWritesNothing)
    {
        const fs::path out = area_->getFolder() / "none.tif";
        Server server;
        const auto response = server.ask(normalizeRequest(32, fakeRaster_, out));
        EXPECT_FALSE(response.ok);
        EXPECT_EQ(response.error, raster::ErrorCode::Unreadable);
        EXPECT_FALSE(fs::exists(out));
    }

    TEST_F(GdalUtilToolTest, aRasterOfMoreThanABillionPixelsIsALimitThatSaysToBuildTheDatasetAndWritesNothing)
    {
        const fs::path huge = area_->getFolder() / "huge.tif";
        {
            CPLStringList options;
            options.SetNameValue("TILED", "YES");
            options.SetNameValue("SPARSE_OK", "TRUE");
            const auto ds = adoptDataset(GetGDALDriverManager()->GetDriverByName("GTiff")->Create(
                io::toUtf8(huge).c_str(), 40000, 30000, 1, GDT_Byte, options.List()));
            ASSERT_TRUE(ds);
        }
        const fs::path out = area_->getFolder() / "huge-normalized.tif";
        Server server;
        const auto response = server.ask(normalizeRequest(33, huge, out));
        EXPECT_FALSE(response.ok);
        EXPECT_EQ(response.error, raster::ErrorCode::Limit);
        EXPECT_NE(response.message.find("build the dataset first"), std::string::npos) << response.message;
        EXPECT_FALSE(fs::exists(out));
    }

    TEST_F(GdalUtilToolTest, aNormalizeRequestNeedsAnAbsoluteOutput)
    {
        Server server;
        const auto noOut = server.ask(normalizeRequest(1, jpeg_, fs::path()));
        EXPECT_FALSE(noOut.ok);
        EXPECT_EQ(noOut.error, raster::ErrorCode::Internal);
        const auto relative = server.ask(normalizeRequest(2, jpeg_, "relative.tif"));
        EXPECT_FALSE(relative.ok);
        EXPECT_EQ(relative.error, raster::ErrorCode::Internal);
        EXPECT_FALSE(fs::exists("relative.tif"));
    }

    TEST_F(GdalUtilToolTest, aThumbnailRequestNeedsAnAbsoluteOutputAndAPositiveSize)
    {
        Server server;
        const auto noOut = server.ask(thumbnailRequest(1, jpeg_, fs::path(), raster::ThumbnailParams{32, std::nullopt}));
        EXPECT_FALSE(noOut.ok);
        EXPECT_EQ(noOut.error, raster::ErrorCode::Internal);

        const auto relative = server.ask(thumbnailRequest(2, jpeg_, "relative.webp", raster::ThumbnailParams{32, std::nullopt}));
        EXPECT_FALSE(relative.ok);
        EXPECT_EQ(relative.error, raster::ErrorCode::Internal);

        const auto empty = server.ask(thumbnailRequest(3, jpeg_, area_->getFolder() / "z.webp", raster::ThumbnailParams{0, std::nullopt}));
        EXPECT_FALSE(empty.ok);
        EXPECT_EQ(empty.error, raster::ErrorCode::Internal);
        EXPECT_FALSE(fs::exists(area_->getFolder() / "z.webp"));
    }

    // ------------------------------------------------------------------ requests that fail

    TEST_F(GdalUtilToolTest, aRelativePathIsRefusedBecauseTheWorkingDirectoryIsNotTheHosts)
    {
        Server server;
        const auto response = server.ask(probeRequest(1, "ortho.tif"));
        EXPECT_FALSE(response.ok);
        EXPECT_EQ(response.error, raster::ErrorCode::Internal);
    }

    TEST_F(GdalUtilToolTest, anUnknownOperationIsAnInternalError)
    {
        Server server;
        raster::Request request = probeRequest(1, geotiff_);
        request.op = "bogus";
        const auto response = server.ask(request);
        EXPECT_FALSE(response.ok);
        EXPECT_EQ(response.error, raster::ErrorCode::Internal);
        EXPECT_NE(response.message.find("bogus"), std::string::npos) << response.message;
    }

    TEST_F(GdalUtilToolTest, badLinesAreAnsweredAndDoNotStopTheServer)
    {
        Server server;
        const auto garbage = server.ask("not json");
        EXPECT_FALSE(garbage.ok);
        EXPECT_EQ(garbage.error, raster::ErrorCode::Internal);
        EXPECT_EQ(garbage.id, std::nullopt);

        const auto noOp = server.ask(R"({"id":31,"path":"/a.tif"})");
        EXPECT_FALSE(noOp.ok);
        EXPECT_EQ(noOp.id, std::optional<int64_t>(31)) << "the answer names the request it could read the id of";

        const auto badParams =
            server.ask(R"({"id":32,"op":"probe","path":")" + io::toUtf8(geotiff_) + R"(","params":{"kind":"mesh"}})");
        EXPECT_FALSE(badParams.ok);
        EXPECT_EQ(badParams.error, raster::ErrorCode::Internal);

        // An empty line is not a request and gets no answer: the next response is the next request's
        server.send("");
        EXPECT_TRUE(server.ask(probeRequest(33, geotiff_)).ok);
    }

    TEST_F(GdalUtilToolTest, aRequestLineOverTheLimitIsAnsweredAndDropped)
    {
        Server server;
        server.send(std::string(raster::kMaxLineBytes + 1, 'x'));
        const auto response = raster::decodeResponse(server.receive());
        EXPECT_FALSE(response.ok);
        EXPECT_EQ(response.error, raster::ErrorCode::Internal);
        EXPECT_NE(response.message.find("16 MiB"), std::string::npos) << response.message;
        EXPECT_TRUE(server.ask(probeRequest(1, geotiff_)).ok);
    }

    // ------------------------------------------------------------------ the stream

    TEST_F(GdalUtilToolTest, standardOutputCarriesNothingButResponses)
    {
        // Verbose logging, which a library may print where it likes: it all goes to standard error
        process::SessionOptions options;
        options.envOverrides = {{"DDB_DEBUG", "1"}};
        Server server(options);
        server.send(raster::encodeRequest(probeRequest(1, geotiff_)));
        server.send(raster::encodeRequest(probeRequest(2, fakeRaster_)));
        server.send(raster::encodeRequest(probeRequest(3, geojson_, vectorParams())));
        for (int i = 1; i <= 3; i++)
        {
            const std::string line = server.receive();
            const auto response = raster::decodeResponse(line); // throws on anything else
            EXPECT_EQ(response.id, std::optional<int64_t>(i));
        }
    }

    TEST_F(GdalUtilToolTest, requestsSentBeforeReadingAreAnsweredInOrder)
    {
        Server server;
        for (int64_t id = 100; id < 108; id++)
            server.send(raster::encodeRequest(probeRequest(id, id % 2 == 0 ? geotiff_ : fakeRaster_)));
        for (int64_t id = 100; id < 108; id++)
        {
            const auto response = raster::decodeResponse(server.receive());
            EXPECT_EQ(response.id, std::optional<int64_t>(id));
            EXPECT_EQ(response.ok, id % 2 == 0);
        }
    }

    TEST_F(GdalUtilToolTest, theServerEndsAtTheEndOfItsInput)
    {
        Server server;
        EXPECT_TRUE(server.ask(probeRequest(1, geotiff_)).ok);
        server.session().closeStdin();
        const auto eof = server.session().readLine(patiently(), raster::kMaxLineBytes);
        EXPECT_EQ(eof.status, Session::ReadStatus::Eof);
        ASSERT_TRUE(server.session().wait(patiently()));
        EXPECT_TRUE(server.session().exitStatus().exited);
        EXPECT_EQ(server.session().exitStatus().exitCode, 0);
    }

    // ------------------------------------------------------------------ --request

    USER_OPEN_TEST_F(GdalUtilToolTest, oneRequestOnTheCommandLineGivesOneResponseLine)
    {
        const ToolRun run = runTool({"--request", raster::encodeRequest(probeRequest(41, geotiff_))});
        ASSERT_EQ(run.lines.size(), 1u) << run.stderrText;
        const auto response = raster::decodeResponse(run.lines[0]);
        EXPECT_TRUE(response.ok);
        EXPECT_EQ(response.id, std::optional<int64_t>(41));
        EXPECT_EQ(response.result, raster::toJson(raster::probe(geotiff_, raster::ProbeParams())));
        EXPECT_TRUE(run.exited);
        EXPECT_EQ(run.exitCode, 0);
    }

    TEST_F(GdalUtilToolTest, aFailedRequestOnTheCommandLineExitsWithOne)
    {
        const ToolRun run = runTool({"--request", raster::encodeRequest(probeRequest(42, fakeRaster_))});
        ASSERT_EQ(run.lines.size(), 1u);
        const auto response = raster::decodeResponse(run.lines[0]);
        EXPECT_FALSE(response.ok);
        EXPECT_EQ(response.error, raster::ErrorCode::Unreadable);
        EXPECT_EQ(run.exitCode, 1);
    }

    TEST_F(GdalUtilToolTest, aRequestOnTheCommandLineThatIsNoRequestIsAnsweredToo)
    {
        const ToolRun run = runTool({"--request", "{}"});
        ASSERT_EQ(run.lines.size(), 1u);
        const auto response = raster::decodeResponse(run.lines[0]);
        EXPECT_FALSE(response.ok);
        EXPECT_EQ(response.error, raster::ErrorCode::Internal);
        EXPECT_EQ(run.exitCode, 1);
    }

    // ------------------------------------------------------------------ the host client on the real program

    // A DXF with one point at UTM coordinates of zone 32N: GDAL's DXF driver ignores a same-name
    // .prj, DroneDB reads it, so the point is on the map with the .prj and in drawing coordinates
    // without
    fs::path writeDrawing(const fs::path &dxf, bool withPrj)
    {
        writeFile(dxf,
                  "0\nSECTION\n2\nHEADER\n0\nENDSEC\n"
                  "0\nSECTION\n2\nENTITIES\n0\nPOINT\n8\n0\n10\n500000.0\n20\n5000000.0\n30\n0.0\n0\nENDSEC\n0\nEOF\n");
        if (withPrj)
        {
            fs::path prj = dxf;
            prj.replace_extension(".prj");
            writeFile(prj, cad::prjContent("EPSG:32632"));
        }
        return dxf;
    }

    USER_OPEN_TEST_F(GdalUtilToolTest, theHostClientProbesThroughTheRealProgramWhatTheProcessFinds)
    {
        raster::GdalUtilHelper helper;

        const raster::ProbeResult raster = helper.probe(geotiff_, raster::ProbeParams());
        EXPECT_EQ(raster.type, "GeoRaster");
        EXPECT_EQ(toJson(raster), toJson(ddb::raster::probe(geotiff_, raster::ProbeParams())));

        const raster::ProbeResult vector = helper.probe(geojson_, vectorParams());
        EXPECT_EQ(vector.type, "Vector");
        EXPECT_EQ(toJson(vector), toJson(ddb::raster::probe(geojson_, vectorParams())));

        try
        {
            helper.probe(fakeRaster_, raster::ProbeParams());
            FAIL() << "no exception";
        }
        catch (const raster::RasterOpException &e)
        {
            EXPECT_EQ(e.code(), raster::ErrorCode::Unreadable);
        }
        // One helper served all three: a failure of a file is no failure of the helper
        EXPECT_EQ(helper.startedHelpers(), 1u);
        helper.shutdown();
    }

    USER_OPEN_TEST_F(GdalUtilToolTest, anEntryMadeThroughTheRealProgramHasWhatTheProcessFinds)
    {
        Entry entry;
        parseEntry(geotiff_, area_->getFolder(), entry, false);
        EXPECT_EQ(entry.type, EntryType::GeoRaster);

        Entry expected;
        raster::applyProbe(raster::probe(geotiff_, raster::ProbeParams()), expected);
        for (const auto &[key, value] : expected.properties.items())
            EXPECT_EQ(entry.properties.at(key), value) << key;
        EXPECT_EQ(entry.point_geom.toWkt(), expected.point_geom.toWkt());
        EXPECT_EQ(entry.polygon_geom.toWkt(), expected.polygon_geom.toWkt());
    }

    USER_OPEN_TEST_F(GdalUtilToolTest, aThumbnailMadeThroughTheRealProgramEqualsTheOneInTheProcess)
    {
        raster::ThumbnailParams plain{32, std::nullopt};
        const fs::path expected = area_->getFolder() / "expected.webp";
        raster::thumbnail(jpeg_, expected, plain);
        EXPECT_EQ(raster::renderThumbnailWebp(jpeg_, plain), readFile(expected));

        raster::ThumbnailParams bands{48, ThumbVisParams()};
        bands.vis->bands = "3,2,1";
        raster::thumbnail(geotiff_, expected, bands);
        EXPECT_EQ(raster::renderThumbnailWebp(geotiff_, bands), readFile(expected));
    }

    TEST_F(GdalUtilToolTest, aThumbnailOfAFileThatIsNoRasterIsAnExceptionAndNoFile)
    {
        const fs::path out = area_->getFolder() / "none.webp";
        EXPECT_THROW(generateThumb(fakeRaster_, 32, out, true, nullptr, nullptr), raster::RasterOpException);
        EXPECT_FALSE(fs::exists(out));
    }

    // The .prj the host ignores in its thread (the index re-reads a drawing right after the .prj
    // left the index, while the file is still on disk) must not count in the helper either
    TEST_F(GdalUtilToolTest, theSidecarThatTheHostDecidedIsTheOneThatGivesTheDrawingItsCoordinateSystem)
    {
        const fs::path dxf = writeDrawing(area_->getFolder() / "site.dxf", true);
        fs::path prj = dxf;
        prj.replace_extension(".prj");

        raster::GdalUtilHelper helper;
        raster::ProbeParams params = vectorParams();
        params.source = dxf;

        // Not told: the helper reads the .prj it finds
        raster::ProbeResult found = helper.probe(dxf, params);
        EXPECT_TRUE(found.properties.at("georeferenced").get<bool>());
        EXPECT_EQ(found.properties.at("vector").at("crs").at("source"), "prj");
        EXPECT_FALSE(found.polygonGeom.empty());

        // Told that none counts
        params.sidecarDecided = true;
        raster::ProbeResult ignored = helper.probe(dxf, params);
        EXPECT_FALSE(ignored.properties.at("georeferenced").get<bool>());
        EXPECT_TRUE(ignored.polygonGeom.empty());

        // Told which one counts
        params.sidecar = prj;
        EXPECT_TRUE(helper.probe(dxf, params).properties.at("georeferenced").get<bool>());
        helper.shutdown();
    }

    TEST_F(GdalUtilToolTest, aDrawingWhoseSidecarTheIndexIgnoresIsLocalThroughTheRealProgram)
    {
        const fs::path dxf = writeDrawing(area_->getFolder() / "site.dxf", true);
        fs::path prj = dxf;
        prj.replace_extension(".prj");

        Entry withPrj;
        parseEntry(dxf, area_->getFolder(), withPrj, false);
        EXPECT_TRUE(withPrj.properties.at("georeferenced").get<bool>());

        cad::IgnoredSidecarScope ignore({prj});
        Entry withoutPrj;
        parseEntry(dxf, area_->getFolder(), withoutPrj, false);
        EXPECT_FALSE(withoutPrj.properties.at("georeferenced").get<bool>());
        EXPECT_TRUE(withoutPrj.polygon_geom.empty());
    }

    // ------------------------------------------------------------------ the builds

    raster::Request buildRequest(int64_t id, const char *op, const fs::path &path, const fs::path &out, const json &params)
    {
        raster::Request request;
        request.id = id;
        request.op = op;
        request.path = path;
        request.out = out;
        request.params = params;
        return request;
    }

    // What two builds of the same raster must share: size, bands, geotransform, projection and the
    // pixels of every band (GDAL's checksum of the whole band)
    json describeRaster(const fs::path &file)
    {
        GDALDatasetH dataset = GDALOpen(io::toUtf8(file).c_str(), GA_ReadOnly);
        EXPECT_NE(dataset, nullptr) << file;
        if (dataset == nullptr)
            return json::object();
        double gt[6] = {0};
        GDALGetGeoTransform(dataset, gt);
        json bands = json::array();
        for (int b = 1; b <= GDALGetRasterCount(dataset); b++)
        {
            GDALRasterBandH band = GDALGetRasterBand(dataset, b);
            bands.push_back({{"type", GDALGetDataTypeName(GDALGetRasterDataType(band))},
                             {"checksum", GDALChecksumImage(band, 0, 0, GDALGetRasterXSize(dataset), GDALGetRasterYSize(dataset))}});
        }
        json result = {{"size", {GDALGetRasterXSize(dataset), GDALGetRasterYSize(dataset)}},
                       {"geotransform", std::vector<double>(gt, gt + 6)},
                       {"projection", GDALGetProjectionRef(dataset)},
                       {"overviews", GDALGetOverviewCount(GDALGetRasterBand(dataset, 1))},
                       {"bands", bands}};
        GDALClose(dataset);
        return result;
    }

    // The statistics sidecar of a COG without the time it was computed at
    json statsOf(const fs::path &cog)
    {
        json stats = json::parse(readFile(io::concat(cog, ".stats.json")));
        stats.erase("computedAt");
        return stats;
    }

    USER_OPEN_TEST_F(GdalUtilToolTest, aCogBuiltByTheProgramEqualsTheCogBuiltInTheProcess)
    {
        const fs::path inProcess = area_->getFolder() / "in-process.tif";
        raster::buildCogFile(geotiff_, inProcess, raster::CogParams(), raster::ProgressCallback());

        const fs::path byProgram = area_->getFolder() / "by-program.tif";
        const ToolRun run = runTool({"--request", raster::encodeRequest(buildRequest(51, raster::op::kCog, geotiff_, byProgram, json::object()))});
        ASSERT_FALSE(run.lines.empty()) << run.stderrText;
        const auto response = raster::decodeResponse(run.lines.back());
        ASSERT_TRUE(response.ok) << response.message;
        EXPECT_EQ(run.exitCode, 0);

        const json expected = describeRaster(inProcess);
        EXPECT_EQ(describeRaster(byProgram), expected);
        EXPECT_NE(expected.at("projection").get<std::string>().find("3857"), std::string::npos) << "Web Mercator";
        EXPECT_EQ(expected.at("bands").size(), 3u);
        EXPECT_EQ(statsOf(byProgram), statsOf(inProcess));
    }

    TEST_F(GdalUtilToolTest, theProgressOfACogReachesTheHostBeforeTheResponse)
    {
        const fs::path out = area_->getFolder() / "progress.tif";
        const ToolRun run = runTool({"--request", raster::encodeRequest(buildRequest(52, raster::op::kCog, geotiff_, out, json::object()))});
        ASSERT_GE(run.lines.size(), 2u) << run.stderrText;

        std::vector<double> progress;
        for (size_t i = 0; i + 1 < run.lines.size(); i++)
        {
            const auto fraction = raster::decodeProgress(run.lines[i]);
            ASSERT_TRUE(fraction.has_value()) << "only progress lines come before the response: " << run.lines[i];
            progress.push_back(*fraction);
        }
        EXPECT_TRUE(std::is_sorted(progress.begin(), progress.end()));
        EXPECT_GE(progress.front(), 0.0);
        EXPECT_DOUBLE_EQ(progress.back(), 1.0);
        EXPECT_FALSE(raster::decodeProgress(run.lines.back()).has_value());
        EXPECT_TRUE(raster::decodeResponse(run.lines.back()).ok);

        // And the host client forwards them
        raster::GdalUtilHelper helper;
        std::vector<double> forwarded;
        helper.buildCog(geotiff_, area_->getFolder() / "forwarded.tif", json::object(),
                        [&forwarded](double fraction) { forwarded.push_back(fraction); });
        ASSERT_FALSE(forwarded.empty());
        EXPECT_DOUBLE_EQ(forwarded.back(), 1.0);
    }

    // A COG of `source` with another compression than the one libddb gives it, written by GDAL's
    // COG driver at the grid of the source: an optimized COG (Web Mercator, tiles of 256 pixels,
    // overviews) that is not on the tile grid of Web Mercator when the source is not
    fs::path writeForeignCog(const fs::path &folder, const fs::path &source)
    {
        const fs::path foreign = folder / io::concat(source.stem(), "-foreign-cog.tif");
        GDALDatasetH input = GDALOpen(io::toUtf8(source).c_str(), GA_ReadOnly);
        EXPECT_NE(input, nullptr);
        char **options = CSLSetNameValue(nullptr, "COMPRESS", "DEFLATE");
        options = CSLSetNameValue(options, "BLOCKSIZE", "256");
        GDALDatasetH copy = GDALCreateCopy(GDALGetDriverByName("COG"), io::toUtf8(foreign).c_str(), input, FALSE, options, nullptr, nullptr);
        CSLDestroy(options);
        EXPECT_NE(copy, nullptr);
        GDALClose(copy);
        GDALClose(input);
        return foreign;
    }

    // The grid, the coordinate system and every band (type, nodata, colour, all of its pixels) of
    // `actual` are the ones of `expected`
    void expectSameRaster(const fs::path &actual, const fs::path &expected)
    {
        GDALDatasetH a = GDALOpen(io::toUtf8(actual).c_str(), GA_ReadOnly);
        GDALDatasetH e = GDALOpen(io::toUtf8(expected).c_str(), GA_ReadOnly);
        ASSERT_NE(a, nullptr) << actual;
        ASSERT_NE(e, nullptr) << expected;
        const int width = GDALGetRasterXSize(e);
        const int height = GDALGetRasterYSize(e);
        EXPECT_EQ(GDALGetRasterXSize(a), width);
        EXPECT_EQ(GDALGetRasterYSize(a), height);
        double gtA[6] = {0}, gtE[6] = {0};
        EXPECT_EQ(GDALGetGeoTransform(a, gtA), CE_None);
        EXPECT_EQ(GDALGetGeoTransform(e, gtE), CE_None);
        EXPECT_EQ(std::vector<double>(gtA, gtA + 6), std::vector<double>(gtE, gtE + 6));
        EXPECT_TRUE(OSRIsSame(GDALGetSpatialRef(a), GDALGetSpatialRef(e)));
        ASSERT_EQ(GDALGetRasterCount(a), GDALGetRasterCount(e));
        if (GDALGetRasterXSize(a) == width && GDALGetRasterYSize(a) == height)
            for (int b = 1; b <= GDALGetRasterCount(e); b++)
            {
                SCOPED_TRACE(b);
                GDALRasterBandH bandA = GDALGetRasterBand(a, b);
                GDALRasterBandH bandE = GDALGetRasterBand(e, b);
                EXPECT_EQ(GDALGetRasterDataType(bandA), GDALGetRasterDataType(bandE));
                EXPECT_EQ(GDALGetRasterColorInterpretation(bandA), GDALGetRasterColorInterpretation(bandE));
                int hasA = 0, hasE = 0;
                const double nodataA = GDALGetRasterNoDataValue(bandA, &hasA);
                const double nodataE = GDALGetRasterNoDataValue(bandE, &hasE);
                EXPECT_EQ(hasA, hasE);
                if (hasA && hasE)
                {
                    EXPECT_EQ(nodataA, nodataE);
                }
                std::vector<double> pixelsA(static_cast<size_t>(width) * height);
                std::vector<double> pixelsE(pixelsA.size());
                ASSERT_EQ(GDALRasterIO(bandA, GF_Read, 0, 0, width, height, pixelsA.data(), width, height, GDT_Float64, 0, 0), CE_None);
                ASSERT_EQ(GDALRasterIO(bandE, GF_Read, 0, 0, width, height, pixelsE.data(), width, height, GDT_Float64, 0, 0), CE_None);
                EXPECT_TRUE(pixelsA == pixelsE) << "every pixel of the band is the one of the input";
            }
        GDALClose(a);
        GDALClose(e);
    }

    // The compression GDAL reports for a raster (LZW, DEFLATE, JPEG, YCbCr JPEG...)
    std::string compressionOf(const fs::path &file)
    {
        GDALDatasetH dataset = GDALOpen(io::toUtf8(file).c_str(), GA_ReadOnly);
        EXPECT_NE(dataset, nullptr) << file;
        if (dataset == nullptr)
            return std::string();
        const char *compression = GDALGetMetadataItem(dataset, "COMPRESSION", "IMAGE_STRUCTURE");
        std::string result = compression ? compression : "";
        GDALClose(dataset);
        return result;
    }

    // Builds the COG of `input` with the program and checks it is an optimized COG at the grid of
    // the input with all of its pixels, written again by GDAL with a lossless compression
    void expectEncodedAgainAtItsOwnGrid(const fs::path &input, const fs::path &out)
    {
        ASSERT_TRUE(raster::probe(input, raster::ProbeParams()).isCog) << "the input is a COG already";

        raster::GdalUtilHelper helper;
        helper.buildCog(input, out, json::object(), raster::ProgressCallback());

        EXPECT_NE(readFile(out), readFile(input)) << "a file of a user is never copied";
        EXPECT_TRUE(raster::probe(out, raster::ProbeParams()).isCog);
        // Not resampled to the tile grid: the same pixels at the same place
        expectSameRaster(out, input);
        EXPECT_EQ(compressionOf(out).find("JPEG"), std::string::npos) << "a lossless input stays lossless";
        EXPECT_TRUE(fs::exists(io::concat(out, ".stats.json")));
    }

    USER_OPEN_TEST_F(GdalUtilToolTest, aCogIsEncodedAgainAndNeverCopied)
    {
        testutils::GeoRasterSpec spec;
        spec.width = 600;
        spec.height = 600;
        spec.bands = 1;
        spec.type = GDT_UInt16;
        spec.epsg = 3857;
        spec.originX = 1000000.0;
        spec.originY = 5000000.0;
        spec.pixelSize = 50.0;
        spec.value = [](int, int x, int y) { return static_cast<double>((x * 7 + y * 13) % 4000); };
        const fs::path source = testutils::createGeoRaster(area_->getFolder() / "uint16.tif", spec);
        const fs::path foreign = writeForeignCog(area_->getFolder(), source);

        expectEncodedAgainAtItsOwnGrid(foreign, area_->getFolder() / "again.tif");
    }

    USER_OPEN_TEST_F(GdalUtilToolTest, anRgbCogWithoutLossIsEncodedAgainWithoutLoss)
    {
        // Eight-bit RGB, which libddb compresses with JPEG when it resamples a raster
        testutils::GeoRasterSpec spec;
        spec.width = 700;
        spec.height = 500;
        spec.bands = 3;
        spec.type = GDT_Byte;
        spec.epsg = 3857;
        spec.originX = -10240777.0;
        spec.originY = 5916486.0;
        spec.pixelSize = 0.0731;
        spec.value = [](int band, int x, int y) { return static_cast<double>((x * band + y * 3) % 256); };
        const fs::path source = testutils::createGeoRaster(area_->getFolder() / "rgb.tif", spec);
        const fs::path foreign = writeForeignCog(area_->getFolder(), source);

        expectEncodedAgainAtItsOwnGrid(foreign, area_->getFolder() / "rgb-again.tif");
    }

    TEST_F(GdalUtilToolTest, aCogOfARasterWithoutProjectionIsUnreadable)
    {
        const fs::path noProjection = area_->getFolder() / "plain.tif";
        {
            GDALDatasetH dataset = GDALCreate(GDALGetDriverByName("GTiff"), io::toUtf8(noProjection).c_str(), 16, 16, 1, GDT_Byte, nullptr);
            ASSERT_NE(dataset, nullptr);
            GDALClose(dataset);
        }
        const ToolRun run = runTool({"--request", raster::encodeRequest(buildRequest(53, raster::op::kCog, noProjection,
                                                                                    area_->getFolder() / "none.tif", json::object()))});
        ASSERT_FALSE(run.lines.empty());
        const auto response = raster::decodeResponse(run.lines.back());
        EXPECT_FALSE(response.ok);
        EXPECT_EQ(response.error, raster::ErrorCode::Unreadable);
        EXPECT_EQ(run.exitCode, 1);
    }

    TEST_F(GdalUtilToolTest, theLongBuildsRunOnlyOneShot)
    {
        Server server;
        for (const char *op : {"cog", "vector"})
        {
            SCOPED_TRACE(op);
            const auto response = server.ask(buildRequest(1, op, geotiff_, area_->getFolder() / "never", json::object()));
            EXPECT_FALSE(response.ok);
            EXPECT_EQ(response.error, raster::ErrorCode::Internal);
            EXPECT_NE(response.message.find("--request"), std::string::npos) << response.message;
        }
        EXPECT_FALSE(fs::exists(area_->getFolder() / "never"));
    }

    // The files of a folder with their sizes, relative to it
    std::map<std::string, std::uintmax_t> filesOf(const fs::path &folder)
    {
        std::map<std::string, std::uintmax_t> files;
        for (const auto &entry : fs::recursive_directory_iterator(folder))
            if (entry.is_regular_file())
                files[io::toUtf8(entry.path().lexically_relative(folder))] = entry.file_size();
        return files;
    }

    USER_OPEN_TEST_F(GdalUtilToolTest, theVectorOutputsOfTheProgramAreTheOnesBuiltInTheProcess)
    {
        const fs::path inProcess = area_->getFolder() / "stage-process";
        const fs::path byProgram = area_->getFolder() / "stage-program";
        fs::create_directories(inProcess);
        fs::create_directories(byProgram);
        raster::buildVectorFiles(geojson_, inProcess, raster::VectorParams());

        const ToolRun run = runTool({"--request", raster::encodeRequest(buildRequest(54, raster::op::kVector, geojson_, byProgram, json::object()))});
        ASSERT_FALSE(run.lines.empty()) << run.stderrText;
        ASSERT_TRUE(raster::decodeResponse(run.lines.back()).ok) << run.lines.back();

        const auto expected = filesOf(inProcess);
        EXPECT_TRUE(expected.count("vec/source.gpkg"));
        EXPECT_TRUE(expected.count("mvt/metadata.json"));
        // The tiles are the same files; the GeoPackage has the same layers and features
        const auto produced = filesOf(byProgram);
        for (const auto &[name, size] : expected)
        {
            if (name != "vec/source.gpkg")
            {
                EXPECT_EQ(produced.at(name), size) << name;
            }
        }
        EXPECT_EQ(produced.size(), expected.size());

        for (const fs::path &stage : {inProcess, byProgram})
        {
            GDALDatasetH gpkg = GDALOpenEx(io::toUtf8(stage / "vec" / "source.gpkg").c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
            ASSERT_NE(gpkg, nullptr);
            ASSERT_EQ(GDALDatasetGetLayerCount(gpkg), 1);
            EXPECT_EQ(OGR_L_GetFeatureCount(GDALDatasetGetLayer(gpkg, 0), TRUE), 2);
            GDALClose(gpkg);
        }
    }

    TEST_F(GdalUtilToolTest, aVectorFileThatIsNoVectorIsUnreadableAndLeavesNothing)
    {
        const fs::path stage = area_->getFolder() / "stage-none";
        fs::create_directories(stage);
        writeFile(area_->getFolder() / "bad.geojson", "this is not a vector");
        const ToolRun bad = runTool({"--request", raster::encodeRequest(buildRequest(56, raster::op::kVector, area_->getFolder() / "bad.geojson", stage, json::object()))});
        ASSERT_FALSE(bad.lines.empty());
        const auto response = raster::decodeResponse(bad.lines.back());
        EXPECT_FALSE(response.ok);
        EXPECT_EQ(response.error, raster::ErrorCode::Unreadable);
        EXPECT_TRUE(filesOf(stage).empty()) << "nothing is left to publish";
    }

    raster::GeoprojectParams photoParams()
    {
        raster::GeoprojectParams params;
        params.width = 96;
        params.height = 64;
        params.footprint = {{{9.100, 45.300}, {9.100, 45.299}, {9.102, 45.299}, {9.102, 45.300}}};
        params.outsize = "100%";
        return params;
    }

    USER_OPEN_TEST_F(GdalUtilToolTest, aPhotoIsGeoprojectedByTheProgramLikeInTheProcess)
    {
        const fs::path inProcess = area_->getFolder() / "projected-process.tif";
        raster::geoprojectImage(jpeg_, inProcess, photoParams());

        const fs::path byProgram = area_->getFolder() / "projected-program.tif";
        Server server;
        const auto response = server.ask(buildRequest(57, raster::op::kGeoproject, jpeg_, byProgram, raster::toJson(photoParams())));
        ASSERT_TRUE(response.ok) << response.message;

        const json expected = describeRaster(inProcess);
        EXPECT_EQ(describeRaster(byProgram), expected);
        EXPECT_EQ(expected.at("bands").size(), 4u) << "RGB and alpha";
        EXPECT_NE(expected.at("projection").get<std::string>().find("4326"), std::string::npos);
    }

    TEST_F(GdalUtilToolTest, aGeoprojectRequestWithoutTheFootprintOfThePhotoIsRefused)
    {
        Server server;
        const auto response = server.ask(buildRequest(58, raster::op::kGeoproject, jpeg_, area_->getFolder() / "none.tif",
                                                      json{{"width", 96}, {"height", 64}}));
        EXPECT_FALSE(response.ok);
        EXPECT_EQ(response.error, raster::ErrorCode::Internal);
        EXPECT_FALSE(fs::exists(area_->getFolder() / "none.tif"));
    }
}
