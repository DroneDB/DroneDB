/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "tiler.h"

#include <mutex>
#include <memory>
#include <vector>

#include "entry.h"
#include "exceptions.h"
#include "geoproject.h"
#include "hash.h"
#include "logger.h"
#include "mio.h"
#include "userprofile.h"

namespace ddb {

bool Tiler::hasGeoreference(const GDALDatasetH &dataset) {
    double geo[6] = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    if (GDALGetGeoTransform(dataset, geo) != CE_None)
        throw GDALException("Cannot fetch geotransform in hasGeoreference");

    return (geo[0] != 0.0 || geo[1] != 1.0 || geo[2] != 0.0 || geo[3] != 0.0 ||
            geo[4] != 0.0 || geo[5] != 1.0) ||
           GDALGetGCPCount(dataset) != 0;
}

bool Tiler::sameProjection(const OGRSpatialReferenceH &a,
                           const OGRSpatialReferenceH &b) {
    char *aProj;
    char *bProj;

    if (OSRExportToProj4(a, &aProj) != CE_None)
        throw GDALException("Cannot export proj4");
    if (OSRExportToProj4(b, &bProj) != CE_None)
        throw GDALException("Cannot export proj4");

    return std::string(aProj) == std::string(bProj);
}

int Tiler::dataBandsCount(const GDALDatasetH &dataset) {
    const GDALRasterBandH raster = GDALGetRasterBand(dataset, 1);
    const GDALRasterBandH alphaBand = GDALGetMaskBand(raster);
    const int bandsCount = GDALGetRasterCount(dataset);

    if (GDALGetMaskFlags(alphaBand) & GMF_ALPHA || bandsCount == 4 ||
        bandsCount == 2) {
        return bandsCount - 1;
    }

    return bandsCount;
}

std::string Tiler::getTilePath(int z, int x, int y, bool createIfNotExists) {
    // TODO: retina tiles support?
    const fs::path dir = outputFolder / std::to_string(z) / std::to_string(x);
    if (createIfNotExists && !fs::exists(dir)) {
        io::createDirectories(dir);
    }

    fs::path p = dir / fs::path(std::to_string(y) + ".png");
    return p.string();
}

Tiler::Tiler(const std::string &geotiffPath, const std::string &outputFolder,
             int tileSize, bool tms)
    : geotiffPath(geotiffPath),
      outputFolder(outputFolder),
      tileSize(tileSize),
      tms(tms),
      mercator(GlobalMercator(tileSize)) {
    if (!fs::exists(geotiffPath))
        throw FSException(geotiffPath + " does not exists");
    if (tileSize <= 0 ||
        std::ceil(std::log2(tileSize) != std::floor(std::log2(tileSize))))
        throw GDALException("Tile size must be a power of 2 greater than 0");

    if (!fs::exists(outputFolder)) {
        // Try to create
        io::createDirectories(outputFolder);
    }

    pngDrv = GDALGetDriverByName("PNG");
    if (pngDrv == nullptr) throw GDALException("Cannot create PNG driver");
    memDrv = GDALGetDriverByName("MEM");
    if (memDrv == nullptr) throw GDALException("Cannot create MEM driver");

    inputDataset = GDALOpen(geotiffPath.c_str(), GA_ReadOnly);
    if (inputDataset == nullptr)
        throw GDALException("Cannot open " + geotiffPath);

    rasterCount = GDALGetRasterCount(inputDataset);
    if (rasterCount == 0)
        throw GDALException("No raster bands found in " + geotiffPath);

    // Extract no data values
    //    std::vector<double> inNodata;
    //    int success;
    //    for (int i = 0; i < rasterCount; i++){
    //        GDALRasterBandH band = GDALGetRasterBand(inputDataset, i + 1);
    //        double nodata = GDALGetRasterNoDataValue(band, &success);
    //        if (success) inNodata.push_back(nodata);
    //    }

    // Extract input SRS
    const OGRSpatialReferenceH inputSrs = OSRNewSpatialReference(nullptr);
    std::string inputSrsWkt;
    if (GDALGetProjectionRef(inputDataset) != nullptr) {
        inputSrsWkt = GDALGetProjectionRef(inputDataset);
    } else if (GDALGetGCPCount(inputDataset) > 0) {
        inputSrsWkt = GDALGetGCPProjection(inputDataset);
    } else {
        throw GDALException("No projection found in " + geotiffPath);
    }

    char *wktp = const_cast<char *>(inputSrsWkt.c_str());
    if (OSRImportFromWkt(inputSrs, &wktp) != OGRERR_NONE) {
        throw GDALException("Cannot read spatial reference system for " +
                            geotiffPath + ". Is PROJ available?");
    }

    // Setup output SRS
    const OGRSpatialReferenceH outputSrs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(outputSrs, 3857);  // TODO: support for geodetic?

    if (!hasGeoreference(inputDataset))
        throw GDALException(geotiffPath + " is not georeferenced.");

    // Check if we need to reproject
    if (!sameProjection(inputSrs, outputSrs)) {
        origDataset = inputDataset;
        inputDataset = createWarpedVRT(inputDataset, outputSrs);
    }

    OSRDestroySpatialReference(inputSrs);
    OSRDestroySpatialReference(outputSrs);

    // TODO: nodata?
    // if (inNodata.size() > 0){
    //        update_no_data_values
    //}

    // warped_input_dataset = inputDataset
    nBands = dataBandsCount(inputDataset);
    //    bool hasAlpha = nBands < GDALGetRasterCount(inputDataset);

    double outGt[6];
    if (GDALGetGeoTransform(inputDataset, outGt) != CE_None)
        throw GDALException("Cannot fetch geotransform outGt");

    oMinX = outGt[0];
    oMaxX = outGt[0] + GDALGetRasterXSize(inputDataset) * outGt[1];
    oMaxY = outGt[3];
    oMinY = outGt[3] - GDALGetRasterYSize(inputDataset) * outGt[1];

    LOGD << "Bounds (output SRS): " << oMinX << "," << oMinY << "," << oMaxX
         << "," << oMaxY;

    // Max/min zoom level
    tMaxZ = mercator.zoomForPixelSize(outGt[1]);
    tMinZ =
        mercator.zoomForPixelSize(outGt[1] *
                                  std::max(GDALGetRasterXSize(inputDataset),
                                           GDALGetRasterYSize(inputDataset)) /
                                  256.0);

    LOGD << "MinZ: " << tMinZ;
    LOGD << "MaxZ: " << tMaxZ;
    LOGD << "Num bands: " << nBands;

}

Tiler::~Tiler() {
    if (inputDataset) GDALClose(inputDataset);
    if (origDataset) GDALClose(origDataset);
}

std::string Tiler::tile(int tz, int tx, int ty) {
    std::string tilePath = getTilePath(tz, tx, ty, true);

    if (tms) {
        ty = tmsToXYZ(ty, tz);
        LOGD << "TY: " << ty;
    }

    BoundingBox<Projected2Di> tMinMax = getMinMaxCoordsForZ(tz);
    if (!tMinMax.contains(tx, ty)) throw GDALException("Out of bounds");

    // Need to create in-memory dataset
    // (PNG driver does not have Create() method)
    const GDALDatasetH dsTile = GDALCreate(memDrv, "", tileSize, tileSize, nBands + 1,
                                           GDT_Byte, nullptr);
    if (dsTile == nullptr) throw GDALException("Cannot create dsTile");

    BoundingBox<Projected2D> b = mercator.tileBounds(tx, ty, tz);

    GQResult g = geoQuery(inputDataset, b.min.x, b.max.y, b.max.x, b.min.y);
    // int nativeSize = g.w.x + g.w.xsize;
    const int querySize = tileSize;  // TODO: you will need to change this for
                               // interpolations other than NN
    g = geoQuery(inputDataset, b.min.x, b.max.y, b.max.x, b.min.y, querySize);

    LOGD << "GeoQuery: " << g.r.x << "," << g.r.y << "|" << g.r.xsize << "x"
         << g.r.ysize << "|" << g.w.x << "," << g.w.y << "|" << g.w.xsize << "x"
         << g.w.ysize;

    if (g.r.xsize != 0 && g.r.ysize != 0 && g.w.xsize != 0 && g.w.ysize != 0) {
        const GDALDataType type =
            GDALGetRasterDataType(GDALGetRasterBand(inputDataset, 1));

        const size_t wSize = g.w.xsize * g.w.ysize;
        char *buffer =
            new char[GDALGetDataTypeSizeBytes(type) * nBands * wSize];

        if (GDALDatasetRasterIO(inputDataset, GF_Read, g.r.x, g.r.y, g.r.xsize,
                                g.r.ysize, buffer, g.w.xsize, g.w.ysize, type,
                                nBands, nullptr, 0, 0, 0) != CE_None) {
            throw GDALException("Cannot read input dataset window");
        }

        // Rescale if needed
        // We currently don't rescale byte datasets
        // TODO: allow people to specify rescale values

        if (type != GDT_Byte && type != GDT_Unknown) {
            for (int i = 0; i < nBands; i++) {
                const GDALRasterBandH hBand = GDALGetRasterBand(inputDataset, i + 1);
                char *wBuf = (buffer + wSize * i);

                switch (type) {
                    case GDT_Byte:
                        rescale<uint8_t>(hBand, wBuf, wSize);
                        break;
                    case GDT_UInt16:
                        rescale<uint16_t>(hBand, wBuf, wSize);
                        break;
                    case GDT_Int16:
                        rescale<int16_t>(hBand, wBuf, wSize);
                        break;
                    case GDT_UInt32:
                        rescale<uint32_t>(hBand, wBuf, wSize);
                        break;
                    case GDT_Int32:
                        rescale<int32_t>(hBand, wBuf, wSize);
                        break;
                    case GDT_Float32:
                        rescale<float>(hBand, wBuf, wSize);
                        break;
                    case GDT_Float64:
                        rescale<double>(hBand, wBuf, wSize);
                        break;
                    default:
                        break;
                }
            }
        }

        const GDALRasterBandH raster = GDALGetRasterBand(inputDataset, 1);
        const GDALRasterBandH alphaBand = GDALGetMaskBand(raster);

        char *alphaBuffer =
            new char[GDALGetDataTypeSizeBytes(GDT_Byte) * wSize];
        if (GDALRasterIO(alphaBand, GF_Read, g.r.x, g.r.y, g.r.xsize, g.r.ysize,
                         alphaBuffer, g.w.xsize, g.w.ysize, GDT_Byte, 0,
                         0) != CE_None) {
            throw GDALException("Cannot read input dataset alpha window");
        }

        // Write data

        if (tileSize == querySize) {
            if (GDALDatasetRasterIO(dsTile, GF_Write, g.w.x, g.w.y, g.w.xsize,
                                    g.w.ysize, buffer, g.w.xsize, g.w.ysize,
                                    type, nBands, nullptr, 0, 0,
                                    0) != CE_None) {
                throw GDALException("Cannot write tile data");
            }

            LOGD << "Wrote tile data";

            const GDALRasterBandH tileAlphaBand =
                GDALGetRasterBand(dsTile, nBands + 1);
            GDALSetRasterColorInterpretation(tileAlphaBand, GCI_AlphaBand);

            if (GDALRasterIO(tileAlphaBand, GF_Write, g.w.x, g.w.y, g.w.xsize,
                             g.w.ysize, alphaBuffer, g.w.xsize, g.w.ysize,
                             GDT_Byte, 0, 0) != CE_None) {
                throw GDALException("Cannot write tile alpha data");
            }

            LOGD << "Wrote tile alpha";
        } else {
            // TODO: readraster query in memory scaled to tilesize
            throw GDALException("Not implemented");
        }

        delete[] buffer;
        delete[] alphaBuffer;
    } else {
        throw GDALException("Geoquery out of bounds");
    }

    const GDALDatasetH outDs = GDALCreateCopy(pngDrv, tilePath.c_str(), dsTile, FALSE,
                                              nullptr, nullptr, nullptr);
    if (outDs == nullptr)
        throw GDALException("Cannot create output dataset " + tilePath);

    GDALClose(dsTile);
    GDALClose(outDs);

    return tilePath;
}

std::string Tiler::tile(const TileInfo &t) { return tile(t.tz, t.tx, t.ty); }

std::vector<TileInfo> Tiler::getTilesForZoomLevel(int tz) const {
    std::vector<TileInfo> result;
    const BoundingBox<Projected2Di> bounds = getMinMaxCoordsForZ(tz);

    for (int ty = bounds.min.y; ty < bounds.max.y + 1; ty++) {
        for (int tx = bounds.min.x; tx < bounds.max.x + 1; tx++) {
            LOGD << tx << " " << ty << " " << tz;
            result.emplace_back(tx, tms ? xyzToTMS(ty, tz) : ty, tz);
        }
    }

    return result;
}

BoundingBox<int> Tiler::getMinMaxZ() const { return BoundingBox(tMinZ, tMaxZ); }

BoundingBox<Projected2Di> Tiler::getMinMaxCoordsForZ(int tz) const {
    BoundingBox<Projected2Di> b(mercator.metersToTile(oMinX, oMinY, tz),
                                mercator.metersToTile(oMaxX, oMaxY, tz));

    // Crop tiles extending world limits (+-180,+-90)
    b.min.x = std::max<int>(0, b.min.x);
    b.max.x = std::min<int>(static_cast<int>(std::pow(2, tz - 1)), b.max.x);

    // TODO: figure this out (TMS vs. XYZ)
    //    b.min.y = std::max<double>(0, b.min.y);
    //    b.max.y = std::min<double>(std::pow(2, tz - 1), b.max.y);

    return b;
}

BoundingBox<int> TilerHelper::parseZRange(const std::string &zRange) {
    BoundingBox<int> r;

    const std::size_t dashPos = zRange.find('-');
    if (dashPos != std::string::npos) {
        r.min = std::stoi(zRange.substr(0, dashPos));
        r.max = std::stoi(zRange.substr(dashPos + 1, zRange.length() - 1));
        if (r.min > r.max) {
            std::swap(r.min, r.max);
        }
    } else {
        r.min = r.max = std::stoi(zRange);
    }

    return r;
}

fs::path TilerHelper::getCacheFolderName(const fs::path &tileablePath,
                                         time_t modifiedTime, int tileSize) {
    std::ostringstream os;
    os << tileablePath.string() << "*" << modifiedTime << "*" << tileSize;
    return Hash::strCRC64(os.str());
}

fs::path TilerHelper::getFromUserCache(const fs::path &tileablePath, int tz,
                                       int tx, int ty, int tileSize, bool tms,
                                       bool forceRecreate) {
    if (std::rand() % 1000 == 0) cleanupUserCache();
    if (!fs::exists(tileablePath))
        throw FSException(tileablePath.string() + " does not exist");

    const time_t modifiedTime = io::Path(tileablePath).getModifiedTime();
    const fs::path tileCacheFolder =
        UserProfile::get()->getTilesDir() /
        getCacheFolderName(tileablePath, modifiedTime, tileSize);
    fs::path outputFile = tileCacheFolder / std::to_string(tz) /
                          std::to_string(tx) / (std::to_string(ty) + ".png");

    // Cache hit
    if (fs::exists(outputFile) && !forceRecreate) {
        return outputFile;
    }

    const fs::path fileToTile = toGeoTIFF(tileablePath, tileSize, forceRecreate,
                                          (tileCacheFolder / "geoprojected.tif"));
    Tiler t(fileToTile.string(), tileCacheFolder.string(), tileSize, tms);
    return t.tile(tz, tx, ty);
}

std::mutex geoprojectMutex;

fs::path TilerHelper::toGeoTIFF(const fs::path &tileablePath, int tileSize,
                                bool forceRecreate,
                                const fs::path &outputGeotiff) {
    const EntryType type = fingerprint(tileablePath);

    if (type == EntryType::GeoRaster) {
        // Georasters can be tiled directly
        return tileablePath;
    } else {
        fs::path outputPath = outputGeotiff;

        if (outputGeotiff.empty()) {
            // Store in user cache if user doesn't specify a preference
            if (std::rand() % 1000 == 0) cleanupUserCache();
            const time_t modifiedTime = io::Path(tileablePath).getModifiedTime();
            const fs::path tileCacheFolder =
                UserProfile::get()->getTilesDir() /
                getCacheFolderName(tileablePath, modifiedTime, tileSize);
            io::assureFolderExists(tileCacheFolder);

            outputPath = tileCacheFolder / "geoprojected.tif";
        } else {
            // Just make sure the parent path exists
            io::assureFolderExists(outputGeotiff.parent_path());
        }

        // We need to (attempt) to geoproject the file first
        if (!fs::exists(outputPath) || forceRecreate) {
            // Multiple processes could be generating the geoprojected
            // file at the same time, so we place a lock
            std::lock_guard<std::mutex> guard(geoprojectMutex);

            // Recheck is needed for other processes that might have generated
            // the file
            if (!fs::exists(outputPath)){
                ddb::geoProject({tileablePath.string()}, outputPath.string(),
                                "100%", true);
            }
        }

        return outputPath;
    }
}

void TilerHelper::cleanupUserCache() {
    LOGD << "Cleaning up tiles user cache";

    const time_t threshold =
        utils::currentUnixTimestamp() - 60 * 60 * 24 * 5;  // 5 days
    const fs::path tilesDir = UserProfile::get()->getTilesDir();

    // Iterate directories
    for (auto d = fs::recursive_directory_iterator(tilesDir);
         d != fs::recursive_directory_iterator(); ++d) {
        fs::path dir = d->path();
        if (fs::is_directory(dir)) {
            if (io::Path(dir).getModifiedTime() < threshold) {
                if (fs::remove_all(dir))
                    LOGD << "Cleaned " << dir.string();
                else
                    LOGD << "Cannot clean " << dir.string();
            }
        }
    }
}

void TilerHelper::runTiler(Tiler &tiler, std::ostream &output,
                           const std::string &format, const std::string &zRange,
                           const std::string &x, const std::string &y) {
    BoundingBox<int> zb;
    if (zRange == "auto") {
        zb = tiler.getMinMaxZ();
    } else {
        zb = parseZRange(zRange);
    }

    const bool json = format == "json";

    if (json) {
        output << "[";
    }

    for (int z = zb.min; z <= zb.max; z++) {
        if (x != "auto" && y != "auto") {
            // Just one tile
            if (json) output << "\"";
            output << tiler.tile(z, std::stoi(x), std::stoi(y));
            if (json)
                output << "\"";
            else
                output << std::endl;
        } else {
            // All tiles
            std::vector<ddb::TileInfo> tiles = tiler.getTilesForZoomLevel(z);
            for (auto &t : tiles) {
                if (json) output << "\"";

                LOGD << "Tiling " << t.tx << " " << t.ty << " " << t.tz;
                output << tiler.tile(t);

                if (json) {
                    output << "\"";
                    if (&t != &tiles[tiles.size() - 1]) output << ",";
                } else {
                    output << std::endl;
                }
            }
        }
    }

    if (json) {
        output << "]";
    }
}

template <typename T>
void Tiler::rescale(GDALRasterBandH hBand, char *buffer, size_t bufsize) {
    double minmax[2];
    GDALComputeRasterMinMax(hBand, TRUE, minmax);

    // Avoid divide by zero
    if (minmax[0] == minmax[1]) minmax[1] += 0.1;

    LOGD << "Min: " << minmax[0] << " | Max: " << minmax[1];

    // Can still happen according to GDAL for very large values
    if (minmax[0] == minmax[1])
        throw GDALException(
            "Cannot scale values due to source min/max being equal");

    double deltamm = minmax[1] - minmax[0];
    T *ptr = reinterpret_cast<T *>(buffer);

    for (size_t i = 0; i < bufsize; i++) {
        ptr[i] = static_cast<T>(((ptr[i] - minmax[0]) / deltamm) * 255.0);
    }
}

GDALDatasetH Tiler::createWarpedVRT(const GDALDatasetH &src,
                                    const OGRSpatialReferenceH &srs,
                                    GDALResampleAlg resampling) {
    // GDALDriverH vrtDrv = GDALGetDriverByName( "VRT" );
    // if (vrtDrv == nullptr) throw GDALException("Cannot create VRT driver");

    // std::string vrtFilename = "/vsimem/" + uuidv4() + ".vrt";
    // GDALDatasetH vrt = GDALCreateCopy(vrtDrv, vrtFilename.c_str(), src,
    // FALSE, nullptr, nullptr, nullptr);

    char *dstWkt;
    if (OSRExportToWkt(srs, &dstWkt) != OGRERR_NONE)
        throw GDALException("Cannot export dst WKT " + geotiffPath +
                            ". Is PROJ available?");
    const char *srcWkt = GDALGetProjectionRef(src);

    const GDALDatasetH warpedVrt = GDALAutoCreateWarpedVRT(
        src, srcWkt, dstWkt, resampling, 0.001, nullptr);
    if (warpedVrt == nullptr) throw GDALException("Cannot create warped VRT");

    return warpedVrt;
}

GQResult Tiler::geoQuery(GDALDatasetH ds, double ulx, double uly, double lrx,
                         double lry, int querySize) {
    GQResult o;
    double geo[6];
    if (GDALGetGeoTransform(ds, geo) != CE_None)
        throw GDALException("Cannot fetch geotransform geo");

    o.r.x = static_cast<int>((ulx - geo[0]) / geo[1] + 0.001);
    o.r.y = static_cast<int>((uly - geo[3]) / geo[5] + 0.001);
    o.r.xsize = static_cast<int>((lrx - ulx) / geo[1] + 0.5);
    o.r.ysize = static_cast<int>((lry - uly) / geo[5] + 0.5);

    if (querySize == 0) {
        o.w.xsize = o.r.xsize;
        o.w.ysize = o.r.ysize;
    } else {
        o.w.xsize = querySize;
        o.w.ysize = querySize;
    }

    o.w.x = 0;
    if (o.r.x < 0) {
        const int rxShift = std::abs(o.r.x);
        o.w.x = static_cast<int>(o.w.xsize * (static_cast<double>(rxShift) /
                                              static_cast<double>(o.r.xsize)));
        o.w.xsize = o.w.xsize - o.w.x;
        o.r.xsize =
            o.r.xsize -
            static_cast<int>(o.r.xsize * (static_cast<double>(rxShift) /
                                          static_cast<double>(o.r.xsize)));
        o.r.x = 0;
    }

    const int rasterXSize = GDALGetRasterXSize(ds);
    const int rasterYSize = GDALGetRasterYSize(ds);

    if (o.r.x + o.r.xsize > rasterXSize) {
        o.w.xsize = static_cast<int>(
            o.w.xsize *
            (static_cast<double>(rasterXSize) - static_cast<double>(o.r.x)) /
            static_cast<double>(o.r.xsize));
        o.r.xsize = rasterXSize - o.r.x;
    }

    o.w.y = 0;
    if (o.r.y < 0) {
        const int ryShift = std::abs(o.r.y);
        o.w.y = static_cast<int>(o.w.ysize * (static_cast<double>(ryShift) /
                                              static_cast<double>(o.r.ysize)));
        o.w.ysize = o.w.ysize - o.w.y;
        o.r.ysize =
            o.r.ysize -
            static_cast<int>(o.r.ysize * (static_cast<double>(ryShift) /
                                          static_cast<double>(o.r.ysize)));
        o.r.y = 0;
    }

    if (o.r.y + o.r.ysize > rasterYSize) {
        o.w.ysize = static_cast<int>(
            o.w.ysize *
            (static_cast<double>(rasterYSize) - static_cast<double>(o.r.y)) /
            static_cast<double>(o.r.ysize));
        o.r.ysize = rasterYSize - o.r.y;
    }

    return o;
}

int Tiler::tmsToXYZ(int ty, int tz) const {
    return static_cast<int>((std::pow(2, tz) - 1)) - ty;
}

int Tiler::xyzToTMS(int ty, int tz) const {
    return static_cast<int>((std::pow(2, tz) - 1)) - ty;  // The same!
}

GlobalMercator::GlobalMercator(int tileSize) : tileSize(tileSize) {
    originShift = 2.0 * M_PI * 6378137.0 / 2.0;
    initialResolution = 2.0 * M_PI * 6378137.0 / static_cast<double>(tileSize);
    maxZoomLevel = 99;
}

BoundingBox<Geographic2D> GlobalMercator::tileLatLonBounds(int tx, int ty,
                                                           int zoom) const {
    const BoundingBox<Projected2D> bounds = tileBounds(tx, ty, zoom);
    const Geographic2D min = metersToLatLon(bounds.min.x, bounds.min.y);
    const Geographic2D max = metersToLatLon(bounds.max.x, bounds.max.y);
    return BoundingBox<Geographic2D>(min, max);
}

BoundingBox<Projected2D> GlobalMercator::tileBounds(int tx, int ty,
                                                    int zoom) const {
    const Projected2D min = pixelsToMeters(tx * tileSize, ty * tileSize, zoom);
    const Projected2D max =
        pixelsToMeters((tx + 1) * tileSize, (ty + 1) * tileSize, zoom);
    return BoundingBox<Projected2D>(min, max);
}

Geographic2D GlobalMercator::metersToLatLon(double mx, double my) const {
    double lon = mx / originShift * 180.0;
    double lat = my / originShift * 180.0;

    lat = 180.0 / M_PI *
          (2 * std::atan(std::exp(lat * M_PI / 180.0)) - M_PI / 2.0);
    return Geographic2D(lon, lat);
}

Projected2Di GlobalMercator::metersToTile(double mx, double my,
                                          int zoom) const {
    const Projected2D p = metersToPixels(mx, my, zoom);
    return pixelsToTile(p.x, p.y);
}

Projected2D GlobalMercator::pixelsToMeters(int px, int py, int zoom) const {
    const double res = resolution(zoom);
    return Projected2D(px * res - originShift, py * res - originShift);
}

Projected2D GlobalMercator::metersToPixels(double mx, double my,
                                           int zoom) const {
    const double res = resolution(zoom);
    return Projected2D((mx + originShift) / res, (my + originShift) / res);
}

Projected2Di GlobalMercator::pixelsToTile(double px, double py) const {
    return Projected2Di(
        static_cast<int>(std::ceil(px / static_cast<double>(tileSize)) - 1),
        static_cast<int>(std::ceil(py / static_cast<double>(tileSize)) - 1));
}

double GlobalMercator::resolution(int zoom) const {
    return initialResolution / std::pow(2, zoom);
}

int GlobalMercator::zoomForPixelSize(double pixelSize) const {
    for (int i = 0; i < maxZoomLevel; i++) {
        if (pixelSize > resolution(i)) {
            return i - 1;
        }
    }
    LOGW << "Exceeded max zoom level";
    return 0;
}

}  // namespace ddb
