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
#include <memory>
#include <regex>
#include <string>
#include <vector>

#include "contract_fixtures.h"
#include "fs.h"
#include "gdal_inc.h"
#include "gtest/gtest.h"
#include "json.h"
#include "json_schema_check.h"
#include "mio.h"
#include "process_runner.h"
#include "raster_ops.h"
#include "raster_protocol.h"
#include "raster_test_utils.h"
#include "testarea.h"
#include "tool_locator.h"

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

    // A JPEG of 96 x 64 pixels, written with GDAL's JPEG driver
    fs::path writeJpeg(const fs::path &path)
    {
        GDALDriverH memory = GDALGetDriverByName("MEM");
        GDALDriverH jpeg = GDALGetDriverByName("JPEG");
        EXPECT_NE(memory, nullptr);
        EXPECT_NE(jpeg, nullptr);
        GDALDatasetH source = GDALCreate(memory, "", 96, 64, 3, GDT_Byte, nullptr);
        std::vector<uint8_t> row(96);
        for (int band = 1; band <= 3; band++)
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

    TEST(GdalUtilTool, theSandboxReportIsOneLine)
    {
        const ToolRun run = runTool({"--sandbox-report"});
        ASSERT_EQ(run.lines.size(), 1u);
        EXPECT_EQ(run.lines[0].rfind("sandbox:", 0), 0u) << run.lines[0];
        EXPECT_EQ(run.exitCode, 0);
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
    TEST_F(GdalUtilToolTest, theProbeOfTheHelperEqualsTheProbeInTheProcess)
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

    TEST_F(GdalUtilToolTest, aJpegThumbnailIsAValidWebpFileEqualToTheOneInTheProcess)
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

    TEST_F(GdalUtilToolTest, aThumbnailWithVisualisationParametersEqualsTheOneInTheProcess)
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

    TEST_F(GdalUtilToolTest, aThumbnailOfAFileThatIsNoRasterIsUnreadableAndWritesNothing)
    {
        const fs::path out = area_->getFolder() / "none.webp";
        Server server;
        const auto response = server.ask(thumbnailRequest(23, fakeRaster_, out, raster::ThumbnailParams{32, std::nullopt}));
        EXPECT_FALSE(response.ok);
        EXPECT_EQ(response.error, raster::ErrorCode::Unreadable);
        EXPECT_FALSE(fs::exists(out));
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

    TEST_F(GdalUtilToolTest, anUnknownOperationAndOnesNotYetAvailableAreInternalErrors)
    {
        Server server;
        for (const char *op : {"bogus", "cog", "vector", "normalize", "geoproject"})
        {
            SCOPED_TRACE(op);
            raster::Request request = probeRequest(1, geotiff_);
            request.op = op;
            const auto response = server.ask(request);
            EXPECT_FALSE(response.ok);
            EXPECT_EQ(response.error, raster::ErrorCode::Internal);
            EXPECT_NE(response.message.find(op), std::string::npos) << response.message;
        }
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

    TEST_F(GdalUtilToolTest, oneRequestOnTheCommandLineGivesOneResponseLine)
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
}
