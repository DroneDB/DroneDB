/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The metadata model and the client side of the metadata-util protocol (vendor/metadata-util/PROTOCOL.md):
// how a response line becomes tags, what a tag answers for a component it has not got, text that
// is not UTF-8, floats JSON cannot hold, the version line, and the request lines. Canned lines,
// no process.

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../vendor/base64/base64.h"
#include "exif_protocol.h"
#include "gtest/gtest.h"
#include "json.h"
#include "mio.h"

namespace
{
    using namespace ddb;
    using exif::ImageMetadata;
    using exif::Tag;
    namespace protocol = exif::protocol;

    // One read response around the given tags
    std::string readResponse(const std::string &exifTags, const std::string &xmpTags = "", int64_t id = 7)
    {
        return "{\"id\":" + std::to_string(id) + ",\"ok\":true,\"width\":5472,\"height\":3648,\"exif\":[" +
               exifTags + "],\"xmp\":[" + xmpTags + "]}";
    }

    protocol::Response parseRead(const std::string &line)
    {
        return protocol::parseResponse(line, protocol::Operation::Read);
    }

    // A copy of the first Exif tag with the key (an empty tag, and a failure, when there is none)
    Tag only(const ImageMetadata &metadata, const std::string &key)
    {
        const Tag *tag = metadata.findExif(key);
        EXPECT_NE(tag, nullptr) << key;
        return tag != nullptr ? *tag : Tag();
    }

    const char *kLatitude =
        R"({"k":"Exif.GPSInfo.GPSLatitude","t":"Rational","n":3,"s":"46/1 4/1 5129/1000",)"
        R"("v":[[46,46.0,[46,1],true],[4,4.0,[4,1],true],[5,5.129,[5129,1000],true]]})";

    TEST(ExifProtocol, aTagKeepsItsTextAndItsComponents)
    {
        const auto response = parseRead(readResponse(kLatitude));
        ASSERT_TRUE(response.ok);
        ASSERT_TRUE(response.id.has_value());
        EXPECT_EQ(*response.id, 7);
        EXPECT_EQ(response.metadata.width(), 5472);
        EXPECT_EQ(response.metadata.height(), 3648);

        const Tag latitude = only(response.metadata, "Exif.GPSInfo.GPSLatitude");
        EXPECT_EQ(latitude.typeName(), "Rational");
        EXPECT_EQ(latitude.count(), 3u);
        EXPECT_EQ(latitude.toString(), "46/1 4/1 5129/1000");
        EXPECT_FALSE(latitude.truncated());
        EXPECT_EQ(latitude.toInt64(0), 46);
        EXPECT_EQ(latitude.toInt64(1), 4);
        EXPECT_FLOAT_EQ(latitude.toFloat(2), 5.129F);
        EXPECT_EQ(latitude.toRational(2), (exif::Rational{5129, 1000}));
        EXPECT_TRUE(latitude.componentOk(2));
        // The first component when no index is given, as the library behind the helper does
        EXPECT_EQ(latitude.toRational(), (exif::Rational{46, 1}));
    }

    TEST(ExifProtocol, aComponentThatDoesNotExistThrowsOutOfRange)
    {
        // The helper sends null where it got std::out_of_range, and at least 3 entries
        const auto response = parseRead(readResponse(
            R"({"k":"Exif.Image.Make","t":"Ascii","n":2,"s":"D","v":[[68,68.0,[68,1],true],[0,0.0,[0,1],true],null]})"));
        const Tag make = only(response.metadata, "Exif.Image.Make");
        EXPECT_EQ(make.toInt64(0), 68);
        EXPECT_EQ(make.toInt64(1), 0);
        EXPECT_THROW(make.toInt64(2), std::out_of_range) << "null entry";
        EXPECT_THROW(make.toFloat(2), std::out_of_range);
        EXPECT_THROW(make.toRational(2), std::out_of_range);
        EXPECT_THROW(make.toInt64(3), std::out_of_range) << "beyond the list";
        EXPECT_THROW(make.toRational(1000), std::out_of_range);
    }

    TEST(ExifProtocol, aTagWithoutComponentsHasNoNumericValue)
    {
        // A tag the helper sent without components (a write response, a truncated tag)
        const Tag bare("Exif.Image.Make", "Ascii", 4, "DJI", false, {});
        EXPECT_EQ(bare.toString(), "DJI");
        EXPECT_THROW(bare.toInt64(0), std::out_of_range);
        EXPECT_THROW(Tag().toFloat(), std::out_of_range);
    }

    TEST(ExifProtocol, valuesThatIgnoreTheIndexAnswerLikeTheFirstOne)
    {
        // XmpText, LangAlt, Date and Time ignore the index in the library behind the helper: a
        // client may keep one entry, and every index answers with it, also beyond the entries
        const auto response = parseRead(
            readResponse("",
                         R"({"k":"Xmp.drone-dji.GimbalYawDegree","t":"XmpText","n":6,"s":"+12.50",)"
                         R"("v":[[12,12.5,[12,1],true],[12,12.5,[12,1],true],[12,12.5,[12,1],true]]},)"
                         R"({"k":"Xmp.dc.title","t":"LangAlt","n":1,"s":"lang=x-default Title",)"
                         R"("v":[[0,0.0,[0,0],false],[0,0.0,[0,0],false],[0,0.0,[0,0],false]]},)"
                         R"({"k":"Xmp.xmp.CreateDate","t":"Date","n":1,"s":"2024-05-01",)"
                         R"("v":[[0,0.0,[0,1],false],[0,0.0,[0,1],false],[0,0.0,[0,1],false]]})"));
        ASSERT_EQ(response.metadata.xmp().size(), 3u);

        const Tag *yaw = response.metadata.findXmp("Xmp.drone-dji.GimbalYawDegree");
        ASSERT_NE(yaw, nullptr);
        for (const size_t index : {size_t{0}, size_t{1}, size_t{2}, size_t{63}, size_t{5000}})
        {
            EXPECT_FLOAT_EQ(yaw->toFloat(index), 12.5F) << index;
            EXPECT_EQ(yaw->toInt64(index), 12) << index;
        }

        const Tag *title = response.metadata.findXmp("Xmp.dc.title");
        ASSERT_NE(title, nullptr);
        EXPECT_EQ(title->toRational(40), (exif::Rational{0, 0}));
        EXPECT_FALSE(title->componentOk(40));

        const Tag *created = response.metadata.findXmp("Xmp.xmp.CreateDate");
        ASSERT_NE(created, nullptr);
        EXPECT_EQ(created->toInt64(9), 0);
    }

    TEST(ExifProtocol, otherValuesDoNotIgnoreTheIndex)
    {
        // An XMP array is bounded by its components like a numeric tag
        const auto response = parseRead(readResponse(
            "", R"({"k":"Xmp.dc.subject","t":"XmpBag","n":1,"s":"a","v":[[1,1.0,[1,1],true],null,null]})"));
        const Tag *subject = response.metadata.findXmp("Xmp.dc.subject");
        ASSERT_NE(subject, nullptr);
        EXPECT_EQ(subject->toInt64(0), 1);
        EXPECT_THROW(subject->toInt64(1), std::out_of_range);
    }

    TEST(ExifProtocol, floatsJsonCannotHoldArriveAsStrings)
    {
        const auto response = parseRead(readResponse(
            R"({"k":"Exif.Photo.A","t":"Float","n":3,"s":"nan inf -inf","v":)"
            R"([[0,"NaN",[0,1],false],[0,"Infinity",[0,1],false],[0,"-Infinity",[0,1],false]]})"));
        const Tag tag = only(response.metadata, "Exif.Photo.A");
        EXPECT_TRUE(std::isnan(tag.toFloat(0)));
        EXPECT_EQ(tag.toFloat(1), std::numeric_limits<float>::infinity());
        EXPECT_EQ(tag.toFloat(2), -std::numeric_limits<float>::infinity());
        EXPECT_FALSE(tag.componentOk(0));
    }

    TEST(ExifProtocol, aStringThatIsNotAFloatIsABrokenResponse)
    {
        EXPECT_THROW(parseRead(readResponse(R"({"k":"Exif.Photo.A","t":"Float","n":1,"s":"x","v":)"
                                            R"([[0,"banana",[0,1],false],null,null]})")),
                     protocol::ProtocolError);
    }

    TEST(ExifProtocol, textThatIsNotUtf8ArrivesAsBase64)
    {
        // "Café" in Latin-1, and a key with an invalid byte
        const std::string latin1 = "Caf\xE9";
        const std::string line =
            readResponse(R"({"k":"Exif.Image.Make","t":"Ascii","n":5,"s64":")" + Base64::encode(latin1) +
                         R"(","v":[[67,67.0,[67,1],true],[97,97.0,[97,1],true],[102,102.0,[102,1],true]]},)"
                         R"({"k64":")" +
                         Base64::encode("Exif.Odd\xFF") + R"(","t":"Ascii","n":1,"s":"x","v":[null,null,null]})");
        const auto response = parseRead(line);
        ASSERT_EQ(response.metadata.exif().size(), 2u);
        EXPECT_EQ(response.metadata.exif()[0].toString(), latin1) << "the raw bytes, not a conversion";
        EXPECT_EQ(response.metadata.exif()[1].key(), "Exif.Odd\xFF");
        EXPECT_NE(response.metadata.findExif("Exif.Odd\xFF"), nullptr);
    }

    TEST(ExifProtocol, textThatIsNotBase64IsABrokenResponse)
    {
        EXPECT_THROW(parseRead(readResponse(R"({"k":"A","t":"Ascii","n":1,"s64":"not base64!","v":[null,null,null]})")),
                     protocol::ProtocolError);
        EXPECT_THROW(parseRead(readResponse(R"({"k":"A","t":"Ascii","n":1,"s64":"abc","v":[null,null,null]})")),
                     protocol::ProtocolError)
            << "the length is not a multiple of 4";
    }

    TEST(ExifProtocol, aTruncatedTagHasNoValue)
    {
        const auto response = parseRead(readResponse(R"({"k":"Exif.Photo.MakerNote","t":"Undefined","n":90000,"trunc":true})"));
        const Tag note = only(response.metadata, "Exif.Photo.MakerNote");
        EXPECT_TRUE(note.truncated());
        EXPECT_EQ(note.count(), 90000u);
        EXPECT_TRUE(note.toString().empty());
        EXPECT_THROW(note.toInt64(0), std::out_of_range);
    }

    TEST(ExifProtocol, theFirstTagWithAKeyWins)
    {
        const auto response = parseRead(readResponse(
            R"({"k":"Exif.Image.Make","t":"Ascii","n":2,"s":"A","v":[[65,65.0,[65,1],true],[0,0.0,[0,1],true],null]},)"
            R"({"k":"Exif.Image.Make","t":"Ascii","n":2,"s":"B","v":[[66,66.0,[66,1],true],[0,0.0,[0,1],true],null]})"));
        EXPECT_EQ(only(response.metadata, "Exif.Image.Make").toString(), "A");
        EXPECT_EQ(response.metadata.findExif("Exif.Image.Model"), nullptr);
        EXPECT_EQ(response.metadata.findXmp("Exif.Image.Make"), nullptr) << "the lists are separate";
        EXPECT_TRUE(response.metadata.hasExif());
        EXPECT_FALSE(response.metadata.hasXmp());
    }

    TEST(ExifProtocol, aCopyOfTheMetadataLooksUpInItsOwnTags)
    {
        const auto response = parseRead(readResponse(kLatitude));
        ImageMetadata copy = response.metadata;
        const Tag *original = response.metadata.findExif("Exif.GPSInfo.GPSLatitude");
        const Tag *copied = copy.findExif("Exif.GPSInfo.GPSLatitude");
        ASSERT_NE(original, nullptr);
        ASSERT_NE(copied, nullptr);
        EXPECT_NE(original, copied);
        ImageMetadata moved = std::move(copy);
        EXPECT_NE(moved.findExif("Exif.GPSInfo.GPSLatitude"), nullptr);
    }

    TEST(ExifProtocol, aFileWithoutMetadataIsAFailureWithAnError)
    {
        const auto response = parseRead(R"({"id":9,"ok":false,"error":"exiv2","message":"corrupted image metadata"})");
        EXPECT_FALSE(response.ok);
        EXPECT_EQ(*response.id, 9);
        EXPECT_EQ(response.error, "exiv2");
        EXPECT_EQ(response.message, "corrupted image metadata");
        EXPECT_TRUE(protocol::isFileError(response.error));
        EXPECT_TRUE(protocol::isFileError("open"));
        EXPECT_FALSE(protocol::isFileError("request"));
        EXPECT_FALSE(protocol::isFileError("something new"));
    }

    TEST(ExifProtocol, aRequestTheHelperCouldNotReadHasNoId)
    {
        const auto response =
            parseRead(R"({"id":null,"ok":false,"error":"request","message":"not a valid JSON line"})");
        EXPECT_FALSE(response.id.has_value());
        EXPECT_EQ(response.error, "request");
    }

    TEST(ExifProtocol, aWriteResponseListsTheTagsSet)
    {
        const auto response = protocol::parseResponse(
            R"({"id":8,"ok":true,"tags":[{"k":"Exif.GPSInfo.GPSLatitudeRef","t":"Ascii","n":2,"s":"N"}]})",
            protocol::Operation::Write);
        ASSERT_TRUE(response.ok);
        ASSERT_EQ(response.tags.size(), 1u);
        EXPECT_EQ(response.tags[0].key(), "Exif.GPSInfo.GPSLatitudeRef");
        EXPECT_EQ(response.tags[0].toString(), "N");
    }

    TEST(ExifProtocol, aResponseThatBreaksTheProtocolIsRejected)
    {
        const char *broken[] = {
            "",
            "not json",
            "[]",
            "{\"id\":7}",
            "{\"id\":7,\"ok\":\"yes\"}",
            "{\"id\":\"7\",\"ok\":true,\"width\":1,\"height\":1,\"exif\":[],\"xmp\":[]}",
            "{\"id\":7,\"ok\":false}",
            "{\"id\":7,\"ok\":true,\"height\":1,\"exif\":[],\"xmp\":[]}",
            "{\"id\":7,\"ok\":true,\"width\":1,\"height\":1,\"exif\":{},\"xmp\":[]}",
            "{\"id\":7,\"ok\":true,\"width\":1,\"height\":1,\"exif\":[1],\"xmp\":[]}",
            "{\"id\":7,\"ok\":true,\"width\":1,\"height\":1,\"exif\":[{\"k\":\"A\",\"t\":\"Ascii\",\"n\":1,\"s\":\"x\"}],\"xmp\":[]}",
            "{\"id\":7,\"ok\":true,\"width\":1,\"height\":1,\"exif\":[{\"k\":\"A\",\"t\":\"Ascii\",\"n\":1,\"s\":\"x\",\"v\":[[1,2]]}],\"xmp\":[]}",
            "{\"id\":7,\"ok\":true,\"width\":99999999999,\"height\":1,\"exif\":[],\"xmp\":[]}",
            "{\"id\":7,\"ok\":true,\"width\":1,\"height\":1,\"exif\":[{\"k\":\"A\",\"t\":\"Rational\",\"n\":1,\"s\":\"x\",\"v\":[[1,1.0,[99999999999,1],true]]}],\"xmp\":[]}",
        };
        for (const char *line : broken)
            EXPECT_THROW(parseRead(line), protocol::ProtocolError) << line;
    }

    TEST(ExifProtocol, aResponseIsReadWhateverTheSpacingAndTheEscapes)
    {
        // Any JSON the protocol allows: spaces everywhere, every escape, a surrogate pair,
        // numbers in every form, and members a later minor version may add (an integer beyond
        // 64 bits and a number too small for a double among them)
        const std::string line =
            " { \"id\" : 7 , \"ok\" : true , \"width\" : 1 , \"height\" : 2 ,\n"
            "   \"exif\" : [ { \"k\" : \"Exif.Image.\\u004dake\" , \"t\" : \"Ascii\" , \"n\" : 4 ,"
            "   \"s\" : \"D\\u004aI \\u00e9 \\ud83d\\ude00 \\\"q\\\" \\\\ \\/ \\b\\f\\n\\r\\t\" ,"
            "   \"v\" : [ [ 68 , 6.8E+1 , [ 68 , 1 ] , true ] , [ -3 , -0.5e0 , [ -1 , 2 ] , false ] , null ] } ] ,\t"
            "   \"xmp\" : [ ] ,"
            "   \"later\" : { \"a\" : [ 18446744073709551616 , 1e-400 , -0 , \"x\" , false , null , { } , [ ] ] } } \r";
        const auto response = parseRead(line);
        ASSERT_EQ(response.id, 7);
        EXPECT_EQ(response.metadata.width(), 1);
        EXPECT_EQ(response.metadata.height(), 2);
        const Tag make = only(response.metadata, "Exif.Image.Make");
        EXPECT_EQ(make.toString(), "DJI \xC3\xA9 \xF0\x9F\x98\x80 \"q\" \\ / \b\f\n\r\t");
        EXPECT_EQ(make.toInt64(0), 68);
        EXPECT_FLOAT_EQ(make.toFloat(0), 68.0F);
        EXPECT_EQ(make.toRational(0), (exif::Rational{68, 1}));
        EXPECT_EQ(make.toInt64(1), -3);
        EXPECT_FLOAT_EQ(make.toFloat(1), -0.5F);
        EXPECT_EQ(make.toRational(1), (exif::Rational{-1, 2}));
        EXPECT_FALSE(make.componentOk(1));
        EXPECT_THROW(make.toInt64(2), std::out_of_range);
    }

    TEST(ExifProtocol, aLineThatIsNotJsonIsRejected)
    {
        const std::string good = R"({"id":7,"ok":false,"error":"open","message":"m"})";
        ASSERT_NO_THROW(parseRead(good));
        const std::string broken[] = {
            good + " x",                                                     // text after the value
            good + good,                                                     // two values
            R"({"id":7,"ok":false,"error":"open","message":"a)" "\x01" R"(b"})", // a control character
            R"({"id":7,"ok":false,"error":"open","message":"\ud800"})",      // a lone high surrogate
            R"({"id":7,"ok":false,"error":"open","message":"\udc00"})",      // a lone low surrogate
            R"({"id":7,"ok":false,"error":"open","message":"\ud800\u0041"})", // not a pair
            R"({"id":7,"ok":false,"error":"open","message":"\x"})",          // an invalid escape
            R"({"id":7,"ok":false,"error":"open","message":"\u12"})",        // a short escape
            R"({"id":7,"ok":false,"error":"open","message":")" "\xFF" R"("})",  // not UTF-8
            R"({"id":7,"ok":false,"error":"open","message":")" "\xC0\x80" R"("})", // overlong
            R"({"id":7,"ok":false,"error":"open","message":"m)",             // a string that does not end
            R"({"id":07,"ok":false,"error":"open","message":"m"})",          // a leading zero
            R"({"id":7,"ok":fals,"error":"open","message":"m"})",            // a broken literal
            R"({"id":7,"ok":false,"error":"open","message":"m","x":1e999})", // beyond a double
            R"({"id":7,"ok":false,"error":"open","message":"m","x":1.})",    // no digit after the point
            R"({"id":7,"ok":false,"error":"open","message":"m","x":1e})",    // no digit in the exponent
            R"({"id":7,"ok":false,"error":"open","message":"m","x":-})",     // a sign alone
            R"({"id":7,"ok":false,"error":"open","message":"m","x":+1})",    // a plus sign
            R"({"id":7,"ok":false,"error":"open","message":"m",})",          // a trailing comma
            R"({"id":7,"ok":false,"error":"open","message":"m","x":[1,]})",  // a trailing comma in a list
            R"({"id":7,"ok":false,"error":"open","message":"m","x":[1 2]})", // no comma
            R"({"id":7 "ok":false,"error":"open","message":"m"})",           // no comma between members
            R"({"id":7,"ok":false,"error":"open","message"})",               // a member without a value
            R"({id:7})",                                                     // a name without quotes
        };
        for (const auto &line : broken)
            EXPECT_THROW(parseRead(line), protocol::ProtocolError) << line;
    }

    TEST(ExifProtocol, anUnsignedIntegerBeyondInt64IsSkippedInAMemberThisClientDoesNotKnow)
    {
        // A later 1.x helper may add members of any JSON value: the range of an integer matters
        // only where the protocol defines one
        for (const std::string big : {"9223372036854775808", "18446744073709551615"})
        {
            SCOPED_TRACE(big);
            const auto response = parseRead(
                R"({"id":7,"ok":true,"width":1,"height":1,"later":)" + big + R"(,"exif":[)"
                R"({"k":"A","t":"Ascii","n":1,"s":"x","later":)" + big + R"(,"v":[[1,1.0,[1,1],true]]}],)"
                R"("xmp":[],"more":{"a":[)" + big + "]}}");
            EXPECT_EQ(response.id, 7);
            EXPECT_EQ(only(response.metadata, "A").toInt64(0), 1);

            // The id, a count and a component's integer: valid with 1, rejected with the big one
            const auto definedMembers = [](const std::string &value) {
                return std::vector<std::string>{
                    R"({"id":)" + value + R"(,"ok":false,"error":"open","message":"m"})",
                    readResponse(R"({"k":"A","t":"Ascii","n":)" + value + R"(,"s":"x","v":[[1,1.0,[1,1],true]]})"),
                    readResponse(R"({"k":"A","t":"Ascii","n":1,"s":"x","v":[[)" + value + R"(,1.0,[1,1],true]]})")};
            };
            for (const std::string &line : definedMembers("1"))
                EXPECT_NO_THROW(parseRead(line)) << line;
            for (const std::string &line : definedMembers(big))
                EXPECT_THROW(parseRead(line), protocol::ProtocolError) << line;
        }
    }

    TEST(ExifProtocol, aFloatWithAnExponentAtTheEndOfInt64IsZeroWhenTinyAndRejectedWhenHuge)
    {
        // A number too close to zero for a double is 0 (with its sign); one too far from it is
        // not a number this client can hold. That holds also when the exponent alone fills 64
        // bits or goes beyond them: the parser reads such a number as 0 or as an infinity, and
        // rejects an infinity.
        const auto floatOf = [](const std::string &number) {
            const auto response =
                parseRead(readResponse(R"({"k":"A","t":"Float","n":1,"s":"x","v":[[0,)" + number + R"(,[0,1],true]]})"));
            return only(response.metadata, "A").toFloat(0);
        };
        for (const std::string tiny : {"1e-9223372036854775807", "0.01e-9223372036854775807",
                                       "10e-9223372036854775807", "0.01e-99999999999999999999"})
        {
            EXPECT_EQ(floatOf(tiny), 0.0F) << tiny;
            EXPECT_FALSE(std::signbit(floatOf(tiny))) << tiny;
            EXPECT_EQ(floatOf("-" + tiny), 0.0F) << tiny;
            EXPECT_TRUE(std::signbit(floatOf("-" + tiny))) << tiny;
        }
        for (const std::string huge : {"1e9223372036854775807", "10e9223372036854775807", "10e+9223372036854775807",
                                       "0.01e9223372036854775807", "10e99999999999999999999"})
        {
            EXPECT_THROW(floatOf(huge), protocol::ProtocolError) << huge;
            EXPECT_THROW(floatOf("-" + huge), protocol::ProtocolError) << huge;
        }
    }

    TEST(ExifProtocol, aNestingDeeperThanAResponseHasIsRejectedWithoutExhaustingTheStack)
    {
        const size_t depth = 100000;
        const std::string line = R"({"id":7,"ok":false,"error":"open","message":"m","later":)" + std::string(depth, '[') +
                                 std::string(depth, ']') + "}";
        EXPECT_THROW(parseRead(line), protocol::ProtocolError);
    }

    TEST(ExifProtocol, theVersionIsTheEndOfTheVersionLine)
    {
        const auto version = protocol::parseVersionOutput("metadata-util 2.0.0 (Exiv2 0.28.9) protocol 1.0\n");
        ASSERT_TRUE(version.has_value());
        EXPECT_EQ(version->major, 1);
        EXPECT_EQ(version->minor, 0);

        // Whatever precedes, a stub of another product, Windows line ends, text before the line
        const auto stub = protocol::parseVersionOutput("a banner\r\nmy-stub 9.9 protocol 2.13  \r\n");
        ASSERT_TRUE(stub.has_value());
        EXPECT_EQ(stub->major, 2);
        EXPECT_EQ(stub->minor, 13);
        EXPECT_EQ(protocol::toString(*stub), "2.13");

        for (const char *text : {"", "metadata-util 2.0.0", "protocol ", "protocol 1", "protocol 1.", "protocol .1",
                                 "protocol 1.0 extra", "protocol a.b", "protocol 1.0\nand then more"})
            EXPECT_FALSE(protocol::parseVersionOutput(text).has_value()) << "[" << text << "]";
    }

    TEST(ExifProtocol, aHelperIsAcceptedWhenItsMajorIsTheSameAndItsMinorIsNotLower)
    {
        EXPECT_TRUE(protocol::isAccepted({protocol::kMajorVersion, protocol::kMinorVersion}));
        EXPECT_TRUE(protocol::isAccepted({protocol::kMajorVersion, protocol::kMinorVersion + 3}));
        EXPECT_FALSE(protocol::isAccepted({protocol::kMajorVersion + 1, protocol::kMinorVersion}));
        EXPECT_FALSE(protocol::isAccepted({protocol::kMajorVersion - 1, 99}));
        if (protocol::kMinorVersion > 0)
        {
            EXPECT_FALSE(protocol::isAccepted({protocol::kMajorVersion, protocol::kMinorVersion - 1}));
        }
    }

    TEST(ExifProtocol, aReadRequestCarriesTheIdAndThePath)
    {
        const fs::path absolute = fs::current_path() / io::fromUtf8("caf\xC3\xA9.jpg");
        const auto request = json::parse(protocol::readRequest(41, absolute));
        EXPECT_EQ(request.at("id"), 41);
        EXPECT_EQ(request.at("op"), "read");
        EXPECT_EQ(request.at("path"), io::toUtf8(absolute));
        EXPECT_FALSE(request.contains("path64"));
    }

    TEST(ExifProtocol, aRelativePathIsSentAbsoluteInTheWorkingDirectoryOfNow)
    {
        // The helper resolves paths in the folder it started in, not in the caller's. The
        // expected path is joined component by component, so it has the separators of the
        // platform (an absolute path on Windows has backslashes only). That the working
        // directory is the one at the time of the request, not at the start of the helper, is
        // ExifHelperPool.aRelativePathIsReadFromTheWorkingDirectoryTheCallerHasWhenItReads.
        const fs::path relative = io::fromUtf8("dir/caf\xC3\xA9.jpg");
        const std::string expected = io::toUtf8(fs::current_path() / "dir" / io::fromUtf8("caf\xC3\xA9.jpg"));
        const auto read = json::parse(protocol::readRequest(1, relative));
        EXPECT_EQ(read.at("path"), expected);
        const auto write = json::parse(protocol::writeRequest(2, relative, {{"Exif.Image.Make", "DJI"}}));
        EXPECT_EQ(write.at("path"), expected);
    }

#ifndef _WIN32
    TEST(ExifProtocol, aPathThatIsNotUtf8IsSentAsBase64)
    {
        // A file name in Latin-1: raw bytes on Linux
        const std::string name = "/dir/caf\xE9.jpg";
        const auto request = json::parse(protocol::readRequest(1, fs::path(name)));
        EXPECT_FALSE(request.contains("path"));
        ASSERT_TRUE(request.contains("path64"));
        EXPECT_EQ(Base64::decode(request.at("path64").get<std::string>()), name);
    }
#endif

    TEST(ExifProtocol, aWriteRequestKeepsTheOrderAndEncodesWhatIsNotUtf8)
    {
        const exif::TagAssignments set = {{"Exif.GPSInfo.GPSLatitudeRef", "N"},
                                          {"Exif.Image.Make", "Caf\xE9"},
                                          {"Exif.GPSInfo.GPSLatitudeRef", "S"}};
        const auto request = json::parse(protocol::writeRequest(5, fs::path("a.jpg"), set));
        EXPECT_EQ(request.at("op"), "write");
        const auto &assignments = request.at("set");
        ASSERT_EQ(assignments.size(), 3u);
        EXPECT_EQ(assignments[0].at("key"), "Exif.GPSInfo.GPSLatitudeRef");
        EXPECT_EQ(assignments[0].at("value"), "N");
        EXPECT_FALSE(assignments[1].contains("value"));
        EXPECT_EQ(Base64::decode(assignments[1].at("value64").get<std::string>()), "Caf\xE9");
        EXPECT_EQ(assignments[2].at("value"), "S");
    }

    TEST(ExifProtocol, aKeyThatIsNotTextCannotBeSent)
    {
        EXPECT_THROW(protocol::writeRequest(5, fs::path("a.jpg"), {{"Exif.\xFF", "x"}}), protocol::ProtocolError);
    }
}
