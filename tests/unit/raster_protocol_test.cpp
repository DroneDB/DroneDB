/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The messages of libddb and the raster helper (apps/gdal-util/PROTOCOL.md): the request and
// response lines, file names that are not UTF-8, lines that are not what the protocol says, the
// version line, and the params and results of the probe and thumbnail operations. Canned lines,
// no process and no file.

#include <string>

#include "../../vendor/base64/base64.h"
#include "gtest/gtest.h"
#include "json.h"
#include "mio.h"
#include "raster_protocol.h"

namespace
{
    using namespace ddb;
    using raster::ErrorCode;
    using raster::ProtocolError;
    using raster::Request;
    using raster::Response;

    // Whether decoding the line as a request fails
    bool refusedRequest(const std::string &line)
    {
        try
        {
            raster::decodeRequest(line);
            return false;
        }
        catch (const ProtocolError &)
        {
            return true;
        }
    }

    bool refusedResponse(const std::string &line)
    {
        try
        {
            raster::decodeResponse(line);
            return false;
        }
        catch (const ProtocolError &)
        {
            return true;
        }
    }

    json plainThumbnailParams()
    {
        return raster::toJson(raster::ThumbnailParams{256, std::nullopt});
    }

    // ------------------------------------------------------------------ requests

    TEST(RasterProtocol, aRequestSurvivesEncodingAndDecoding)
    {
        Request request;
        request.id = 9007199254740993; // beyond what a double holds
        request.op = raster::op::kThumbnail;
        request.path = io::fromUtf8("/data/ds/orthophoto \xC3\xA8.tif");
        request.out = io::fromUtf8("/tmp/ddb/out/thumb.webp");
        request.params = plainThumbnailParams();

        const std::string line = raster::encodeRequest(request);
        EXPECT_EQ(line.find('\n'), std::string::npos);
        EXPECT_EQ(line.find("path64"), std::string::npos);

        const Request back = raster::decodeRequest(line);
        EXPECT_EQ(back.id, request.id);
        EXPECT_EQ(back.op, request.op);
        EXPECT_EQ(back.path, request.path);
        EXPECT_EQ(back.out, request.out);
        EXPECT_EQ(back.params, request.params);
    }

    TEST(RasterProtocol, aRequestWithoutOutputHasNoOutMember)
    {
        Request request;
        request.id = 1;
        request.op = raster::op::kProbe;
        request.path = io::fromUtf8("/data/a.tif");
        EXPECT_EQ(json::parse(raster::encodeRequest(request)).count("out"), 0u);
        EXPECT_TRUE(raster::decodeRequest(raster::encodeRequest(request)).out.empty());
    }

#ifndef _WIN32
    TEST(RasterProtocol, aPathThatIsNotUtf8TravelsAsBase64)
    {
        Request request;
        request.id = 2;
        request.op = raster::op::kProbe;
        // A file name is bytes on POSIX systems: this one is Latin-1
        request.path = fs::path(std::string("/data/caf\xE9.tif"));
        request.out = fs::path(std::string("/tmp/out\xFF/x.webp"));

        const json line = json::parse(raster::encodeRequest(request));
        EXPECT_FALSE(line.contains("path"));
        EXPECT_FALSE(line.contains("out"));
        EXPECT_EQ(Base64::decode(line.at("path64").get<std::string>()), "/data/caf\xE9.tif");

        const Request back = raster::decodeRequest(line.dump());
        EXPECT_EQ(back.path, request.path);
        EXPECT_EQ(back.out, request.out);
    }
#endif

    TEST(RasterProtocol, aRequestAcceptsTheBase64FormOfAPath)
    {
        const std::string line =
            R"({"id":3,"op":"probe","path64":")" + Base64::encode(std::string("/data/a b.tif")) + R"("})";
        const Request request = raster::decodeRequest(line);
        EXPECT_EQ(request.path, io::fromUtf8("/data/a b.tif"));
        EXPECT_TRUE(request.params.is_object());
        EXPECT_TRUE(request.params.empty());
    }

    TEST(RasterProtocol, aRequestThatIsNotOneIsRefused)
    {
        EXPECT_TRUE(refusedRequest(""));
        EXPECT_TRUE(refusedRequest("not json"));
        EXPECT_TRUE(refusedRequest("[1,2]"));
        EXPECT_TRUE(refusedRequest(R"({"op":"probe","path":"/a"})"));              // no id
        EXPECT_TRUE(refusedRequest(R"({"id":"7","op":"probe","path":"/a"})"));    // id is no integer
        EXPECT_TRUE(refusedRequest(R"({"id":1.5,"op":"probe","path":"/a"})"));
        EXPECT_TRUE(refusedRequest(R"({"id":1,"path":"/a"})"));                    // no op
        EXPECT_TRUE(refusedRequest(R"({"id":1,"op":7,"path":"/a"})"));
        EXPECT_TRUE(refusedRequest(R"({"id":1,"op":"probe","path":7})"));
        EXPECT_TRUE(refusedRequest(R"({"id":1,"op":"probe","path":"/a","params":[]})"));
        EXPECT_TRUE(refusedRequest(R"({"id":1,"op":"probe","path":"/a","path64":"L2E="})"));  // both forms
        EXPECT_TRUE(refusedRequest(R"({"id":1,"op":"probe","path64":"not base64!"})"));
        EXPECT_TRUE(refusedRequest(R"({"id":1,"op":"probe","path64":"L2E"})"));  // padding missing
        EXPECT_TRUE(refusedRequest(R"({"id":1,"op":"probe","path64":5})"));
    }

    TEST(RasterProtocol, aRequestMemberTheProtocolDoesNotKnowIsIgnored)
    {
        const Request request = raster::decodeRequest(R"({"id":4,"op":"probe","path":"/a","later":{"x":1}})");
        EXPECT_EQ(request.id, 4);
    }

    TEST(RasterProtocol, theIdOfARefusedRequestIsKnownWhenItHasOne)
    {
        EXPECT_EQ(raster::requestIdOf(R"({"id":12,"op":5})"), std::optional<int64_t>(12));
        EXPECT_EQ(raster::requestIdOf(R"({"id":"12"})"), std::nullopt);
        EXPECT_EQ(raster::requestIdOf("garbage"), std::nullopt);
        EXPECT_EQ(raster::requestIdOf("[1]"), std::nullopt);
    }

    // ------------------------------------------------------------------ responses

    TEST(RasterProtocol, aSuccessSurvivesEncodingAndDecoding)
    {
        const Response response = Response::success(7, json{{"bytes", 1234}});
        const std::string line = raster::encodeResponse(response);
        EXPECT_EQ(line.find('\n'), std::string::npos);

        const Response back = raster::decodeResponse(line);
        EXPECT_TRUE(back.ok);
        EXPECT_EQ(back.id, std::optional<int64_t>(7));
        EXPECT_EQ(back.result, response.result);
    }

    TEST(RasterProtocol, everyErrorCodeSurvivesEncodingAndDecoding)
    {
        for (const ErrorCode code : {ErrorCode::Unreadable, ErrorCode::Policy, ErrorCode::Limit, ErrorCode::Crashed,
                                     ErrorCode::Timeout, ErrorCode::NoSpace, ErrorCode::Internal})
        {
            SCOPED_TRACE(raster::toString(code));
            const Response back =
                raster::decodeResponse(raster::encodeResponse(Response::failure(5, code, "it failed")));
            EXPECT_FALSE(back.ok);
            EXPECT_EQ(back.error, code);
            EXPECT_EQ(back.message, "it failed");
            EXPECT_EQ(raster::parseErrorCode(raster::toString(code)), std::optional<ErrorCode>(code));
        }
        EXPECT_EQ(raster::parseErrorCode("nope"), std::nullopt);
    }

    TEST(RasterProtocol, theCodesAreTheOnesTheProtocolNames)
    {
        EXPECT_STREQ(raster::toString(ErrorCode::Unreadable), "unreadable");
        EXPECT_STREQ(raster::toString(ErrorCode::Policy), "policy");
        EXPECT_STREQ(raster::toString(ErrorCode::Limit), "limit");
        EXPECT_STREQ(raster::toString(ErrorCode::Crashed), "crashed");
        EXPECT_STREQ(raster::toString(ErrorCode::Timeout), "timeout");
        EXPECT_STREQ(raster::toString(ErrorCode::NoSpace), "nospace");
        EXPECT_STREQ(raster::toString(ErrorCode::Internal), "internal");
    }

    TEST(RasterProtocol, aResponseWithoutAnIdHasANullId)
    {
        const std::string line = raster::encodeResponse(Response::failure(std::nullopt, ErrorCode::Internal, "bad"));
        EXPECT_TRUE(json::parse(line).at("id").is_null());
        EXPECT_EQ(raster::decodeResponse(line).id, std::nullopt);
    }

    TEST(RasterProtocol, aMessageThatIsNotUtf8IsStillAValidLine)
    {
        const std::string line =
            raster::encodeResponse(Response::failure(1, ErrorCode::Unreadable, "bad byte \xFF in a name"));
        const json parsed = json::parse(line); // throws on invalid UTF-8
        EXPECT_NE(parsed.at("message").get<std::string>().find("bad byte"), std::string::npos);
    }

    TEST(RasterProtocol, aLongMessageIsCutAtACharacter)
    {
        // Two-byte characters across the limit: the cut must not leave half of one
        std::string message;
        while (message.size() < 3000)
            message += "\xC3\xA8";
        const Response back =
            raster::decodeResponse(raster::encodeResponse(Response::failure(1, ErrorCode::Internal, message)));
        EXPECT_LE(back.message.size(), raster::kMaxMessageBytes);
        EXPECT_GT(back.message.size(), raster::kMaxMessageBytes - 4);
        EXPECT_EQ(back.message.find("\xEF\xBF\xBD"), std::string::npos) << "no replacement character";
    }

    TEST(RasterProtocol, aResponseThatIsNotOneIsRefused)
    {
        EXPECT_TRUE(refusedResponse(""));
        EXPECT_TRUE(refusedResponse("nope"));
        EXPECT_TRUE(refusedResponse("[]"));
        EXPECT_TRUE(refusedResponse(R"({"id":1})"));                                  // no ok
        EXPECT_TRUE(refusedResponse(R"({"id":1,"ok":"yes"})"));
        EXPECT_TRUE(refusedResponse(R"({"id":"1","ok":true})"));
        EXPECT_TRUE(refusedResponse(R"({"id":1,"ok":true,"result":[1]})"));
        EXPECT_TRUE(refusedResponse(R"({"id":1,"ok":false})"));                       // no error
        EXPECT_TRUE(refusedResponse(R"({"id":1,"ok":false,"error":"bogus"})"));
        EXPECT_TRUE(refusedResponse(R"({"id":1,"ok":false,"error":"policy","message":4})"));
    }

    TEST(RasterProtocol, theLimitsAreTheOnesTheProtocolStates)
    {
        EXPECT_EQ(raster::kMaxLineBytes, 16u * 1024 * 1024);
        EXPECT_EQ(raster::kMaxMessageBytes, 2000u);
    }

    // ------------------------------------------------------------------ version

    TEST(RasterProtocol, theVersionLineIsRecognised)
    {
        const auto version = raster::parseVersionOutput("gdal-util 2.0.0 protocol 1.0\n");
        ASSERT_TRUE(version.has_value());
        EXPECT_EQ(version->major, 1);
        EXPECT_EQ(version->minor, 0);
        EXPECT_EQ(raster::toString(*version), "1.0");
        EXPECT_FALSE(raster::parseVersionOutput("gdal-util 2.0.0\n").has_value());
    }

    TEST(RasterProtocol, aHelperIsAcceptedWhenTheMajorVersionIsTheSameAndTheMinorIsNotLower)
    {
        EXPECT_TRUE(raster::isAccepted({raster::kMajorVersion, raster::kMinorVersion}));
        EXPECT_TRUE(raster::isAccepted({raster::kMajorVersion, raster::kMinorVersion + 1}));
        EXPECT_FALSE(raster::isAccepted({raster::kMajorVersion + 1, raster::kMinorVersion}));
        EXPECT_FALSE(raster::isAccepted({raster::kMajorVersion - 1, 9}));
    }

    // ------------------------------------------------------------------ probe

    BasicPolygonGeometry footprint()
    {
        BasicPolygonGeometry polygon;
        polygon.addPoint(9.1, 45.2, 0.0);
        polygon.addPoint(9.2, 45.2, 0.0);
        polygon.addPoint(9.2, 45.3, 0.0);
        polygon.addPoint(9.1, 45.3, 0.0);
        polygon.addPoint(9.1, 45.2, 0.0);
        return polygon;
    }

    TEST(RasterProtocol, aGeoRasterProbeSurvivesEncodingAndDecoding)
    {
        raster::ProbeResult result;
        result.type = "GeoRaster";
        result.isCog = true;
        result.properties = {{"width", 4096},
                             {"height", 2048},
                             {"geotransform", {500000.0, 0.05, 0.0, 5000000.0, 0.0, -0.05}},
                             {"bands", {{{"type", "Byte"}, {"colorInterp", "Red"}}}}};
        result.wkt = R"(PROJCS["WGS 84 / UTM zone 32N",GEOGCS["WGS 84"]])";
        result.polygonGeom = footprint();
        result.pointGeom.addPoint(9.15, 45.25, 0.0);

        const raster::ProbeResult back = raster::probeResultFromJson(json::parse(raster::toJson(result).dump()));
        EXPECT_EQ(back.type, result.type);
        EXPECT_TRUE(back.isCog);
        EXPECT_EQ(back.properties, result.properties);
        EXPECT_EQ(back.wkt, result.wkt);
        ASSERT_EQ(back.polygonGeom.points.size(), 5u);
        EXPECT_DOUBLE_EQ(back.polygonGeom.points[2].x, 9.2);
        EXPECT_DOUBLE_EQ(back.polygonGeom.points[2].y, 45.3);
        ASSERT_EQ(back.pointGeom.points.size(), 1u);
        EXPECT_DOUBLE_EQ(back.pointGeom.points[0].x, 9.15);
    }

    TEST(RasterProtocol, aProbeWithoutFootprintHasNoGeometryMembers)
    {
        raster::ProbeResult result;
        result.type = "Raster";
        const json encoded = raster::toJson(result);
        EXPECT_FALSE(encoded.contains("wkt"));
        EXPECT_FALSE(encoded.contains("pointGeom"));
        EXPECT_FALSE(encoded.contains("polygonGeom"));
        const raster::ProbeResult back = raster::probeResultFromJson(encoded);
        EXPECT_TRUE(back.polygonGeom.empty());
        EXPECT_TRUE(back.pointGeom.empty());
        EXPECT_TRUE(back.wkt.empty());
    }

    TEST(RasterProtocol, aProbeThatIsNotOneIsRefused)
    {
        const auto refused = [](const json &result) {
            try
            {
                raster::probeResultFromJson(result);
                return false;
            }
            catch (const ProtocolError &)
            {
                return true;
            }
        };
        EXPECT_TRUE(refused(json::array()));
        EXPECT_TRUE(refused({{"properties", json::object()}}));                              // no type
        EXPECT_TRUE(refused({{"type", "Mesh"}, {"properties", json::object()}}));            // unknown type
        EXPECT_TRUE(refused({{"type", "Raster"}}));                                          // no properties
        EXPECT_TRUE(refused({{"type", "Raster"}, {"properties", 5}}));
        EXPECT_TRUE(refused({{"type", "Raster"}, {"properties", json::object()}, {"isCog", "no"}}));
        EXPECT_TRUE(refused({{"type", "Raster"}, {"properties", json::object()}, {"wkt", 4}}));
        EXPECT_TRUE(refused({{"type", "GeoRaster"}, {"properties", json::object()},
                             {"polygonGeom", {{"type", "Point"}, {"coordinates", {1, 2}}}}}));  // wrong geometry
        EXPECT_TRUE(refused({{"type", "GeoRaster"}, {"properties", json::object()},
                             {"pointGeom", {{"type", "Point"}, {"coordinates", {1}}}}}));
        EXPECT_TRUE(refused({{"type", "GeoRaster"}, {"properties", json::object()},
                             {"pointGeom", {{"type", "Point"}, {"coordinates", {1, "x"}}}}}));
        EXPECT_TRUE(refused({{"type", "GeoRaster"}, {"properties", json::object()},
                             {"polygonGeom", {{"type", "Polygon"}, {"coordinates", json::array()}}}}));
    }

    TEST(RasterProtocol, theParamsOfAProbeSurviveEncodingAndDecoding)
    {
        raster::ProbeParams params;
        params.kind = raster::ProbeKind::Vector;
        params.source = io::fromUtf8("/data/plan.dwg");
        params.converted = true;
        const raster::ProbeParams back = raster::probeParamsFromJson(raster::toJson(params));
        EXPECT_EQ(back.kind, raster::ProbeKind::Vector);
        EXPECT_EQ(back.source, params.source);
        EXPECT_TRUE(back.converted);

        const raster::ProbeParams plain = raster::probeParamsFromJson(raster::toJson(raster::ProbeParams()));
        EXPECT_EQ(plain.kind, raster::ProbeKind::Raster);
        EXPECT_TRUE(plain.source.empty());
        EXPECT_FALSE(plain.converted);
    }

    TEST(RasterProtocol, theParamsOfAProbeThatAreNotOnesAreRefused)
    {
        EXPECT_THROW(raster::probeParamsFromJson(json::object()), ProtocolError);                 // no kind
        EXPECT_THROW(raster::probeParamsFromJson({{"kind", "mesh"}}), ProtocolError);
        EXPECT_THROW(raster::probeParamsFromJson({{"kind", "raster"}, {"converted", 1}}), ProtocolError);
        EXPECT_THROW(raster::probeParamsFromJson(json::array()), ProtocolError);
    }

    // ------------------------------------------------------------------ thumbnail

    TEST(RasterProtocol, thePlainThumbnailParamsHaveNoVisualisation)
    {
        const raster::ThumbnailParams back = raster::thumbnailParamsFromJson(plainThumbnailParams());
        EXPECT_EQ(back.size, 256);
        EXPECT_FALSE(back.vis.has_value());
    }

    TEST(RasterProtocol, theVisualisationOfAThumbnailSurvivesEncodingAndDecoding)
    {
        raster::ThumbnailParams params;
        params.size = 512;
        params.vis = ThumbVisParams{"false-color", "4,3,2", "NDVI", "RGBNRe", "rdylgn", "0,0.8"};

        const raster::ThumbnailParams back = raster::thumbnailParamsFromJson(json::parse(raster::toJson(params).dump()));
        EXPECT_EQ(back.size, 512);
        ASSERT_TRUE(back.vis.has_value());
        EXPECT_EQ(back.vis->preset, "false-color");
        EXPECT_EQ(back.vis->bands, "4,3,2");
        EXPECT_EQ(back.vis->formula, "NDVI");
        EXPECT_EQ(back.vis->bandFilter, "RGBNRe");
        EXPECT_EQ(back.vis->colormap, "rdylgn");
        EXPECT_EQ(back.vis->rescale, "0,0.8");
    }

    TEST(RasterProtocol, aVisualisationWithoutValuesIsNotNoVisualisation)
    {
        // The rendering with visualisation parameters differs from the plain one even when all
        // of them are empty, so the difference has to reach the helper
        raster::ThumbnailParams params;
        params.size = 64;
        params.vis = ThumbVisParams();
        const raster::ThumbnailParams back = raster::thumbnailParamsFromJson(raster::toJson(params));
        EXPECT_TRUE(back.vis.has_value());
    }

    TEST(RasterProtocol, theParamsOfAThumbnailThatAreNotOnesAreRefused)
    {
        EXPECT_THROW(raster::thumbnailParamsFromJson(json::object()), ProtocolError);              // no size
        EXPECT_THROW(raster::thumbnailParamsFromJson({{"size", "big"}}), ProtocolError);
        EXPECT_THROW(raster::thumbnailParamsFromJson({{"size", 1.5}}), ProtocolError);
        EXPECT_THROW(raster::thumbnailParamsFromJson({{"size", 64}, {"vis", 3}}), ProtocolError);
        EXPECT_THROW(raster::thumbnailParamsFromJson({{"size", 64}, {"vis", {{"bands", 4}}}}), ProtocolError);
    }
}
