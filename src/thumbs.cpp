/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <sstream>
#include <cstdlib>
#include <gdal_priv.h>
#include <gdal_utils.h>

#include "thumbs.h"

#include <coordstransformer.h>
#include <epttiler.h>
#include <pointcloud.h>
#include <tiler.h>

#include <Options.hpp>
#include <filters/ColorinterpFilter.hpp>
#include <io/EptReader.hpp>

#include "exceptions.h"
#include "hash.h"
#include "utils.h"
#include "userprofile.h"
#include "dbops.h"
#include "mio.h"

namespace ddb{

fs::path getThumbFromUserCache(const fs::path &imagePath, int thumbSize, bool forceRecreate){
    if (std::rand() % 1000 == 0) cleanupThumbsUserCache();
    if (!fs::exists(imagePath)) throw FSException(imagePath.string() + " does not exist");

    const fs::path outdir = UserProfile::get()->getThumbsDir(thumbSize);
    io::Path p = imagePath;
    const fs::path thumbPath = outdir / getThumbFilename(imagePath, p.getModifiedTime(), thumbSize);
    return generateThumb(imagePath, thumbSize, thumbPath, forceRecreate);
}

bool supportsThumbnails(EntryType type){
    return type == Image || type == GeoImage || type == GeoRaster;
}

void generateThumbs(const std::vector<std::string> &input, const fs::path &output, int thumbSize, bool useCrc){
    if (input.size() > 1) io::assureFolderExists(output);
    const bool outputIsFile = input.size() == 1 && io::Path(output).checkExtension({"jpg", "jpeg"});

    const std::vector<fs::path> filePaths = std::vector<fs::path>(input.begin(), input.end());

    for (auto &fp : filePaths){
        LOGD << "Parsing entry " << fp.string();

        const EntryType type = fingerprint(fp);
        io::Path p(fp);

        // NOTE: This check is looking pretty ugly, maybe move "ept.json" in a const?
        if (supportsThumbnails(type) || fp.filename() == "ept.json") {
            fs::path outImagePath;
            if (useCrc){
                outImagePath = output / getThumbFilename(fp, p.getModifiedTime(), thumbSize);
            }else if (outputIsFile){
                outImagePath = output;
            }else{
                outImagePath = output / fs::path(fp).replace_extension(".jpg").filename();
            }
            std::cout << generateThumb(fp, thumbSize, outImagePath, true).string() << std::endl;
        }else{
            LOGD << "Skipping " << fp;
        }
    }
}


fs::path getThumbFilename(const fs::path &imagePath, time_t modifiedTime, int thumbSize){
    // Thumbnails are JPG files idenfitied by:
    // CRC64(imagePath + "*" + modifiedTime + "*" + thumbSize).jpg
    std::ostringstream os;
    os << imagePath.string() << "*" << modifiedTime << "*" << thumbSize;
    return fs::path(Hash::strCRC64(os.str()) + ".jpg");
}

void generateImageThumb(const fs::path& imagePath, int thumbSize, const fs::path& outImagePath) {

    // Compute image with GDAL otherwise
    GDALDatasetH hSrcDataset = GDALOpen(imagePath.string().c_str(), GA_ReadOnly);

    if (!hSrcDataset)
        throw GDALException("Cannot open " + imagePath.string() + " for reading");

    const int width = GDALGetRasterXSize(hSrcDataset);
    const int height = GDALGetRasterYSize(hSrcDataset);
    int targetWidth;
    int targetHeight;

    if (width > height){
        targetWidth = thumbSize;
        targetHeight = static_cast<int>((static_cast<float>(thumbSize) / static_cast<float>(width)) * static_cast<float>(height));
    } else {
        targetHeight = thumbSize;
        targetWidth = static_cast<int>((static_cast<float>(thumbSize) / static_cast<float>(height)) * static_cast<float>(width));
    }

    char** targs = nullptr;
    targs = CSLAddString(targs, "-outsize");
    targs = CSLAddString(targs, std::to_string(targetWidth).c_str());
    targs = CSLAddString(targs, std::to_string(targetHeight).c_str());

    targs = CSLAddString(targs, "-ot");
    targs = CSLAddString(targs, "Byte");

    targs = CSLAddString(targs, "-scale");

    targs = CSLAddString(targs, "-co");
    targs = CSLAddString(targs, "WRITE_EXIF_METADATA=NO");

    // Max 3 bands + alpha
    if (GDALGetRasterCount(hSrcDataset) > 4){
        targs = CSLAddString(targs, "-b");
        targs = CSLAddString(targs, "1");
        targs = CSLAddString(targs, "-b");
        targs = CSLAddString(targs, "2");
        targs = CSLAddString(targs, "-b");
        targs = CSLAddString(targs, "3");
    }

    CPLSetConfigOption("GDAL_PAM_ENABLED", "NO"); // avoid aux files for PNG tiles
    CPLSetConfigOption("GDAL_ALLOW_LARGE_LIBJPEG_MEM_ALLOC", "YES"); // Avoids ERROR 6: Reading this image would require libjpeg to allocate at least 107811081 bytes

    GDALTranslateOptions* psOptions = GDALTranslateOptionsNew(targs, nullptr);
    CSLDestroy(targs);
    GDALDatasetH hNewDataset = GDALTranslate(outImagePath.string().c_str(),
                                             hSrcDataset,
                                             psOptions,
                                             nullptr);
    GDALTranslateOptionsFree(psOptions);

    GDALClose(hNewDataset);
    GDALClose(hSrcDataset);

}

void drawCircle(uint8_t *buffer, uint8_t *alpha, int px, int py,
                          int radius, uint8_t r, uint8_t g, uint8_t b, int tileSize, int wSize) {
    int r2 = radius * radius;
    int area = r2 << 2;
    int rr = radius << 1;

    for (int i = 0; i < area; i++) {
        int tx = (i % rr) - radius;
        int ty = (i / rr) - radius;
        if (tx * tx + ty * ty <= r2) {
            int dx = px + tx;
            int dy = py + ty;
            if (dx >= 0 && dx < tileSize && dy >= 0 && dy < tileSize) {
                buffer[dy * tileSize + dx + wSize * 0] = r;
                buffer[dy * tileSize + dx + wSize * 1] = g;
                buffer[dy * tileSize + dx + wSize * 2] = b;
                alpha[dy * tileSize + dx] = 255;
            }
        }
    }
}

void generatePointCloudThumb(const fs::path &eptPath, int thumbSize,
                        const fs::path &outImagePath) {

    LOGD << "Generating point cloud thumb";

    try {

        PointCloudInfo eptInfo;
        
        // Open EPT
        int span;
        if (!getEptInfo(eptPath.string(), eptInfo, 3857, &span)){
            throw InvalidArgsException("Cannot get EPT info for " +
                                    eptPath.string());
        }

        const auto oMinX = eptInfo.polyBounds.getPoint(0).y;
        const auto oMaxX = eptInfo.polyBounds.getPoint(2).y;
        const auto oMaxY = eptInfo.polyBounds.getPoint(2).x;
        const auto oMinY = eptInfo.polyBounds.getPoint(0).x;

        LOGD << "Bounds (output SRS): (" << oMinX << "; " << oMinY << ") - ("
            << oMaxX << "; " << oMaxY << ")";

        const auto tileSize = thumbSize;

        GlobalMercator mercator(tileSize);

        // Max/min zoom level
        const auto tMinZ = mercator.zoomForLength(std::min(oMaxX - oMinX, oMaxY - oMinY));
        const auto tMaxZ = tMinZ + static_cast<int>(std::round(std::log(static_cast<double>(span) / 4.0) / std::log(2)));

        LOGD << "MinZ: " << tMinZ;
        LOGD << "MaxZ: " << tMaxZ;

        const auto hasColors = std::find(eptInfo.dimensions.begin(), eptInfo.dimensions.end(), "Red") != eptInfo.dimensions.end() &&
                        std::find(eptInfo.dimensions.begin(), eptInfo.dimensions.end(), "Green") != eptInfo.dimensions.end() &&
                        std::find(eptInfo.dimensions.begin(), eptInfo.dimensions.end(), "Blue") != eptInfo.dimensions.end();
        LOGD << "Has colors: " << (hasColors ? "true" : "false");

    #ifdef _WIN32
        const fs::path caBundlePath = io::getDataPath("curl-ca-bundle.crt");
        if (!caBundlePath.empty()) {
            LOGD << "ARBITRER CA Bundle: " << caBundlePath.string();
            std::stringstream ss;
            ss << "ARBITER_CA_INFO=" << caBundlePath.string();
            if (_putenv(ss.str().c_str()) != 0) {
                LOGD << "Cannot set ARBITER_CA_INFO";
            }
        }
    #endif

        const auto tz = tMinZ;

        // -----------------------------------------------

        BoundingBox b(mercator.metersToTile(oMinX, oMinY, tz),
                    mercator.metersToTile(oMaxX, oMaxY, tz));

        // Crop tiles extending world limits (+-180,+-90)
        b.min.x = std::max<int>(0, b.min.x);
        b.max.x = std::min<int>(static_cast<int>(std::pow(2, tz) - 1), b.max.x);

        LOGD << "MinMaxCoordsForZ(" << tz << ") = (" << b.min.x << ", "
            << b.min.y << "), (" << b.max.x << ", " << b.max.y << ")";

        // Get bounds of tile (3857), convert to EPT CRS
        auto tileBounds = mercator.tileBounds(oMinX, oMinY, tz);
        auto bounds = tileBounds;

        // -----------------------------------------------

        pdal::Options eptOpts;
        eptOpts.add("filename", eptPath);

        std::stringstream ss;
        ss << std::setprecision(14) << "([" << b.min.x << "," << b.min.y
        << "], "
        << "[" << b.max.x << "," << b.max.y << "])";
        eptOpts.add("bounds", ss.str());
        LOGD << "EPT bounds: " << ss.str();

        double resolution = mercator.resolution(tz - 2);
        eptOpts.add("resolution", resolution);
        LOGD << "EPT resolution: " << resolution;

        std::unique_ptr<pdal::EptReader> eptReader =
            std::make_unique<pdal::EptReader>();
        pdal::Stage *main = eptReader.get();
        eptReader->setOptions(eptOpts);
        LOGD << "Options set";

        // -----------------------------------------------------------------

        std::unique_ptr<pdal::ColorinterpFilter> colorFilter;
        if (!hasColors) {
            colorFilter.reset(new pdal::ColorinterpFilter());

            // Add ramp filter
            LOGD << "Adding ramp filter (" << eptInfo.bounds[2] << ", "
                << eptInfo.bounds[5] << ")";

            pdal::Options cfOpts;
            cfOpts.add("ramp", "pestel_shades");
            cfOpts.add("minimum", eptInfo.bounds[2]);
            cfOpts.add("maximum", eptInfo.bounds[5]);
            colorFilter->setOptions(cfOpts);
            colorFilter->setInput(*eptReader);
            main = colorFilter.get();
        }

        pdal::PointTable table;
        main->prepare(table);
        pdal::PointViewSet point_view_set;

        LOGD << "PointTable prepared";

        try {
            point_view_set = main->execute(table);
        } catch (const pdal::pdal_error &e) {
            throw PDALException(e.what());
        }

        pdal::PointViewPtr point_view = *point_view_set.begin();
        pdal::Dimension::IdList dims = point_view->dims();

        const auto wSize = tileSize * tileSize;

        const int nBands = 3;
        const int bufSize = GDALGetDataTypeSizeBytes(GDT_Byte) * wSize;
        std::unique_ptr<uint8_t> buffer(new uint8_t[bufSize * nBands]);
        std::unique_ptr<uint8_t> alphaBuffer(new uint8_t[bufSize]);
        std::unique_ptr<float> zBuffer(new float[bufSize]);

        memset(buffer.get(), 0, bufSize * nBands);
        memset(alphaBuffer.get(), 0, bufSize);

        for (int i = 0; i < wSize; i++) {
            zBuffer.get()[i] = -99999.0;
        }

        LOGD << "Fetched " << point_view->size() << " points";

        const double tileScaleW = tileSize / (tileBounds.max.x - tileBounds.min.x);
        const double tileScaleH = tileSize / (tileBounds.max.y - tileBounds.min.y);
        CoordsTransformer ict(eptInfo.wktProjection, 3857);

        for (pdal::PointId idx = 0; idx < point_view->size(); ++idx) {
            auto p = point_view->point(idx);
            double x = p.getFieldAs<double>(pdal::Dimension::Id::X);
            double y = p.getFieldAs<double>(pdal::Dimension::Id::Y);
            double z = p.getFieldAs<double>(pdal::Dimension::Id::Z);

            ict.transform(&x, &y);

            // Map projected coordinates to local PNG coordinates
            int px = std::round((x - tileBounds.min.x) * tileScaleW);
            int py = tileSize - 1 - std::round((y - tileBounds.min.y) * tileScaleH);

            if (px >= 0 && px < tileSize && py >= 0 && py < tileSize) {
                // Within bounds
                uint8_t red = p.getFieldAs<uint8_t>(pdal::Dimension::Id::Red);
                uint8_t green = p.getFieldAs<uint8_t>(pdal::Dimension::Id::Green);
                uint8_t blue = p.getFieldAs<uint8_t>(pdal::Dimension::Id::Blue);

                if (zBuffer.get()[py * tileSize + px] < z) {
                    zBuffer.get()[py * tileSize + px] = z;
                    drawCircle(buffer.get(), alphaBuffer.get(), px, py, 2, red,
                            green, blue, tileSize, wSize);
                }
            }
        }

        GDALDriverH memDrv = GDALGetDriverByName("MEM");
        if (memDrv == nullptr) throw GDALException("Cannot create MEM driver");
        GDALDriverH pngDrv = GDALGetDriverByName("PNG");
        if (pngDrv == nullptr) throw GDALException("Cannot create PNG driver");

        // Need to create in-memory dataset
        // (PNG driver does not have Create() method)
        const GDALDatasetH dsTile = GDALCreate(memDrv, "", tileSize, tileSize,
                                            nBands + 1, GDT_Byte, nullptr);
        if (dsTile == nullptr) throw GDALException("Cannot create dsTile");

        if (GDALDatasetRasterIO(dsTile, GF_Write, 0, 0, tileSize, tileSize,
                                buffer.get(), tileSize, tileSize, GDT_Byte, nBands,
                                nullptr, 0, 0, 0) != CE_None) {
            throw GDALException("Cannot write tile data");
        }

        const GDALRasterBandH tileAlphaBand = GDALGetRasterBand(dsTile, nBands + 1);
        GDALSetRasterColorInterpretation(tileAlphaBand, GCI_AlphaBand);

        if (GDALRasterIO(tileAlphaBand, GF_Write, 0, 0, tileSize, tileSize,
                        alphaBuffer.get(), tileSize, tileSize, GDT_Byte, 0,
                        0) != CE_None) {
            throw GDALException("Cannot write tile alpha data");
        }

        const GDALDatasetH outDs = GDALCreateCopy(pngDrv, outImagePath.string().c_str(), dsTile,
                                                FALSE, nullptr, nullptr, nullptr);
        if (outDs == nullptr)
            throw GDALException("Cannot create output dataset " +
                                outImagePath.string());

        GDALClose(outDs);
        GDALClose(dsTile);

    } catch(const std::exception& e) {
        LOGD << e.what();
    } catch(const std::string& e) {
        LOGD << e;
    }

}

// imagePath can be either absolute or relative and it's up to the user to
// invoke the function properly as to avoid conflicts with relative paths
fs::path generateThumb(const fs::path &imagePath, int thumbSize, const fs::path &outImagePath, bool forceRecreate){
    if (!exists(imagePath)) throw FSException(imagePath.string() + " does not exist");

    // Check existance of thumbnail, return if exists
    if (exists(outImagePath) && !forceRecreate){
        return outImagePath;
    }

    LOGD << "ImagePath = " << imagePath;
    LOGD << "OutImagePath = " << outImagePath;
    LOGD << "Size = " << thumbSize;

    if (imagePath.filename() == "ept.json")
        generatePointCloudThumb(imagePath, thumbSize, outImagePath);
    else
        generateImageThumb(imagePath, thumbSize, outImagePath);

    return outImagePath;
}

void cleanupThumbsUserCache(){
    LOGD << "Cleaning up thumbs user cache";

    const time_t threshold = utils::currentUnixTimestamp() - 60 * 60 * 24 * 5; // 5 days
    const fs::path thumbsDir = UserProfile::get()->getThumbsDir();
    std::vector<fs::path> cleanupDirs;

    // Iterate size directories
    for(auto sd = fs::recursive_directory_iterator(thumbsDir);
            sd != fs::recursive_directory_iterator();
            ++sd ){
        fs::path sizeDir = sd->path();
        if (is_directory(sizeDir)){
            for(auto t = fs::recursive_directory_iterator(sizeDir);
                    t != fs::recursive_directory_iterator();
                    ++t ){
                fs::path thumb = t->path();
                if (io::Path(thumb).getModifiedTime() < threshold){
                    if (fs::remove(thumb)) LOGD << "Cleaned " << thumb.string();
                    else LOGD << "Cannot clean " << thumb.string();
                }
            }

            if (is_empty(sizeDir)){
                // Remove directory too
                cleanupDirs.push_back(sizeDir);
            }
        }
    }

    for (auto &d : cleanupDirs){
        if (fs::remove(d)) LOGD << "Cleaned " << d.string();
        else LOGD << "Cannot clean " << d.string();
    }
}

}
