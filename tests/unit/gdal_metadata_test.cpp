/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <algorithm>
#include <string>
#include <vector>

#include "gdal_metadata.h"
#include "gdal_raii.h"
#include "test.h"

namespace
{
    using namespace ddb;
    using namespace ddb::gdalmeta;

    GDALDatasetUniquePtr memDataset(int bands = 2)
    {
        return adoptDataset(GDALCreate(GDALGetDriverByName("MEM"), "", 4, 4, bands, GDT_Byte, nullptr));
    }

    bool has(const std::vector<std::string> &entries, const std::string &entry)
    {
        return std::find(entries.begin(), entries.end(), entry) != entries.end();
    }

    TEST(GdalMetadata, SubdatasetDomainsAreSkipped)
    {
        auto ds = memDataset();
        GDALSetMetadataItem(ds.get(), "DERIVED_SUBDATASET_1_NAME", "DERIVED_SUBDATASET:LOGAMPLITUDE:file", "DERIVED_SUBDATASETS");
        GDALSetMetadataItem(ds.get(), "SUBDATASET_1_NAME", "NETCDF:file:variable", "SUBDATASETS");
        GDALSetMetadataItem(ds.get(), "SENSOR", "tirs", nullptr);
        GDALSetMetadataItem(ds.get(), "ABOUT", "other", "IMAGE_STRUCTURE");

        const auto entries = collectMetadata(ds.get(), {});

        EXPECT_TRUE(has(entries, "SENSOR=tirs"));
        EXPECT_TRUE(has(entries, "ABOUT=other"));
        EXPECT_FALSE(has(entries, "DERIVED_SUBDATASET_1_NAME=DERIVED_SUBDATASET:LOGAMPLITUDE:file"));
        EXPECT_FALSE(has(entries, "SUBDATASET_1_NAME=NETCDF:file:variable"));
    }

    TEST(GdalMetadata, ValuesThatArePathsOrUrlsAreSkipped)
    {
        auto ds = memDataset();
        GDALSetMetadataItem(ds.get(), "UNIX", "/var/data/file.tif", nullptr);
        GDALSetMetadataItem(ds.get(), "VSI", "/vsimem/file.tif", nullptr);
        GDALSetMetadataItem(ds.get(), "DRIVE", "C:\\data\\file.tif", nullptr);
        GDALSetMetadataItem(ds.get(), "UNC", "\\\\server\\share\\file.tif", nullptr);
        GDALSetMetadataItem(ds.get(), "URL", "see https://example.com/a", nullptr);
        GDALSetMetadataItem(ds.get(), "KEPT", "LST day", nullptr);
        GDALSetMetadataItem(GDALGetRasterBand(ds.get(), 1), "BANDPATH", "/data/b.tif", nullptr);
        GDALSetMetadataItem(GDALGetRasterBand(ds.get(), 1), "BANDKEPT", "kelvin", nullptr);

        CollectOptions options;
        options.bandCount = 2;
        const auto entries = collectMetadata(ds.get(), options);

        EXPECT_TRUE(has(entries, "KEPT=LST day"));
        EXPECT_TRUE(has(entries, "BANDKEPT=kelvin"));
        for (const auto &entry : entries)
            EXPECT_FALSE(isPathOrUrl(entry.substr(entry.find('=') + 1))) << entry;
        EXPECT_FALSE(has(entries, "UNIX=/var/data/file.tif"));
        EXPECT_FALSE(has(entries, "BANDPATH=/data/b.tif"));
    }

    TEST(GdalMetadata, IsPathOrUrl)
    {
        EXPECT_TRUE(isPathOrUrl("/tmp/a.tif"));
        EXPECT_TRUE(isPathOrUrl("  /tmp/a.tif"));
        EXPECT_TRUE(isPathOrUrl("/vsicurl/https://host/a.tif"));
        EXPECT_TRUE(isPathOrUrl("c:/a.tif"));
        EXPECT_TRUE(isPathOrUrl("D:\\a.tif"));
        EXPECT_TRUE(isPathOrUrl("\\\\host\\share"));
        EXPECT_TRUE(isPathOrUrl("file:///a.tif"));
        EXPECT_TRUE(isPathOrUrl("text with http://host in it"));

        EXPECT_FALSE(isPathOrUrl(""));
        EXPECT_FALSE(isPathOrUrl("Kelvin"));
        EXPECT_FALSE(isPathOrUrl("a/b"));
        EXPECT_FALSE(isPathOrUrl("1:2"));
        EXPECT_FALSE(isPathOrUrl("LST: day"));
        EXPECT_FALSE(isPathOrUrl("12 /s"));
    }

    TEST(GdalMetadata, BandScopeAndDescriptionFollowTheOptions)
    {
        auto ds = memDataset(3);
        GDALSetDescription(GDALGetRasterBand(ds.get(), 1), "LST");
        GDALSetDescription(GDALGetRasterBand(ds.get(), 2), "second");
        GDALSetMetadataItem(GDALGetRasterBand(ds.get(), 1), "A", "one", nullptr);
        GDALSetMetadataItem(GDALGetRasterBand(ds.get(), 2), "B", "two", nullptr);

        CollectOptions firstOnly;
        firstOnly.bandCount = 1;
        firstOnly.bandDescriptions = true;
        const auto first = collectMetadata(ds.get(), firstOnly);
        EXPECT_TRUE(has(first, "LST"));
        EXPECT_TRUE(has(first, "A=one"));
        EXPECT_FALSE(has(first, "second"));
        EXPECT_FALSE(has(first, "B=two"));

        CollectOptions allBands;
        allBands.bandCount = 3;
        const auto all = collectMetadata(ds.get(), allBands);
        EXPECT_TRUE(has(all, "A=one"));
        EXPECT_TRUE(has(all, "B=two"));
        EXPECT_FALSE(has(all, "LST"));

        const auto none = collectMetadata(ds.get(), {});
        EXPECT_FALSE(has(none, "A=one"));
        EXPECT_FALSE(has(none, "LST"));
    }

    TEST(GdalMetadata, DatasetItemsComeFirstWhenPresent)
    {
        auto ds = memDataset(1);
        GDALSetMetadataItem(ds.get(), "TIFFTAG_SOFTWARE", "Pix4D", nullptr);

        CollectOptions options;
        options.datasetItems = {"TIFFTAG_IMAGEDESCRIPTION", "TIFFTAG_SOFTWARE"};
        const auto entries = collectMetadata(ds.get(), options);

        ASSERT_FALSE(entries.empty());
        EXPECT_EQ(entries[0], "Pix4D");
        EXPECT_TRUE(has(entries, "TIFFTAG_SOFTWARE=Pix4D"));
        EXPECT_FALSE(has(entries, "TIFFTAG_IMAGEDESCRIPTION="));
    }

    TEST(GdalMetadata, TokensAreMaximalRunsOfLettersAndDigits)
    {
        EXPECT_TRUE(containsToken("bt", "bt"));
        EXPECT_TRUE(containsToken("band=bt", "bt"));
        EXPECT_TRUE(containsToken("bt_band_10", "bt"));
        EXPECT_TRUE(containsToken("x (lst) y", "lst"));
        EXPECT_TRUE(containsToken("a,dem.b", "dem"));
        EXPECT_TRUE(containsToken("LST Day", "lst"));
        EXPECT_TRUE(containsToken("bt10 bt", "bt"));
        // Only ASCII letters and digits make a word
        EXPECT_TRUE(containsToken("\xc2\xb0" "bt", "bt"));

        EXPECT_FALSE(containsToken("subtitles", "tir"));
        EXPECT_FALSE(containsToken("subtitles_bt2", "bt"));
        EXPECT_FALSE(containsToken("academic", "dem"));
        EXPECT_FALSE(containsToken("demo", "dem"));
        EXPECT_FALSE(containsToken("bt10", "bt"));
        EXPECT_FALSE(containsToken("abt", "bt"));
        EXPECT_FALSE(containsToken("", "bt"));
        EXPECT_FALSE(containsToken("bt", ""));
        EXPECT_FALSE(containsToken("b", "bt"));
    }

} // namespace
