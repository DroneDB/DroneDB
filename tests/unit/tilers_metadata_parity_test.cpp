/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The metadata ddb-tilers reports for the rasters libddb reads in its process (ddbtRasterMetadata,
// tilers::Raster::metadata) is GDAL's, string for string: every domain GDALGetMetadataDomainList lists
// (but the derived and sub datasets, which name files), in its order, with the items GDALGetMetadata gives,
// in their order, and the default domain of each band; so the strings the sensor profiles and the thermal
// detection match (gdalmeta::collectMetadata) are the same through either reader.

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "artefact_cache.h"
#include "raster_helper.h"
#include "gdal_inc.h"
#include "gdal_metadata.h"
#include "gdal_raii.h"
#include "mio.h"
#include "raster_source.h"
#include "test.h"
#include "testarea.h"
#include "tilers_raster.h"

namespace
{
    using namespace ddb;

    // What GDAL reports, without the domains that name files
    gdalmeta::RasterMetadata gdalMetadataOf(const fs::path &file)
    {
        const auto ds = adoptDataset(GDALOpen(io::toUtf8(file).c_str(), GA_ReadOnly));
        EXPECT_TRUE(ds) << file;
        if (!ds)
            return {};
        gdalmeta::RasterMetadata metadata = gdalmeta::metadataOf(datasetHandle(ds));
        std::erase_if(metadata.domains, [](const gdalmeta::Domain &d) {
            return d.name == "DERIVED_SUBDATASETS" || d.name == "SUBDATASETS";
        });
        return metadata;
    }

    std::string describe(const gdalmeta::RasterMetadata &m)
    {
        std::ostringstream out;
        for (const auto &d : m.domains)
        {
            out << "[" << d.name << "]\n";
            for (const auto &i : d.items)
                out << "  " << i.substr(0, 200) << "\n";
        }
        for (size_t b = 0; b < m.bandItems.size(); b++)
        {
            out << "[band " << b + 1 << "]\n";
            for (const auto &i : m.bandItems[b])
                out << "  " << i.substr(0, 200) << "\n";
        }
        return out.str();
    }

    // The collected strings of both readers, for the options of the sensor profiles and of the thermal detection
    void expectSameCollected(const fs::path &file)
    {
        const auto ds = adoptDataset(GDALOpen(io::toUtf8(file).c_str(), GA_ReadOnly));
        ASSERT_TRUE(ds);
        const auto raster = tilers::Raster::open(file);
        const auto gdalSource = rasterSourceOf(datasetHandle(ds));
        const auto tilersSource = rasterSourceOf(*raster);
        gdalmeta::CollectOptions profiles;
        profiles.datasetItems = {"TIFFTAG_IMAGEDESCRIPTION", "TIFFTAG_SOFTWARE"};
        profiles.bandCount = gdalSource->bandCount();
        gdalmeta::CollectOptions thermal;
        thermal.bandCount = 1;
        thermal.bandDescriptions = true;
        for (const auto &options : {profiles, thermal})
            EXPECT_EQ(gdalmeta::collectMetadata(*tilersSource, options), gdalmeta::collectMetadata(*gdalSource, options));
        for (int b = 1; b <= gdalSource->bandCount(); b++)
        {
            SCOPED_TRACE("band " + std::to_string(b));
            EXPECT_EQ(tilersSource->colorInterp(b), gdalSource->colorInterp(b));
            EXPECT_EQ(tilersSource->description(b), gdalSource->description(b));
            EXPECT_EQ(tilersSource->unit(b), gdalSource->unit(b));
            EXPECT_EQ(tilersSource->scale(b), gdalSource->scale(b));
            EXPECT_EQ(tilersSource->offset(b), gdalSource->offset(b));
        }
        EXPECT_EQ(tilersSource->wkt(), gdalSource->wkt());
    }

    void expectMetadataAsGdal(const fs::path &file)
    {
        SCOPED_TRACE(file.filename().string());
        const gdalmeta::RasterMetadata gdal = gdalMetadataOf(file);
        const gdalmeta::RasterMetadata tilers = tilers::Raster::open(file)->metadata();
        EXPECT_EQ(tilers, gdal) << "ddb-tilers:\n" << describe(tilers) << "GDAL:\n" << describe(gdal);
        if (std::getenv("DDB_METADATA_PARITY_SHOW") != nullptr)
            std::cout << describe(gdal);
        expectSameCollected(file);
    }

    // A raster GDAL writes with `options`, its pixels a gradient, `setup` setting its metadata
    template <typename Setup>
    fs::path writeGtiff(const fs::path &path, int width, int height, int bands, GDALDataType type,
                        const std::vector<std::string> &options, const Setup &setup)
    {
        CPLStringList list;
        for (const auto &o : options)
            list.AddString(o.c_str());
        auto ds = adoptDataset(GDALCreate(GDALGetDriverByName("GTiff"), io::toUtf8(path).c_str(), width, height,
                                          bands, type, list.List()));
        EXPECT_TRUE(ds) << path << ": " << CPLGetLastErrorMsg();
        if (!ds)
            return path;
        GDALDatasetH h = datasetHandle(ds);
        setup(h);
        std::vector<double> values(static_cast<size_t>(width) * height);
        for (int b = 1; b <= bands; b++)
        {
            for (int y = 0; y < height; y++)
                for (int x = 0; x < width; x++)
                    values[static_cast<size_t>(y) * width + x] = (x * 3 + y * 5 + b * 40) % 200;
            EXPECT_EQ(GDALRasterIO(GDALGetRasterBand(h, b), GF_Write, 0, 0, width, height, values.data(), width, height,
                                   GDT_Float64, 0, 0),
                      CE_None);
        }
        return path;
    }

    void georeference(GDALDatasetH h, int epsg)
    {
        const double gt[6] = {500000.0, 0.5, 0, 4500000.0, 0, -0.5};
        GDALSetGeoTransform(h, const_cast<double *>(gt));
        OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
        OSRImportFromEPSG(srs, epsg);
        GDALSetSpatialRef(h, srs);
        OSRDestroySpatialReference(srs);
    }

    void setItems(GDALMajorObjectH object, const std::vector<std::string> &items, const char *domain)
    {
        CPLStringList list;
        for (const auto &i : items)
            list.AddString(i.c_str());
        EXPECT_EQ(GDALSetMetadata(object, list.List(), domain), CE_None);
    }

    // A COG GDAL's COG driver makes of `source`
    fs::path writeCog(const fs::path &source, const fs::path &path, const std::vector<std::string> &options)
    {
        const auto src = adoptDataset(GDALOpen(io::toUtf8(source).c_str(), GA_ReadOnly));
        CPLStringList list;
        for (const auto &o : options)
            list.AddString(o.c_str());
        auto cog = adoptDataset(GDALCreateCopy(GDALGetDriverByName("COG"), io::toUtf8(path).c_str(), datasetHandle(src),
                                               FALSE, list.List(), nullptr, nullptr));
        EXPECT_TRUE(cog) << CPLGetLastErrorMsg();
        return path;
    }

    // Every item and domain GDAL's GTiff driver reads from the tags and GDAL_METADATA: the TIFFTAG_* items, the
    // items GDAL decodes twice (a value with "&" GDAL cannot decode ends there), the xml: and json: domains, other
    // domains (the EXIF domain of a photo is one), the roles and items of the bands, XMP, an ICC profile, the
    // colorimetry of a TIFF, RPCs, PixelIsPoint, the IMAGE_STRUCTURE of each compression GDAL writes
    TEST(TilersMetadataParity, everyDomainAndItemIsWhatGdalReports)
    {
        TestArea ta(TEST_NAME, true);
        std::vector<fs::path> files;

        files.push_back(writeGtiff(
            ta.getPath("tags.tif"), 300, 200, 3, GDT_Byte,
            {"COMPRESS=DEFLATE", "PREDICTOR=2", "TILED=YES", "PHOTOMETRIC=RGB"}, [](GDALDatasetH h) {
                georeference(h, 32633);
                setItems(h,
                         {"TIFFTAG_DOCUMENTNAME=survey", "TIFFTAG_IMAGEDESCRIPTION=Orthophoto of a field",
                          "TIFFTAG_SOFTWARE=OpenDroneMap 3.5", "TIFFTAG_DATETIME=2026:11:15 10:20:30",
                          "TIFFTAG_ARTIST=pilot", "TIFFTAG_HOSTCOMPUTER=node-7", "TIFFTAG_COPYRIGHT=(c) owner",
                          "TIFFTAG_XRESOLUTION=72.3", "TIFFTAG_YRESOLUTION=300", "TIFFTAG_RESOLUTIONUNIT=3",
                          "TIFFTAG_MINSAMPLEVALUE=3", "TIFFTAG_MAXSAMPLEVALUE=250", "TIFF_RSID=rs-1",
                          "GEO_METADATA=<geo/>", "Camera=MicaSense RedEdge-MX", "Lab=R&D <dept>", "Escaped=a &lt; b",
                          "References=&#65;&#x42;&#X43;", "Path=/data/rasters/a.tif", "Url=https://example.org/x"},
                         nullptr);
                setItems(h, {"EXIF_Make=DJI", "EXIF_Model=ZH20T", "EXIF_FNumber=(2.8)"}, "EXIF");
                setItems(h, {"SATELLITEID=none", "CLOUDCOVER=0"}, "IMAGERY");
                setItems(h,
                         {"<?xpacket begin='' id='W5M0MpCehiHzreSzNTczkc9d'?><x:xmpmeta xmlns:x=\"adobe:ns:meta/\">"
                          "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"><rdf:Description "
                          "Camera:BandName=\"Blue\" xmlns:Camera=\"http://pix4d.com/camera/1.0/\"/></rdf:RDF>"
                          "</x:xmpmeta><?xpacket end='w'?>"},
                         "xml:XMP");
                setItems(h, {"<root><a>1 &amp; 2</a></root>"}, "xml:custom");
                setItems(h, {"{\"bands\": [\"blue\", \"green\"]}"}, "json:custom");
                GDALRasterBandH b1 = GDALGetRasterBand(h, 1);
                setItems(b1, {"WAVELENGTH=475", "FWHM=32", "Note=x &amp; y"}, nullptr);
                setItems(b1, {"CENTRAL=475"}, "SPECTRAL");
                GDALSetDescription(GDALGetRasterBand(h, 2), "Green band");
                GDALSetRasterUnitType(GDALGetRasterBand(h, 3), "reflectance");
                GDALSetRasterOffset(GDALGetRasterBand(h, 3), 0.5);
            }));

        files.push_back(writeGtiff(ta.getPath("multispectral.tif"), 200, 150, 5, GDT_UInt16,
                                   {"COMPRESS=LZW", "PREDICTOR=2", "INTERLEAVE=BAND"}, [](GDALDatasetH h) {
                                       georeference(h, 4326);
                                       const GDALColorInterp interps[5] = {GCI_BlueBand, GCI_GreenBand, GCI_RedBand,
                                                                           GCI_RedEdgeBand, GCI_NIRBand};
                                       for (int b = 1; b <= 5; b++)
                                       {
                                           GDALSetRasterColorInterpretation(GDALGetRasterBand(h, b), interps[b - 1]);
                                           GDALSetRasterScale(GDALGetRasterBand(h, b), 1.0 / 65535);
                                       }
                                       GDALSetMetadataItem(h, "TIFFTAG_SOFTWARE", "Agisoft Metashape", nullptr);
                                   }));

        files.push_back(writeGtiff(ta.getPath("thermal.tif"), 160, 120, 1, GDT_Float32,
                                   {"COMPRESS=DEFLATE", "PREDICTOR=3", "TILED=YES", "BLOCKXSIZE=64", "BLOCKYSIZE=64"},
                                   [](GDALDatasetH h) {
                                       GDALSetMetadataItem(h, "AREA_OR_POINT", "Point", nullptr);
                                       georeference(h, 32632);
                                       GDALRasterBandH b = GDALGetRasterBand(h, 1);
                                       GDALSetDescription(b, "LST");
                                       GDALSetRasterUnitType(b, "Celsius");
                                       GDALSetRasterScale(b, 0.00341802);
                                       GDALSetRasterOffset(b, 149.0);
                                       GDALSetRasterNoDataValue(b, -9999);
                                       setItems(b, {"STATISTICS_MAXIMUM=55.5", "BT=brightness temperature"}, nullptr);
                                   }));

        // An ICC profile; the colorimetry of a TIFF without one (libtiff's default transfer function)
        const std::string icc = "AAACMGxjbXMEQAAAbW50clJHQiBYWVogB+YAAQABAAAAAAAAYWNzcEFQUEwAAAAA";
        files.push_back(writeGtiff(ta.getPath("icc.tif"), 64, 48, 3, GDT_Byte,
                                   {"COMPRESS=DEFLATE", "SOURCE_ICC_PROFILE=" + icc}, [](GDALDatasetH) {}));
        files.push_back(writeGtiff(ta.getPath("colorimetry.tif"), 64, 48, 3, GDT_Byte,
                                   {"SOURCE_PRIMARIES_RED=0.64,0.33,1.0", "SOURCE_PRIMARIES_GREEN=0.3,0.6,1.0",
                                    "SOURCE_PRIMARIES_BLUE=0.15,0.06,1.0", "SOURCE_WHITEPOINT=0.3127,0.329,1.0"},
                                   [](GDALDatasetH) {}));

        files.push_back(writeGtiff(ta.getPath("rpc.tif"), 64, 48, 1, GDT_UInt16, {"COMPRESS=DEFLATE", "PREDICTOR=2", "RPB=NO"},
                                   [](GDALDatasetH h) {
                                       std::string num, den;
                                       for (int i = 0; i < 20; i++)
                                       {
                                           num += std::to_string(0.001 * (i + 1) - 0.0137) + " ";
                                           den += (i == 0 ? std::string("1") : std::to_string(1e-6 * i)) + " ";
                                       }
                                       setItems(h,
                                                {"ERR_BIAS=0.5", "ERR_RAND=0.1", "LINE_OFF=3000", "SAMP_OFF=4000.25",
                                                 "LAT_OFF=45.123456789012", "LONG_OFF=9.87654321", "HEIGHT_OFF=250",
                                                 "LINE_SCALE=3000", "SAMP_SCALE=4000", "LAT_SCALE=0.05",
                                                 "LONG_SCALE=0.07", "HEIGHT_SCALE=500", "LINE_NUM_COEFF=" + num,
                                                 "LINE_DEN_COEFF=" + den, "SAMP_NUM_COEFF=" + num,
                                                 "SAMP_DEN_COEFF=" + den},
                                                "RPC");
                                   }));

        // Odd sizes of samples, MINISWHITE
        files.push_back(writeGtiff(ta.getPath("nbits12.tif"), 64, 48, 2, GDT_UInt16, {"NBITS=12", "COMPRESS=LZW"},
                                   [](GDALDatasetH) {}));
        files.push_back(writeGtiff(ta.getPath("nbits4.tif"), 64, 48, 1, GDT_Byte, {"NBITS=4", "COMPRESS=PACKBITS"},
                                   [](GDALDatasetH) {}));
        files.push_back(writeGtiff(ta.getPath("miniswhite.tif"), 64, 48, 1, GDT_Byte, {"PHOTOMETRIC=MINISWHITE"},
                                   [](GDALDatasetH) {}));

        // JPEG: the quality GDAL guesses from the tables, YCbCr, RGB and grey, 8 and 16-bit tables
        for (const int q : {5, 10, 30, 50, 75, 90, 100})
        {
            const std::string s = std::to_string(q);
            files.push_back(writeGtiff(ta.getPath("ycbcr-" + s + ".tif"), 128, 96, 3, GDT_Byte,
                                       {"COMPRESS=JPEG", "PHOTOMETRIC=YCBCR", "JPEG_QUALITY=" + s, "TILED=YES"},
                                       [](GDALDatasetH) {}));
            files.push_back(writeGtiff(ta.getPath("grey-" + s + ".tif"), 128, 96, 1, GDT_Byte,
                                       {"COMPRESS=JPEG", "JPEG_QUALITY=" + s}, [](GDALDatasetH) {}));
        }
        files.push_back(writeGtiff(ta.getPath("rgb-jpeg.tif"), 128, 96, 3, GDT_Byte,
                                   {"COMPRESS=JPEG", "PHOTOMETRIC=RGB", "JPEG_QUALITY=60"}, [](GDALDatasetH) {}));

        // COGs (GDAL's ghost header) and the layout GDAL checks without one
        const fs::path big = writeGtiff(ta.getPath("big.tif"), 1100, 700, 3, GDT_Byte, {"TILED=YES"},
                                        [](GDALDatasetH h) { georeference(h, 32633); });
        files.push_back(big);  // tiled, larger than 512 pixels, no overviews: not a COG
        files.push_back(writeCog(big, ta.getPath("cog-jpeg.tif"), {"COMPRESS=JPEG", "QUALITY=85"}));
        files.push_back(writeCog(big, ta.getPath("cog-deflate.tif"), {"COMPRESS=DEFLATE", "PREDICTOR=YES"}));
        files.push_back(writeCog(ta.getPath("thermal.tif"), ta.getPath("cog-float.tif"), {"COMPRESS=LZW"}));
        {
            const fs::path withOverviews = writeGtiff(ta.getPath("overviews.tif"), 1100, 700, 1, GDT_Byte,
                                                      {"TILED=YES", "COMPRESS=DEFLATE"}, [](GDALDatasetH) {});
            const auto ds = adoptDataset(GDALOpen(io::toUtf8(withOverviews).c_str(), GA_Update));
            int levels[2] = {2, 4};
            EXPECT_EQ(GDALBuildOverviews(datasetHandle(ds), "AVERAGE", 2, levels, 0, nullptr, nullptr, nullptr), CE_None);
            files.push_back(withOverviews);  // overviews after the data: not a COG
        }
        files.push_back(writeGtiff(ta.getPath("small-tiled.tif"), 300, 200, 1, GDT_Byte, {"TILED=YES"},
                                   [](GDALDatasetH) {}));  // a tiled raster of 512 pixels or less: a COG for GDAL

        for (const auto &file : files)
            expectMetadataAsGdal(file);
    }

    // The normalized copy libddb reads of a raster that is not one of its own (the raster helper keeps its domains
    // in GDAL_METADATA) reports what GDAL reports of it
    TEST(TilersMetadataParity, theNormalizedCopyOfARasterReportsWhatGdalReports)
    {
        TestArea ta(TEST_NAME, true);
        const fs::path source = writeGtiff(ta.getPath("photo.tif"), 120, 90, 3, GDT_Byte,
                                           {"COMPRESS=JPEG", "PHOTOMETRIC=YCBCR"}, [](GDALDatasetH h) {
                                               setItems(h, {"EXIF_Make=DJI", "EXIF_Model=M3T"}, "EXIF");
                                               setItems(h, {"<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"/>"}, "xml:XMP");
                                               setItems(h, {"TIFFTAG_SOFTWARE=FLIR Tools", "Sensor=Boson 640"},
                                                        nullptr);
                                               GDALSetDescription(GDALGetRasterBand(h, 1), "thermal preview");
                                           });
        expectMetadataAsGdal(raster::artefactFor(source));
    }

    // Real rasters (DDB_METADATA_PARITY_FILES, separated by ':'): the file libddb reads for each (the file of a build,
    // the normalized copy of any other)
    TEST(TilersMetadataParity, rastersFromTheEnvironmentReportWhatGdalReports)
    {
        const char *files = std::getenv("DDB_METADATA_PARITY_FILES");
        if (files == nullptr || *files == 0)
            GTEST_SKIP() << "DDB_METADATA_PARITY_FILES is not set";
        std::stringstream list(files);
        for (std::string name; std::getline(list, name, ':');)
        {
            fs::path file;
            try
            {
                file = raster::artefactFor(fs::path(name));
            }
            catch (const raster::RasterNotReadException &e)
            {
                // A raster libddb does not read in one piece has no copy: it is read once built
                if (e.code() != raster::ErrorCode::Limit)
                    throw;
                std::cout << "Not compared (" << e.what() << ")\n";
                continue;
            }
            expectMetadataAsGdal(file);
        }
    }
}
