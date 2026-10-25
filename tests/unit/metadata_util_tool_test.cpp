/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The metadata-util program itself (vendor/metadata-util/PROTOCOL.md), driven through process::Session the
// way a host drives it and without the client of libddb: the version line, a bad or unknown or
// oversized request that must not stop the server, what stdout carries, the answer to a missing
// file or a file that is not an image, the limits on a tag, pipelined requests, end of input, and
// the dump command against the answers of --serve. The program is the one built next to the test
// binary. Waits are bounded deadlines, never sleeps used for synchronization.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <regex>
#include <string>
#include <vector>

#include "exif_helper_pool.h"
#include "fs.h"
#include "gtest/gtest.h"
#include "json.h"
#include "mio.h"
#include "process_runner.h"
#include "testarea.h"
#include "tool_locator.h"

namespace
{
    using namespace ddb;
    using process::Session;
    using std::chrono::milliseconds;
    using std::chrono::seconds;

    // How long a step that must succeed may take before the test calls it hung
    constexpr seconds kPatience(30);
    constexpr size_t kMaxLine = 64 * 1024 * 1024;
    constexpr size_t kRequestLimit = 16 * 1024 * 1024;
    constexpr size_t kTagLimit = 65536;

    Session::Clock::time_point patiently() { return Session::Clock::now() + kPatience; }

    // A TIFF of 4 x 4 pixels: a header and an IFD of the two size tags (little endian, 38 bytes)
    std::string tinyTiff()
    {
        const unsigned char bytes[] = {'I', 'I', 42, 0, 8, 0, 0, 0,  // header, IFD at 8
                                       2, 0,                         // two entries
                                       0, 1, 3, 0, 1, 0, 0, 0, 4, 0, 0, 0, // ImageWidth, Short, 1, 4
                                       1, 1, 3, 0, 1, 0, 0, 0, 4, 0, 0, 0, // ImageLength, Short, 1, 4
                                       0, 0, 0, 0};                  // no next IFD
        return std::string(reinterpret_cast<const char *>(bytes), sizeof(bytes));
    }

    fs::path exifTool()
    {
        return tools::findTool(exif::kMetadataUtilName, exif::kMetadataUtilEnv);
    }

    void writeFile(const fs::path &file, const std::string &content)
    {
        std::ofstream out(file, std::ios::binary);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
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
        if (!session.start(exifTool(), args))
        {
            ADD_FAILURE() << session.startError();
            return run;
        }
        session.closeStdin();
        for (;;)
        {
            const auto line = session.readLine(patiently(), kMaxLine);
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
        Server()
        {
            started_ = session_.start(exifTool(), {"--serve"});
            EXPECT_TRUE(started_) << session_.startError();
        }

        bool started() const { return started_; }

        void send(const std::string &line)
        {
            ASSERT_EQ(session_.writeLine(line, patiently()), Session::WriteStatus::Ok);
        }

        // The next response line; the test fails when there is none
        std::string receive()
        {
            const auto reply = session_.readLine(patiently(), kMaxLine);
            EXPECT_EQ(reply.status, Session::ReadStatus::Line);
            return reply.line;
        }

        std::string ask(const std::string &line)
        {
            send(line);
            return receive();
        }

        nlohmann::json askJson(const std::string &line)
        {
            const std::string reply = ask(line);
            const auto parsed = nlohmann::json::parse(reply, nullptr, false);
            EXPECT_TRUE(parsed.is_object()) << "not a JSON object: " << reply.substr(0, 200);
            return parsed;
        }

        Session &session() { return session_; }

      private:
        Session session_;
        bool started_ = false;
    };

    std::string readRequest(int64_t id, const fs::path &path)
    {
        return nlohmann::json{{"id", id}, {"op", "read"}, {"path", io::toUtf8(path)}}.dump();
    }

    std::string writeRequest(int64_t id, const fs::path &path, const std::string &key, const std::string &value)
    {
        return nlohmann::json{{"id", id},
                              {"op", "write"},
                              {"path", io::toUtf8(path)},
                              {"set", nlohmann::json::array({{{"key", key}, {"value", value}}})}}
            .dump();
    }

    // A folder with a TIFF (size tags only), a text file and a folder, and the program located
    class MetadataUtilToolTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            ASSERT_FALSE(exifTool().empty()) << "metadata-util is built next to the test binary";
            area_ = std::make_unique<TestArea>(std::string("ExifTool.") +
                                                   ::testing::UnitTest::GetInstance()->current_test_info()->name(),
                                               true);
            tiff_ = area_->getFolder() / "image.tif";
            writeFile(tiff_, tinyTiff());
            text_ = area_->getFolder() / "text.jpg";
            writeFile(text_, "this is not an image");
            missing_ = area_->getFolder() / "missing.jpg";
        }

        std::unique_ptr<TestArea> area_;
        fs::path tiff_;
        fs::path text_;
        fs::path missing_;
    };

    // ------------------------------------------------------------------ --version

    TEST(MetadataUtilTool, versionPrintsOneLineAndExitsZero)
    {
        ASSERT_FALSE(exifTool().empty()) << "metadata-util is built next to the test binary";
        const ToolRun run = runTool({"--version"});
        ASSERT_EQ(run.lines.size(), 1u);
        EXPECT_TRUE(std::regex_match(run.lines[0],
                                     std::regex(R"(metadata-util \S+ \(Exiv2 [0-9]+\.[0-9]+\.[0-9]+\) protocol [0-9]+\.[0-9]+)")))
            << run.lines[0];
        EXPECT_TRUE(run.exited);
        EXPECT_EQ(run.exitCode, 0);
        EXPECT_TRUE(run.stderrText.empty()) << run.stderrText;
    }

    // ------------------------------------------------------------------ --serve

    TEST_F(MetadataUtilToolTest, aLineThatIsNotARequestIsAnsweredAndTheNextOneIsServed)
    {
        Server server;
        ASSERT_TRUE(server.started());

        const auto bad = server.askJson("this is not json");
        EXPECT_FALSE(bad.value("ok", true));
        EXPECT_EQ(bad.value("error", ""), "request");
        EXPECT_TRUE(bad.contains("id"));
        EXPECT_TRUE(bad["id"].is_null()) << "no id can be told";
        EXPECT_FALSE(bad.value("message", "").empty());

        const auto good = server.askJson(readRequest(3, tiff_));
        EXPECT_TRUE(good.value("ok", false));
        EXPECT_EQ(good.value("id", int64_t(-1)), 3);
    }

    TEST_F(MetadataUtilToolTest, aRequestOfTheWrongShapeIsARequestError)
    {
        Server server;
        ASSERT_TRUE(server.started());
        for (const char *line : {R"({"id":1,"op":"read"})",             // no path
                                 R"({"op":"read","path":"x"})",         // no id
                                 R"({"id":"one","op":"read","path":"x"})", // id is not an integer
                                 R"([1,2,3])",
                                 R"({"id":1,"op":"write","path":"x","set":5})"})
        {
            const auto reply = server.askJson(line);
            EXPECT_FALSE(reply.value("ok", true)) << line;
            EXPECT_EQ(reply.value("error", ""), "request") << line;
        }
        EXPECT_TRUE(server.askJson(readRequest(9, tiff_)).value("ok", false)) << "still served";
    }

    TEST_F(MetadataUtilToolTest, anUnknownOperationIsARequestErrorThatKeepsTheId)
    {
        Server server;
        ASSERT_TRUE(server.started());
        const auto reply = server.askJson(R"({"id":41,"op":"frobnicate","path":"x"})");
        EXPECT_FALSE(reply.value("ok", true));
        EXPECT_EQ(reply.value("error", ""), "request");
        EXPECT_EQ(reply.value("id", int64_t(-1)), 41);
        EXPECT_TRUE(server.askJson(readRequest(42, tiff_)).value("ok", false)) << "still served";
    }

    TEST_F(MetadataUtilToolTest, aFileThatIsNotThereOrIsNotAFileIsAnOpenError)
    {
        Server server;
        ASSERT_TRUE(server.started());
        for (const fs::path &path : {missing_, area_->getFolder()})
        {
            const auto reply = server.askJson(readRequest(5, path));
            EXPECT_FALSE(reply.value("ok", true)) << path;
            EXPECT_EQ(reply.value("error", ""), "open") << path;
            EXPECT_EQ(reply.value("id", int64_t(-1)), 5);
            EXPECT_FALSE(reply.value("message", "").empty());
        }
    }

    TEST_F(MetadataUtilToolTest, aFileThatIsNotAnImageIsAnExiv2Error)
    {
        Server server;
        ASSERT_TRUE(server.started());
        const auto reply = server.askJson(readRequest(6, text_));
        EXPECT_FALSE(reply.value("ok", true));
        EXPECT_EQ(reply.value("error", ""), "exiv2");
        EXPECT_EQ(reply.value("id", int64_t(-1)), 6);
        EXPECT_FALSE(reply.value("message", "").empty());
        EXPECT_TRUE(server.askJson(readRequest(7, tiff_)).value("ok", false)) << "still served";
    }

    TEST_F(MetadataUtilToolTest, standardOutputCarriesNothingButResponseLines)
    {
        Server server;
        ASSERT_TRUE(server.started());
        // Failures of every kind, all on the same server: one object per request and no more lines
        const std::vector<std::string> requests = {readRequest(1, tiff_),
                                                   readRequest(2, text_),
                                                   readRequest(3, missing_),
                                                   "garbage",
                                                   R"({"id":4,"op":"nope"})",
                                                   writeRequest(5, tiff_, "Exif.Image.Make", "Acme"),
                                                   writeRequest(6, tiff_, "Not.A.Key", "x"),
                                                   readRequest(7, tiff_)};
        for (const auto &request : requests)
            server.send(request);
        for (size_t i = 0; i < requests.size(); i++)
        {
            const std::string line = server.receive();
            const auto parsed = nlohmann::json::parse(line, nullptr, false);
            EXPECT_TRUE(parsed.is_object() && parsed.contains("ok")) << "response " << i << ": " << line.substr(0, 200);
        }
        // Nothing more is pending: a deadline in the past finds no line
        EXPECT_EQ(server.session().readLine(Session::Clock::now() - seconds(1), kMaxLine).status,
                  Session::ReadStatus::Timeout);
    }

    TEST_F(MetadataUtilToolTest, pipelinedRequestsAreAnsweredInOrder)
    {
        Server server;
        ASSERT_TRUE(server.started());
        constexpr int count = 20;
        std::string batch;
        for (int i = 0; i < count; i++)
        {
            // Every third request fails, so the order does not depend on the kind of answer
            const fs::path &path = i % 3 == 2 ? missing_ : tiff_;
            batch += readRequest(1000 + i, path) + "\n";
        }
        ASSERT_EQ(server.session().write(batch, patiently()), Session::WriteStatus::Ok);
        for (int i = 0; i < count; i++)
        {
            const auto reply = nlohmann::json::parse(server.receive(), nullptr, false);
            ASSERT_TRUE(reply.is_object());
            EXPECT_EQ(reply.value("id", int64_t(-1)), 1000 + i);
            EXPECT_EQ(reply.value("ok", false), i % 3 != 2) << i;
        }
    }

    TEST_F(MetadataUtilToolTest, aRequestLineOverSixteenMebibytesIsRejectedAndTheNextOneIsServed)
    {
        Server server;
        ASSERT_TRUE(server.started());
        // Valid JSON in every way but its length
        std::string big = R"({"id":77,"op":"read","path":")";
        big.append(kRequestLimit + 1024, 'a');
        big += "\"}";
        ASSERT_GT(big.size(), kRequestLimit);
        const auto reply = server.askJson(big);
        EXPECT_FALSE(reply.value("ok", true));
        EXPECT_EQ(reply.value("error", ""), "request");

        // The next line is not taken for the tail of the long one
        const auto next = server.askJson(readRequest(78, tiff_));
        EXPECT_TRUE(next.value("ok", false));
        EXPECT_EQ(next.value("id", int64_t(-1)), 78);
    }

    TEST_F(MetadataUtilToolTest, endOfInputEndsTheServerWithinOneSecond)
    {
        Server server;
        ASSERT_TRUE(server.started());
        EXPECT_TRUE(server.askJson(readRequest(1, tiff_)).value("ok", false));
        server.session().closeStdin();
        EXPECT_TRUE(server.session().wait(Session::Clock::now() + seconds(1)));
        EXPECT_TRUE(server.session().exitStatus().exited);
        EXPECT_EQ(server.session().exitStatus().exitCode, 0);
        const auto rest = server.session().readLine(patiently(), kMaxLine);
        EXPECT_EQ(rest.status, Session::ReadStatus::Eof);
        EXPECT_TRUE(rest.line.empty()) << "no output after the last response";
    }

    TEST_F(MetadataUtilToolTest, endOfInputBeforeAnyRequestEndsTheServerToo)
    {
        Server server;
        ASSERT_TRUE(server.started());
        server.session().closeStdin();
        EXPECT_TRUE(server.session().wait(Session::Clock::now() + seconds(1)));
        EXPECT_EQ(server.session().exitStatus().exitCode, 0);
    }

    // ------------------------------------------------------------------ tags

    TEST_F(MetadataUtilToolTest, aTagHasAtLeastThreeComponentValuesWithNullsWhereThereIsNoComponent)
    {
        Server server;
        ASSERT_TRUE(server.started());
        const auto written = server.askJson(writeRequest(1, tiff_, "Exif.Image.Make", "A"));
        ASSERT_TRUE(written.value("ok", false)) << written.dump().substr(0, 300);

        const auto reply = server.askJson(readRequest(2, tiff_));
        ASSERT_TRUE(reply.value("ok", false));
        EXPECT_EQ(reply.value("width", 0), 4);
        EXPECT_EQ(reply.value("height", 0), 4);
        ASSERT_TRUE(reply["exif"].is_array());
        ASSERT_TRUE(reply["xmp"].is_array());

        bool sawWidth = false;
        bool sawMake = false;
        for (const auto &tag : reply["exif"])
        {
            ASSERT_TRUE(tag.contains("k") && tag.contains("t") && tag.contains("n")) << tag.dump();
            ASSERT_TRUE(tag.contains("v")) << tag.dump();
            const auto &v = tag["v"];
            const size_t n = tag["n"].get<size_t>();
            EXPECT_GE(v.size(), 3u) << tag["k"];
            EXPECT_LE(v.size(), 64u) << tag["k"];
            EXPECT_EQ(v.size(), std::max<size_t>(std::min<size_t>(n, 64), 3)) << tag["k"];
            for (size_t i = 0; i < v.size(); i++)
            {
                if (i < n)
                {
                    ASSERT_TRUE(v[i].is_array() && v[i].size() == 4) << tag["k"] << "[" << i << "]";
                    EXPECT_TRUE(v[i][0].is_number_integer());
                    EXPECT_TRUE(v[i][1].is_number() || v[i][1].is_string());
                    ASSERT_TRUE(v[i][2].is_array() && v[i][2].size() == 2);
                    EXPECT_TRUE(v[i][3].is_boolean());
                }
                else
                    EXPECT_TRUE(v[i].is_null()) << tag["k"] << "[" << i << "]";
            }
            if (tag["k"] == "Exif.Image.ImageWidth")
            {
                sawWidth = true;
                EXPECT_EQ(tag["n"], 1);
                EXPECT_EQ(tag["s"], "4");
                EXPECT_EQ(v[0][0], 4);
                EXPECT_TRUE(v[1].is_null());
                EXPECT_TRUE(v[2].is_null());
            }
            if (tag["k"] == "Exif.Image.Make")
            {
                sawMake = true;
                EXPECT_EQ(tag["t"], "Ascii");
                EXPECT_EQ(tag["n"], 2) << "the terminator counts";
                EXPECT_EQ(tag["s"], "A");
                EXPECT_EQ(v[0][0], 65);
                EXPECT_TRUE(v[2].is_null());
            }
        }
        EXPECT_TRUE(sawWidth);
        EXPECT_TRUE(sawMake);
    }

    TEST_F(MetadataUtilToolTest, aTagValueOverSixtyFiveThousandBytesIsTruncatedWithoutTextOrValues)
    {
        Server server;
        ASSERT_TRUE(server.started());

        // Exactly the limit is kept; one byte more is cut
        const auto atLimit = server.askJson(writeRequest(1, tiff_, "Exif.Image.ImageDescription", std::string(kTagLimit, 'x')));
        ASSERT_TRUE(atLimit.value("ok", false)) << atLimit.dump().substr(0, 300);
        auto reply = server.askJson(readRequest(2, tiff_));
        ASSERT_TRUE(reply.value("ok", false));
        const nlohmann::json *description = nullptr;
        for (const auto &tag : reply["exif"])
            if (tag["k"] == "Exif.Image.ImageDescription")
                description = &tag;
        ASSERT_NE(description, nullptr);
        EXPECT_FALSE(description->contains("trunc"));
        ASSERT_TRUE(description->contains("s"));
        EXPECT_EQ((*description)["s"].get<std::string>().size(), kTagLimit);
        EXPECT_TRUE(description->contains("v"));

        const auto over = server.askJson(writeRequest(3, tiff_, "Exif.Image.ImageDescription", std::string(kTagLimit + 1, 'x')));
        ASSERT_TRUE(over.value("ok", false)) << over.dump().substr(0, 300);
        ASSERT_EQ(over["tags"].size(), 1u);
        EXPECT_TRUE(over["tags"][0].value("trunc", false)) << "a write answer follows the same rule";
        EXPECT_FALSE(over["tags"][0].contains("s"));

        reply = server.askJson(readRequest(4, tiff_));
        ASSERT_TRUE(reply.value("ok", false));
        description = nullptr;
        for (const auto &tag : reply["exif"])
            if (tag["k"] == "Exif.Image.ImageDescription")
                description = &tag;
        ASSERT_NE(description, nullptr);
        EXPECT_TRUE(description->value("trunc", false));
        EXPECT_FALSE(description->contains("s"));
        EXPECT_FALSE(description->contains("s64"));
        EXPECT_FALSE(description->contains("v"));
        EXPECT_EQ((*description)["t"], "Ascii");
        EXPECT_GT((*description)["n"].get<size_t>(), kTagLimit);
    }

    TEST_F(MetadataUtilToolTest, aWriteIsKeptByTheFileAndAFailedWriteIsAnError)
    {
        Server server;
        ASSERT_TRUE(server.started());
        const auto ok = server.askJson(writeRequest(1, tiff_, "Exif.Image.Model", "Model X"));
        ASSERT_TRUE(ok.value("ok", false));
        ASSERT_EQ(ok["tags"].size(), 1u);
        EXPECT_EQ(ok["tags"][0]["k"], "Exif.Image.Model");
        EXPECT_EQ(ok["tags"][0]["s"], "Model X");
        EXPECT_FALSE(ok["tags"][0].contains("v"));

        const auto bad = server.askJson(writeRequest(2, tiff_, "Not.A.Key", "x"));
        EXPECT_FALSE(bad.value("ok", true));
        EXPECT_EQ(bad.value("error", ""), "exiv2");

        const auto gone = server.askJson(writeRequest(3, missing_, "Exif.Image.Model", "x"));
        EXPECT_FALSE(gone.value("ok", true));
        EXPECT_FALSE(fs::exists(missing_)) << "a write creates no file";

        // A read in another run of the program sees the value
        const ToolRun dump = runTool({"dump", io::toUtf8(tiff_)});
        ASSERT_EQ(dump.lines.size(), 1u);
        EXPECT_NE(dump.lines[0].find("Model X"), std::string::npos) << dump.lines[0].substr(0, 300);
    }

    // ------------------------------------------------------------------ dump

    TEST_F(MetadataUtilToolTest, dumpPrintsTheServeAnswersInArgumentOrderAndExitsZero)
    {
        const fs::path second = area_->getFolder() / "second.tif";
        writeFile(second, tinyTiff());
        {
            Server setup;
            ASSERT_TRUE(setup.started());
            ASSERT_TRUE(setup.askJson(writeRequest(1, second, "Exif.Image.Make", "Second")).value("ok", false));
        }

        const ToolRun dump = runTool({"dump", io::toUtf8(tiff_), io::toUtf8(second)});
        EXPECT_TRUE(dump.exited);
        EXPECT_EQ(dump.exitCode, 0);
        ASSERT_EQ(dump.lines.size(), 2u);

        Server server;
        ASSERT_TRUE(server.started());
        EXPECT_EQ(dump.lines[0], server.ask(readRequest(0, tiff_)));
        EXPECT_EQ(dump.lines[1], server.ask(readRequest(1, second)));
        EXPECT_NE(dump.lines[0], dump.lines[1]);
    }

    TEST_F(MetadataUtilToolTest, dumpOfAMissingFilePrintsAFailureLineInItsPlaceAndExitsOne)
    {
        const ToolRun dump = runTool({"dump", io::toUtf8(tiff_), io::toUtf8(missing_), io::toUtf8(text_), io::toUtf8(tiff_)});
        EXPECT_TRUE(dump.exited);
        EXPECT_EQ(dump.exitCode, 1);
        ASSERT_EQ(dump.lines.size(), 4u) << "a failed file still prints a line";

        std::vector<nlohmann::json> lines;
        for (const auto &line : dump.lines)
            lines.push_back(nlohmann::json::parse(line, nullptr, false));
        for (size_t i = 0; i < lines.size(); i++)
        {
            ASSERT_TRUE(lines[i].is_object()) << i;
            EXPECT_EQ(lines[i].value("id", int64_t(-1)), static_cast<int64_t>(i));
        }
        EXPECT_TRUE(lines[0].value("ok", false));
        EXPECT_FALSE(lines[1].value("ok", true));
        EXPECT_EQ(lines[1].value("error", ""), "open");
        EXPECT_FALSE(lines[2].value("ok", true));
        EXPECT_EQ(lines[2].value("error", ""), "exiv2");
        EXPECT_TRUE(lines[3].value("ok", false)) << "the files after a failure are still read";
    }

} // namespace
