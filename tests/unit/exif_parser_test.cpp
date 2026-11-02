/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// ExifParser over the neutral tag model (exif_metadata.h): the lookups return a tag or nullptr, the
// extractors keep their defaults and their quirks, a tag with too few components throws
// std::out_of_range as the library behind the metadata helper does, and the parser keeps what it
// reads alive. The metadata is built by hand, so none of this needs a helper program.

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "exif.h"
#include "gtest/gtest.h"

namespace
{
    using namespace ddb;
    using exif::ImageMetadata;
    using exif::Tag;

    Tag::Component component(int32_t numerator, int32_t denominator)
    {
        Tag::Component c;
        c.integer = denominator != 0 ? numerator / denominator : 0;
        c.real = denominator != 0 ? static_cast<float>(numerator) / static_cast<float>(denominator) : 0.0F;
        c.rational = {numerator, denominator};
        c.ok = true;
        return c;
    }

    // A Rational tag: "46/1 30/1 0/1"
    Tag rationalTag(const std::string &key, const std::vector<exif::Rational> &values)
    {
        std::string text;
        Tag::Components components;
        for (const auto &v : values)
        {
            if (!text.empty())
                text += " ";
            text += std::to_string(v.first) + "/" + std::to_string(v.second);
            components.push_back(component(v.first, v.second));
        }
        return Tag(key, "Rational", values.size(), text, false, components);
    }

    // The components after the first two are empty, as the helper reports a tag that has only two
    Tag shortRationalTag(const std::string &key, const std::vector<exif::Rational> &values)
    {
        Tag full = rationalTag(key, values);
        Tag::Components components;
        for (const auto &v : values)
            components.push_back(component(v.first, v.second));
        components.resize(3);
        return Tag(key, "Rational", values.size(), full.toString(), false, components);
    }

    Tag asciiTag(const std::string &key, const std::string &text)
    {
        Tag::Components components;
        components.push_back(component(0, 1));
        return Tag(key, "Ascii", text.size() + 1, text, false, components);
    }

    Tag shortTag(const std::string &key, int value)
    {
        Tag::Components components;
        components.push_back(component(value, 1));
        return Tag(key, "Short", 1, std::to_string(value), false, components);
    }

    // An XMP text that is a number: every index answers like the first
    Tag xmpNumber(const std::string &key, double value)
    {
        Tag::Component c;
        c.integer = static_cast<int64_t>(value);
        c.real = static_cast<float>(value);
        c.ok = true;
        Tag::Components components;
        components.push_back(c);
        return Tag(key, "XmpText", std::to_string(value).size(), std::to_string(value), false, components);
    }

    Tag xmpText(const std::string &key, const std::string &text)
    {
        Tag::Components components;
        components.push_back(Tag::Component{});
        return Tag(key, "XmpText", text.size(), text, false, components);
    }

    // Metadata with a parser over it
    struct Parsed
    {
        ImageMetadata metadata;
        ExifParser parser;

        Parsed(int width, int height, std::vector<Tag> exif, std::vector<Tag> xmp)
            : metadata(width, height, std::move(exif), std::move(xmp)), parser(metadata)
        {
        }
    };

    TEST(ExifParser, lookupsReturnTheFirstTagOfTheListOrNullptr)
    {
        const ImageMetadata metadata(64, 32, {asciiTag("Exif.Image.Make", "DJI"), asciiTag("Exif.Image.Model", "FC220")},
                                     {xmpText("Xmp.Camera.BandName", "Green")});
        ExifParser parser(metadata);

        ASSERT_NE(parser.findExifKey("Exif.Image.Make"), nullptr);
        EXPECT_EQ(parser.findExifKey("Exif.Image.Make")->toString(), "DJI");
        EXPECT_EQ(parser.findExifKey("Exif.Image.Orientation"), nullptr);
        // The order of the list decides, not the order of the tags
        EXPECT_EQ(parser.findExifKey({"Exif.Image.Model", "Exif.Image.Make"})->toString(), "FC220");
        EXPECT_EQ(parser.findExifKey({"Exif.Image.Orientation", "Exif.Image.Make"})->toString(), "DJI");
        EXPECT_EQ(parser.findExifKey({"Exif.Image.Orientation", "Exif.Image.Software"}), nullptr);

        EXPECT_EQ(parser.findXmpKey("Xmp.Camera.BandName")->toString(), "Green");
        EXPECT_EQ(parser.findXmpKey("Xmp.Camera.Nothing"), nullptr);
        // A key that is not a valid XMP key is not found, it does not throw
        EXPECT_EQ(parser.findXmpKey("not a key"), nullptr);
        EXPECT_EQ(parser.findXmpKey({"", "Xmp.Camera.BandName"})->toString(), "Green");

        EXPECT_TRUE(parser.hasExif());
        EXPECT_TRUE(parser.hasXmp());
        EXPECT_TRUE(parser.hasTags());
    }

    TEST(ExifParser, withoutTagsEveryExtractorGivesItsDefault)
    {
        const ImageMetadata metadata(100, 50, {}, {});
        ExifParser parser(metadata);

        EXPECT_FALSE(parser.hasTags());
        EXPECT_EQ(parser.extractMake(), "unknown");
        EXPECT_EQ(parser.extractModel(), "unknown");
        EXPECT_EQ(parser.extractImageOrientation(), 1);
        EXPECT_EQ(parser.extractCaptureTime(), 0.0);

        GeoLocation geo;
        EXPECT_FALSE(parser.extractGeo(geo));
        CameraOrientation orientation;
        EXPECT_FALSE(parser.extractCameraOrientation(orientation));
        EXPECT_EQ(orientation.pitch, -90);
        FlightSpeed speed;
        EXPECT_FALSE(parser.extractFlightSpeed(speed));
        GpsAccuracy accuracy;
        EXPECT_FALSE(parser.extractGpsAccuracy(accuracy));
        GpsDirection direction;
        EXPECT_FALSE(parser.extractGpsDirection(direction));
    }

    TEST(ExifParser, theImageSizeIsTheOneTheReaderReported)
    {
        const ImageMetadata metadata(4000, 3000, {}, {});
        ExifParser parser(metadata);
        const auto size = parser.extractImageSize();
        EXPECT_EQ(size.width, 4000);
        EXPECT_EQ(size.height, 3000);

        // 0x0 where the format does not say; the panorama defaults take it as it is
        const ImageMetadata unknown(0, 0, {}, {});
        PanoramaInfo info;
        ASSERT_TRUE(ExifParser(unknown).extractPanoramaInfo(info));
        EXPECT_EQ(info.croppedWidth, 0);
        EXPECT_EQ(info.croppedHeight, 0);
        EXPECT_EQ(info.projectionType, "equirectangular");
    }

    TEST(ExifParser, theGpsTagsGiveThePositionWithItsSigns)
    {
        const ImageMetadata metadata(
            8, 8,
            {rationalTag("Exif.GPSInfo.GPSLatitude", {{46, 1}, {30, 1}, {0, 1}}), asciiTag("Exif.GPSInfo.GPSLatitudeRef", "S"),
             rationalTag("Exif.GPSInfo.GPSLongitude", {{11, 1}, {15, 1}, {36, 1}}), asciiTag("Exif.GPSInfo.GPSLongitudeRef", "w"),
             rationalTag("Exif.GPSInfo.GPSAltitude", {{100, 2}}), shortTag("Exif.GPSInfo.GPSAltitudeRef", 1)},
            {});
        GeoLocation geo;
        ASSERT_TRUE(ExifParser(metadata).extractGeo(geo));
        EXPECT_DOUBLE_EQ(geo.latitude, -46.5);
        EXPECT_DOUBLE_EQ(geo.longitude, -(11.0 + 15.0 / 60.0 + 36.0 / 3600.0));
        EXPECT_DOUBLE_EQ(geo.altitude, -50.0);
    }

    TEST(ExifParser, theDjiXmpTagsRefineTheExifPositionOrStandAlone)
    {
        const std::vector<Tag> gps = {rationalTag("Exif.GPSInfo.GPSLatitude", {{46, 1}, {0, 1}, {0, 1}}),
                                      rationalTag("Exif.GPSInfo.GPSLongitude", {{11, 1}, {0, 1}, {0, 1}}),
                                      rationalTag("Exif.GPSInfo.GPSAltitude", {{10, 1}})};
        const std::vector<Tag> dji = {xmpNumber("Xmp.drone-dji.Latitude", 46.25), xmpNumber("Xmp.drone-dji.Longitude", 11.5),
                                      xmpNumber("Xmp.drone-dji.AbsoluteAltitude", 321.5)};

        GeoLocation refined;
        ASSERT_TRUE(Parsed(8, 8, gps, dji).parser.extractGeo(refined));
        EXPECT_DOUBLE_EQ(refined.latitude, 46.25);
        EXPECT_DOUBLE_EQ(refined.longitude, 11.5);
        EXPECT_DOUBLE_EQ(refined.altitude, 321.5);

        // Some models write XMP only
        GeoLocation alone;
        ASSERT_TRUE(Parsed(8, 8, {}, dji).parser.extractGeo(alone));
        EXPECT_DOUBLE_EQ(alone.latitude, 46.25);
        EXPECT_DOUBLE_EQ(alone.altitude, 321.5);

        // A latitude without a longitude is no position
        GeoLocation none;
        EXPECT_FALSE(Parsed(8, 8, {gps[0]}, {}).parser.extractGeo(none));
    }

    TEST(ExifParser, aVideoPositionIsParsedFromItsXmpText)
    {
        const ImageMetadata metadata(0, 0, {}, {xmpText("Xmp.video.GPSCoordinates", "+46.839139-91.999828+25.700")});
        GeoLocation geo;
        ASSERT_TRUE(ExifParser(metadata).extractGeo(geo));
        EXPECT_DOUBLE_EQ(geo.latitude, 46.839139);
        EXPECT_DOUBLE_EQ(geo.longitude, -91.999828);
        EXPECT_DOUBLE_EQ(geo.altitude, 25.7);
    }

    // A latitude of two numbers: the third component does not exist, as for the library behind the helper
    TEST(ExifParser, aCoordinateWithTooFewComponentsThrowsOutOfRange)
    {
        const ImageMetadata metadata(8, 8,
                                     {shortRationalTag("Exif.GPSInfo.GPSLatitude", {{46, 1}, {30, 1}}),
                                      rationalTag("Exif.GPSInfo.GPSLongitude", {{11, 1}, {15, 1}, {0, 1}})},
                                     {});
        GeoLocation geo;
        EXPECT_THROW(ExifParser(metadata).extractGeo(geo), std::out_of_range);
    }

    TEST(ExifParser, theCaptureTimePrefersTheGpsStampThenTheDateTime)
    {
        // GPS date and time stamp (UTC) win over the local date and time
        const ImageMetadata withStamp(
            8, 8,
            {asciiTag("Exif.GPSInfo.GPSDateStamp", "2024:05:01"),
             rationalTag("Exif.GPSInfo.GPSTimeStamp", {{10, 1}, {20, 1}, {30, 1}}),
             asciiTag("Exif.Photo.DateTimeOriginal", "2020:01:01 00:00:00")},
            {});
        EXPECT_EQ(ExifParser(withStamp).extractCaptureTime(), 1714558830000.0);

        // The date and time with an offset
        const ImageMetadata withOffset(8, 8,
                                       {asciiTag("Exif.Photo.DateTimeOriginal", "2023:03:02 06:05:06"),
                                        asciiTag("Exif.Photo.OffsetTimeOriginal", "+02:00")},
                                       {});
        EXPECT_EQ(ExifParser(withOffset).extractCaptureTime(), 1677729906000.0);

        // Without position or offset the date and time is taken as UTC
        const ImageMetadata naive(8, 8, {asciiTag("Exif.Image.DateTime", "2023:03:02 04:05:06")}, {});
        EXPECT_EQ(ExifParser(naive).extractCaptureTime(), 1677729906000.0);
    }

    TEST(ExifParser, theSenseFlyCameraOrientationIsCorrected)
    {
        const std::vector<Tag> xmp = {xmpNumber("Xmp.Camera.Pitch", 0.0), xmpNumber("Xmp.Camera.Yaw", 45.0),
                                      xmpNumber("Xmp.Camera.Roll", 5.0)};
        CameraOrientation plain;
        ASSERT_TRUE(Parsed(8, 8, {asciiTag("Exif.Image.Make", "DJI")}, xmp).parser.extractCameraOrientation(plain));
        EXPECT_DOUBLE_EQ(plain.pitch, 0.0);
        EXPECT_DOUBLE_EQ(plain.roll, 5.0);

        CameraOrientation senseFly;
        ASSERT_TRUE(Parsed(8, 8, {asciiTag("Exif.Image.Make", "senseFly")}, xmp).parser.extractCameraOrientation(senseFly));
        EXPECT_DOUBLE_EQ(senseFly.pitch, -90.0);
        EXPECT_DOUBLE_EQ(senseFly.yaw, 45.0);
        EXPECT_DOUBLE_EQ(senseFly.roll, -5.0);
    }

    TEST(ExifParser, theGpsSpeedIsConvertedFromItsUnit)
    {
        FlightSpeed kmh;
        ASSERT_TRUE(Parsed(8, 8, {rationalTag("Exif.GPSInfo.GPSSpeed", {{36, 1}})}, {}).parser.extractFlightSpeed(kmh));
        EXPECT_DOUBLE_EQ(kmh.x, 10.0);

        FlightSpeed knots;
        ASSERT_TRUE(Parsed(8, 8,
                           {rationalTag("Exif.GPSInfo.GPSSpeed", {{1944, 1000}}), asciiTag("Exif.GPSInfo.GPSSpeedRef", "N")},
                           {})
                        .parser.extractFlightSpeed(knots));
        EXPECT_NEAR(knots.x, 1.0, 1e-9);
    }

    // The parser built from a shared pointer keeps the metadata, so a parser can leave the scope
    // of the code that read the file; the fingerprint context depends on it
    TEST(ExifParser, aParserThatOwnsItsMetadataOutlivesTheCodeThatMadeIt)
    {
        std::unique_ptr<ExifParser> parser;
        {
            auto metadata = std::make_shared<const ImageMetadata>(
                320, 200, std::vector<Tag>{asciiTag("Exif.Image.Make", "Parrot")}, std::vector<Tag>{});
            parser = std::make_unique<ExifParser>(metadata);
        }
        EXPECT_EQ(parser->extractMake(), "Parrot");
        EXPECT_EQ(parser->extractImageSize().width, 320);

        // Moving the context that holds the parser does not move the metadata
        FingerprintContext first;
        first.parser = std::move(parser);
        FingerprintContext second = std::move(first);
        ASSERT_NE(second.parser, nullptr);
        EXPECT_EQ(second.parser->extractMake(), "Parrot");
    }
}
