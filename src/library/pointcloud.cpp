/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <pdal/StageFactory.hpp>
#include <pdal/PointRef.hpp>
#include <pdal/io/LasWriter.hpp>
#include "gdal_inc.h"
#include <untwine/Common.hpp>
#include <untwine/ProgressWriter.hpp>
#include <epf/Epf.hpp>
#include <bu/BuPyramid.hpp>

#include "pointcloud.h"
#include "entry.h"
#include "exceptions.h"
#include "mio.h"
#include "geo.h"
#include "logger.h"
#include "ply.h"
#include <cpr/cpr.h>
#include "utils.h"

namespace untwine
{

    void fatal(const std::string &err)
    {
        throw ddb::UntwineException("Untwine fatal error: " + err);
    }

}

namespace ddb
{

    bool getPointCloudInfo(const std::string &filename, PointCloudInfo &info, int polyBoundsSrs)
    {
        if (io::Path(filename).checkExtension({"ply"}))
        {
            PlyInfo plyInfo;
            if (!getPlyInfo(filename, plyInfo))
                return false;
            else
            {
                info.bounds.clear();
                info.polyBounds.clear();
                info.pointCount = plyInfo.vertexCount;
                info.dimensions = plyInfo.dimensions;
                return true;
            }
        }

        // Las/Laz
        try
        {

            pdal::StageFactory factory;
            std::string driver = pdal::StageFactory::inferReaderDriver(filename);
            if (driver.empty())
            {
                LOGD << "Can't infer point cloud reader from " << filename;
                return false;
            }

            pdal::Stage *s = factory.createStage(driver);
            pdal::Options opts;
            opts.add("filename", filename);
            s->setOptions(opts);

            pdal::QuickInfo qi = s->preview();
            if (!qi.valid())
            {
                LOGD << "Cannot get quick info for point cloud " << filename;
                return false;
            }

            info.pointCount = qi.m_pointCount;

            if (qi.m_srs.valid())
            {
                info.wktProjection = qi.m_srs.getWKT();
            }
            else
            {
                info.wktProjection = "";
            }

            info.dimensions.clear();
            for (auto &dim : qi.m_dimNames)
            {
                info.dimensions.push_back(dim);
            }

            info.bounds.clear();
            if (qi.m_bounds.valid())
            {
                info.bounds.push_back(qi.m_bounds.minx);
                info.bounds.push_back(qi.m_bounds.miny);
                info.bounds.push_back(qi.m_bounds.minz);
                info.bounds.push_back(qi.m_bounds.maxx);
                info.bounds.push_back(qi.m_bounds.maxy);
                info.bounds.push_back(qi.m_bounds.maxz);

                pdal::BOX3D bbox = qi.m_bounds;

                // We need to convert the bbox to EPSG:<polyboundsSrs>
                if (qi.m_srs.valid())
                {
                    OGRSpatialReferenceH hSrs = OSRNewSpatialReference(nullptr);
                    OGRSpatialReferenceH hTgt = OSRNewSpatialReference(nullptr);

                    std::string proj = qi.m_srs.getProj4();
                    if (OSRImportFromProj4(hSrs, proj.c_str()) != OGRERR_NONE)
                    {
                        throw GDALException("Cannot import spatial reference system " + proj + ". Is PROJ available?");
                    }
                    OSRSetAxisMappingStrategy(hSrs, OSRAxisMappingStrategy::OAMS_TRADITIONAL_GIS_ORDER);

                    OSRImportFromEPSG(hTgt, polyBoundsSrs);
                    OGRCoordinateTransformationH hTransform = OCTNewCoordinateTransformation(hSrs, hTgt);

                    double geoMinX = bbox.minx;
                    double geoMinY = bbox.miny;
                    double geoMinZ = bbox.minz;
                    double geoMaxX = bbox.maxx;
                    double geoMaxY = bbox.maxy;
                    double geoMaxZ = bbox.maxz;

                    bool minSuccess = OCTTransform(hTransform, 1, &geoMinX, &geoMinY, &geoMinZ);
                    bool maxSuccess = OCTTransform(hTransform, 1, &geoMaxX, &geoMaxY, &geoMaxZ);

                    if (!minSuccess || !maxSuccess)
                    {
                        throw GDALException("Cannot transform coordinates " + bbox.toWKT() + " to " + proj);
                    }

                    info.polyBounds.clear();

                    if (geoMinZ < -30000 || geoMaxZ > 30000 || (geoMinX == -90 && geoMaxX == 90))
                    {
                        LOGD << "Strange point cloud bounds [[" << geoMinX << ", " << geoMaxX << "], [" << geoMinY << ", " << geoMaxY << "], [" << geoMinZ << ", " << geoMaxZ << "]]";
                        info.bounds.clear();
                    }
                    else
                    {
                        info.polyBounds.addPoint(geoMinY, geoMinX, geoMinZ);
                        info.polyBounds.addPoint(geoMinY, geoMaxX, geoMinZ);
                        info.polyBounds.addPoint(geoMaxY, geoMaxX, geoMinZ);
                        info.polyBounds.addPoint(geoMaxY, geoMinX, geoMinZ);
                        info.polyBounds.addPoint(geoMinY, geoMinX, geoMinZ);

                        double centroidX = (bbox.minx + bbox.maxx) / 2.0;
                        double centroidY = (bbox.miny + bbox.maxy) / 2.0;
                        double centroidZ = bbox.minz;

                        if (OCTTransform(hTransform, 1, &centroidX, &centroidY, &centroidZ))
                        {
                            info.centroid.clear();
                            info.centroid.addPoint(centroidY, centroidX, centroidZ);
                        }
                        else
                        {
                            throw GDALException("Cannot transform coordinates " + std::to_string(centroidX) + ", " + std::to_string(centroidY) + " to " + proj);
                        }
                    }

                    OCTDestroyCoordinateTransformation(hTransform);
                    OSRDestroySpatialReference(hTgt);
                    OSRDestroySpatialReference(hSrs);
                }
            }
        }
        catch (pdal::pdal_error &e)
        {
            LOGD << "PDAL Error: " << e.what();
            throw PDALException(e.what());
        }

        return true;
    }

    bool getEptInfo(const std::string &eptJson, PointCloudInfo &info, int polyBoundsSrs, int *span)
    {
        json j;
        try
        {

            auto contents = utils::readFile(eptJson);

            j = json::parse(utils::readFile(eptJson));
        }
        catch (json::exception &e)
        {
            LOGD << e.what();
            return false;
        }
        catch (const FSException &e)
        {
            LOGD << e.what();
            return false;
        }
        catch (const NetException &e)
        {
            LOGD << e.what();
            return false;
        }

        if (!j.contains("boundsConforming") || !j.contains("points") || !j.contains("schema") || !j.contains("span"))
        {
            LOGD << "Invalid EPT: " << eptJson;
            return false;
        }

        info.pointCount = j["points"];

        if (j.contains("srs") && j["srs"].contains("wkt"))
        {
            info.wktProjection = j["srs"]["wkt"];
        }
        else
        {
            info.wktProjection = "";
        }

        info.dimensions.clear();
        for (auto &dim : j["schema"])
        {
            if (dim.contains("name"))
            {
                info.dimensions.push_back(dim["name"]);
            }
        }

        if (span != nullptr)
        {
            *span = j["span"];
        }

        const double minx = j["boundsConforming"][0];
        const double miny = j["boundsConforming"][1];
        const double minz = j["boundsConforming"][2];
        const double maxx = j["boundsConforming"][3];
        const double maxy = j["boundsConforming"][4];
        const double maxz = j["boundsConforming"][5];

        info.bounds.clear();
        info.bounds.push_back(minx);
        info.bounds.push_back(miny);
        info.bounds.push_back(minz);
        info.bounds.push_back(maxx);
        info.bounds.push_back(maxy);
        info.bounds.push_back(maxz);

        if (info.wktProjection.empty())
        {
            // Nothing else to do
            LOGD << "WKT projection is empty";
            return true;
        }

        OGRSpatialReferenceH hSrs = OSRNewSpatialReference(nullptr);
        OGRSpatialReferenceH hTgt = OSRNewSpatialReference(nullptr);

        char *wkt = strdup(info.wktProjection.c_str());

        char* wktPointer = wkt;

        if (OSRImportFromWkt(hSrs, &wktPointer) != OGRERR_NONE)
        {
            free(wkt);
            OSRDestroySpatialReference(hTgt);
            OSRDestroySpatialReference(hSrs);
            throw GDALException("Cannot import spatial reference system " + info.wktProjection + ". Is PROJ available?");
        }
        free(wkt);
        OSRSetAxisMappingStrategy(hSrs, OAMS_TRADITIONAL_GIS_ORDER);

        OSRImportFromEPSG(hTgt, polyBoundsSrs);
        OGRCoordinateTransformationH hTransform = OCTNewCoordinateTransformation(hSrs, hTgt);

        double geoMinX = minx;
        double geoMinY = miny;
        double geoMinZ = minz;
        double geoMaxX = maxx;
        double geoMaxY = maxy;
        double geoMaxZ = maxz;

        const bool minSuccess = OCTTransform(hTransform, 1, &geoMinX, &geoMinY, &geoMinZ);
        const bool maxSuccess = OCTTransform(hTransform, 1, &geoMaxX, &geoMaxY, &geoMaxZ);
        info.polyBounds.clear();

        if (!minSuccess || !maxSuccess)
        {
            LOGD << "Cannot transform coordinates " << info.wktProjection << " to EPSG:" << std::to_string(polyBoundsSrs);
        }
        else
        {
            info.polyBounds.addPoint(geoMinY, geoMinX, geoMinZ);
            info.polyBounds.addPoint(geoMinY, geoMaxX, geoMinZ);
            info.polyBounds.addPoint(geoMaxY, geoMaxX, geoMinZ);
            info.polyBounds.addPoint(geoMaxY, geoMinX, geoMinZ);
            info.polyBounds.addPoint(geoMinY, geoMinX, geoMinZ);

            double centroidX = (minx + maxx) / 2.0;
            double centroidY = (miny + maxy) / 2.0;
            double centroidZ = minz;

            if (OCTTransform(hTransform, 1, &centroidX, &centroidY, &centroidZ))
            {
                info.centroid.clear();
                info.centroid.addPoint(centroidY, centroidX, centroidZ);
            }
            else
            {
                throw GDALException("Cannot transform coordinates " + std::to_string(centroidX) + ", " + std::to_string(centroidY) + " to EPSG:" + std::to_string(polyBoundsSrs));
            }
        }

        OCTDestroyCoordinateTransformation(hTransform);
        OSRDestroySpatialReference(hTgt);
        OSRDestroySpatialReference(hSrs);

        return true;
    }

    void buildEpt(const std::vector<std::string> &filenames, const std::string &outdir)
    {
        fs::path dest = outdir;
        fs::path tmpDir = dest / "tmp";
        io::assureFolderExists(tmpDir);

        untwine::Options options;
        for (const std::string &f : filenames)
        {
            if (!fs::exists(f))
                throw FSException(f + " does not exist");

            const EntryType type = fingerprint(f);
            if (type != PointCloud)
                throw InvalidArgsException(f + " is not a supported point cloud file");
        }

        std::vector<std::string> inputFiles;

        // Make sure these are LAS/LAZ. If it's PLY, we first need to convert
        // to LAS
        for (const auto &f : filenames)
        {
            auto p = io::Path(f);
            if (p.checkExtension({"ply"}))
            {
                std::string lasF = (tmpDir / Hash::strCRC64(f)).string() + ".las";
                LOGD << "Converting " << f << " to " << lasF;
                translateToLas(f, lasF);
                inputFiles.push_back(lasF);
            }
            else
            {
                inputFiles.push_back(f);
            }
        }

        for (const auto &f : inputFiles)
            options.inputFiles.push_back(f);

        options.tempDir = tmpDir.string();
        options.outputDir = dest.string();
        options.fileLimit = 10000000;
        options.progressFd = -1;
        options.stats = false;
        options.level = -1;

        io::assureFolderExists(dest);
        io::assureIsRemoved(dest / "ept.json");
        io::assureIsRemoved(dest / "ept-data");
        io::assureIsRemoved(dest / "ept-hierarchy");
        io::assureFolderExists(dest / "ept-data");
        io::assureFolderExists(dest / "ept-hierarchy");

        untwine::ProgressWriter progress(options.progressFd);

        try
        {
            untwine::BaseInfo common;

            untwine::epf::Epf preflight(common);
            preflight.run(options, progress);

            untwine::bu::BuPyramid builder(common);
            builder.run(options, progress);

            io::assureIsRemoved(tmpDir);
            io::assureIsRemoved(dest / "temp");
        }
        catch (const std::exception &e)
        {
            io::assureIsRemoved(tmpDir);
            io::assureIsRemoved(dest / "temp");

            throw UntwineException(e.what());
        }
    }

    json PointCloudInfo::toJSON()
    {
        json j;
        j["pointCount"] = pointCount;
        j["projection"] = wktProjection;
        j["dimensions"] = dimensions;

        return j;
    }

    // Iterates a point view and returns an array with normalized 8bit colors
    std::vector<PointColor> normalizeColors(std::shared_ptr<pdal::PointView> point_view)
    {
        std::vector<PointColor> result;

        bool normalize = false;
        for (pdal::PointId idx = 0; idx < point_view->size(); ++idx)
        {
            auto p = point_view->point(idx);
            uint16_t red = p.getFieldAs<uint16_t>(pdal::Dimension::Id::Red);
            uint16_t green = p.getFieldAs<uint16_t>(pdal::Dimension::Id::Green);
            uint16_t blue = p.getFieldAs<uint16_t>(pdal::Dimension::Id::Blue);

            if (red > 255 || green > 255 || blue > 255)
            {
                normalize = true;
                break;
            }
        }

        for (pdal::PointId idx = 0; idx < point_view->size(); ++idx)
        {
            auto p = point_view->point(idx);
            uint16_t red = p.getFieldAs<uint16_t>(pdal::Dimension::Id::Red);
            uint16_t green = p.getFieldAs<uint16_t>(pdal::Dimension::Id::Green);
            uint16_t blue = p.getFieldAs<uint16_t>(pdal::Dimension::Id::Blue);
            PointColor color;

            if (normalize)
            {
                color.r = red >> 8;
                color.g = green >> 8;
                color.b = blue >> 8;
            }
            else
            {
                color.r = static_cast<uint8_t>(red);
                color.g = static_cast<uint8_t>(green);
                color.b = static_cast<uint8_t>(blue);
            }

            result.push_back(color);
        }

        return result;
    }

    void translateToLas(const std::string &input, const std::string &outputLas)
    {
        if (!fs::exists(input))
            throw FSException(input + " does not exist");

        std::string driver = pdal::StageFactory::inferReaderDriver(input);
        if (driver.empty())
        {
            throw PDALException("Cannot infer reader driver for " + input);
        }

        try
        {
            pdal::StageFactory factory;
            pdal::Stage *reader = factory.createStage(driver);
            pdal::Options inOpts;
            inOpts.add("filename", input);
            reader->setOptions(inOpts);

            pdal::PointTable table;
            pdal::Options outLasOpts;
            outLasOpts.add("filename", outputLas);
            outLasOpts.add("minor_version", 2);
            outLasOpts.add("dataformat_id", 3);

            pdal::LasWriter writer;
            writer.setOptions(outLasOpts);
            writer.setInput(*reader);
            writer.prepare(table);
            writer.execute(table);
        }
        catch (pdal::pdal_error &e)
        {
            throw PDALException(e.what());
        }
    }

}
