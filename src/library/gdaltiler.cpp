/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gdaltiler.h"

#include <mutex>
#include <memory>
#include <vector>
#include <algorithm>
#include <sstream>
#include <cmath>

#include "entry.h"
#include "exceptions.h"
#include "logger.h"
#include "mio.h"
#include "sensorprofile.h"
#include "vegetation.h"

namespace ddb
{

    bool GDALTiler::hasGeoreference(const GDALDatasetH &dataset)
    {
        double geo[6] = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
        if (GDALGetGeoTransform(dataset, geo) != CE_None)
            throw GDALException("Cannot fetch geotransform in hasGeoreference");

        return (geo[0] != 0.0 || geo[1] != 1.0 || geo[2] != 0.0 || geo[3] != 0.0 ||
                geo[4] != 0.0 || geo[5] != 1.0) ||
               GDALGetGCPCount(dataset) != 0;
    }

    bool GDALTiler::sameProjection(const OGRSpatialReferenceH &a,
                                   const OGRSpatialReferenceH &b)
    {
        char *aProj = nullptr;
        char *bProj = nullptr;

        if (OSRExportToProj4(a, &aProj) != CE_None)
            throw GDALException("Cannot export proj4");
        if (OSRExportToProj4(b, &bProj) != CE_None)
        {
            CPLFree(aProj);
            throw GDALException("Cannot export proj4");
        }

        bool result = std::string(aProj) == std::string(bProj);

        CPLFree(aProj);
        CPLFree(bProj);

        return result;
    }

    int GDALTiler::dataBandsCount(const GDALDatasetH &dataset)
    {
        const GDALRasterBandH raster = GDALGetRasterBand(dataset, 1);
        const GDALRasterBandH alphaBand = GDALGetMaskBand(raster);
        const int bandsCount = GDALGetRasterCount(dataset);
        const GDALRasterBandH lastBand = GDALGetRasterBand(dataset, bandsCount);

        if (GDALGetMaskFlags(alphaBand) & GMF_ALPHA || bandsCount == 4 ||
            bandsCount == 2 || GDALGetRasterColorInterpretation(lastBand) == GCI_AlphaBand)
        {
            return bandsCount - 1;
        }

        return bandsCount;
    }

    GDALTiler::GDALTiler(const std::string &inputPath, const std::string &outputFolder,
                         int tileSize, bool tms)
        : Tiler(inputPath, outputFolder, tileSize, tms)
    {

        pngDrv = GDALGetDriverByName("PNG");
        if (pngDrv == nullptr)
            throw GDALException("Cannot create PNG driver");
        memDrv = GDALGetDriverByName("MEM");
        if (memDrv == nullptr)
            throw GDALException("Cannot create MEM driver");

        std::string openPath = inputPath;
        if (utils::isNetworkPath(openPath))
        {
            CPLSetConfigOption("GDAL_DISABLE_READDIR_ON_OPEN", "YES");
            CPLSetConfigOption("CPL_VSIL_CURL_ALLOWED_EXTENSIONS", ".tif,.tiff");
            // CPLSetConfigOption("CPL_DEBUG", "ON");

            openPath = "/vsicurl/" + openPath;
        }

        inputDataset = GDALOpen(openPath.c_str(), GA_ReadOnly);
        if (inputDataset == nullptr)
            throw GDALException("Cannot open " + openPath);

        rasterCount = GDALGetRasterCount(inputDataset);
        if (rasterCount == 0)
            throw GDALException("No raster bands found in " + openPath);

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
        if (GDALGetProjectionRef(inputDataset) != nullptr)
        {
            inputSrsWkt = GDALGetProjectionRef(inputDataset);
        }
        else if (GDALGetGCPCount(inputDataset) > 0)
        {
            inputSrsWkt = GDALGetGCPProjection(inputDataset);
        }
        else
        {
            throw GDALException("No projection found in " + openPath);
        }

        char *wktp = const_cast<char *>(inputSrsWkt.c_str());
        if (OSRImportFromWkt(inputSrs, &wktp) != OGRERR_NONE)
            throw GDALException("Cannot read spatial reference system for " + openPath + ". Is PROJ available?");

        // Setup output SRS
        const OGRSpatialReferenceH outputSrs = OSRNewSpatialReference(nullptr);
        OSRImportFromEPSG(outputSrs, 3857); // TODO: support for geodetic?

        if (!hasGeoreference(inputDataset))
            throw GDALException(openPath + " is not georeferenced.");

        // Check if we need to reproject
        if (!sameProjection(inputSrs, outputSrs))
        {
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

        double outGt[6];
        if (GDALGetGeoTransform(inputDataset, outGt) != CE_None)
            throw GDALException("Cannot fetch geotransform outGt");

        // Validate geotransform values
        if (std::abs(outGt[1]) < std::numeric_limits<double>::epsilon() ||
            std::abs(outGt[5]) < std::numeric_limits<double>::epsilon())
        {
            throw GDALException("Invalid geotransform: pixel size is zero");
        }

        oMinX = outGt[0];
        oMaxX = outGt[0] + GDALGetRasterXSize(inputDataset) * outGt[1];
        oMaxY = outGt[3];
        oMinY = outGt[3] - GDALGetRasterYSize(inputDataset) * outGt[1];

        LOGD << "Bounds (output SRS): " << oMinX << "," << oMinY << "," << oMaxX
             << "," << oMaxY;

        // Max/min zoom level
        tMaxZ = mercator.zoomForPixelSize(outGt[1]);
        tMinZ = mercator.zoomForPixelSize(outGt[1] *
                                          std::max(GDALGetRasterXSize(inputDataset),
                                                   GDALGetRasterYSize(inputDataset)) /
                                          tileSize);

        LOGD << "MinZ: " << tMinZ;
        LOGD << "MaxZ: " << tMaxZ;
        LOGD << "Num bands: " << nBands;
    }

    GDALTiler::~GDALTiler()
    {
        if (inputDataset && inputDataset != origDataset)
            GDALClose(inputDataset);
        if (origDataset)
            GDALClose(origDataset);
    }

    std::string GDALTiler::tile(int tz, int tx, int ty, uint8_t **outBuffer, int *outBufferSize)
    {
        std::string tilePath = getTilePath(tz, tx, ty, true);

        if (tms)
        {
            ty = tmsToXYZ(ty, tz);
            LOGD << "TY: " << ty;
        }

        BoundingBox<Projected2Di> tMinMax = getMinMaxCoordsForZ(tz);
        if (!tMinMax.contains(tx, ty))
            throw GDALException("Out of bounds");

        // Need to create in-memory dataset
        // (PNG driver does not have Create() method)
        int cappedBands = std::min(3, nBands); // PNG driver supports at most 4 bands (rgba)
        const GDALDatasetH dsTile = GDALCreate(memDrv, "", tileSize, tileSize, cappedBands + 1,
                                               GDT_Byte, nullptr);
        if (dsTile == nullptr)
            throw GDALException("Cannot create dsTile");

        BoundingBox<Projected2D> b = mercator.tileBounds(tx, ty, tz);

        const int querySize = tileSize; // TODO: you will need to change this for
                                        // interpolations other than NN
        GQResult g = geoQuery(inputDataset, b.min.x, b.max.y, b.max.x, b.min.y, querySize);

        LOGD << "GeoQuery: " << g.r.x << "," << g.r.y << "|" << g.r.xsize << "x"
             << g.r.ysize << "|" << g.w.x << "," << g.w.y << "|" << g.w.xsize << "x"
             << g.w.ysize;

        if (g.r.xsize != 0 && g.r.ysize != 0 && g.w.xsize != 0 && g.w.ysize != 0)
        {
            const GDALDataType type =
                GDALGetRasterDataType(GDALGetRasterBand(inputDataset, 1));

            const size_t wSize = g.w.xsize * g.w.ysize;
            std::unique_ptr<uint8_t[]> buffer(
                new uint8_t[GDALGetDataTypeSizeBytes(type) * cappedBands * wSize]);

            if (GDALDatasetRasterIO(inputDataset, GF_Read, g.r.x, g.r.y, g.r.xsize,
                                    g.r.ysize, buffer.get(), g.w.xsize, g.w.ysize, type,
                                    cappedBands, nullptr, 0, 0, 0) != CE_None)
            {
                throw GDALException("Cannot read input dataset window");
            }

            // Rescale if needed
            // We currently don't rescale byte datasets
            // TODO: allow people to specify rescale values

            if (type != GDT_Byte && type != GDT_Unknown)
            {
                std::unique_ptr<uint8_t[]> scaledBuffer(new uint8_t[GDALGetDataTypeSizeBytes(GDT_Byte) * cappedBands * wSize]);
                size_t bufSize = wSize * cappedBands;

                double globalMin = std::numeric_limits<double>::max(),
                       globalMax = std::numeric_limits<double>::min();

                for (int i = 0; i < cappedBands; i++)
                {
                    double bMin, bMax;

                    GDALDatasetH ds = origDataset != nullptr ? origDataset : inputDataset; // Use the actual dataset, not the VRT
                    GDALDatasetH hBand = GDALGetRasterBand(ds, i + 1);

                    CPLErr statsRes = GDALGetRasterStatistics(hBand, TRUE, FALSE, &bMin, &bMax, nullptr, nullptr);
                    if (statsRes == CE_Warning)
                    {
                        double bMean, bStdDev;
                        if (GDALGetRasterStatistics(hBand, TRUE, TRUE, &bMin, &bMax, &bMean, &bStdDev) != CE_None)
                            throw GDALException("Cannot compute band statistics (forced)");
                        if (GDALSetRasterStatistics(hBand, bMin, bMax, bMean, bStdDev) != CE_None)
                            throw GDALException("Cannot cache band statistics");

                        LOGD << "Cached band " << i << " statistics (" << bMin << ", " << bMax << ")";
                    }
                    else if (statsRes == CE_Failure)
                    {
                        throw GDALException("Cannot compute band statistics");
                    }

                    globalMin = std::min(globalMin, bMin);
                    globalMax = std::max(globalMax, bMax);
                }

                switch (type)
                {
                case GDT_Byte:
                    rescale<uint8_t>(buffer.get(), scaledBuffer.get(), bufSize, globalMin, globalMax);
                    break;
                case GDT_UInt16:
                    rescale<uint16_t>(buffer.get(), scaledBuffer.get(), bufSize, globalMin, globalMax);
                    break;
                case GDT_Int16:
                    rescale<int16_t>(buffer.get(), scaledBuffer.get(), bufSize, globalMin, globalMax);
                    break;
                case GDT_UInt32:
                    rescale<uint32_t>(buffer.get(), scaledBuffer.get(), bufSize, globalMin, globalMax);
                    break;
                case GDT_Int32:
                    rescale<int32_t>(buffer.get(), scaledBuffer.get(), bufSize, globalMin, globalMax);
                    break;
                case GDT_Float32:
                    rescale<float>(buffer.get(), scaledBuffer.get(), bufSize, globalMin, globalMax);
                    break;
                case GDT_Float64:
                    rescale<double>(buffer.get(), scaledBuffer.get(), bufSize, globalMin, globalMax);
                    break;
                default:
                    break;
                }

                buffer = std::move(scaledBuffer);
            }

            const GDALRasterBandH raster = GDALGetRasterBand(inputDataset, 1);
            GDALRasterBandH alphaBand = FindAlphaBand(inputDataset);
            if (alphaBand == nullptr)
                alphaBand = GDALGetMaskBand(raster);

            std::unique_ptr<uint8_t[]> alphaBuffer(
                new uint8_t[GDALGetDataTypeSizeBytes(GDT_Byte) * wSize]);
            if (GDALRasterIO(alphaBand, GF_Read, g.r.x, g.r.y, g.r.xsize, g.r.ysize,
                             alphaBuffer.get(), g.w.xsize, g.w.ysize, GDT_Byte, 0,
                             0) != CE_None)
            {
                throw GDALException("Cannot read input dataset alpha window");
            }

            // Write data

            if (tileSize == querySize)
            {
                if (GDALDatasetRasterIO(dsTile, GF_Write, g.w.x, g.w.y, g.w.xsize,
                                        g.w.ysize, buffer.get(), g.w.xsize, g.w.ysize,
                                        GDT_Byte, cappedBands, nullptr, 0, 0,
                                        0) != CE_None)
                {
                    throw GDALException("Cannot write tile data");
                }

                LOGD << "Wrote tile data";

                const GDALRasterBandH tileAlphaBand =
                    GDALGetRasterBand(dsTile, cappedBands + 1);
                GDALSetRasterColorInterpretation(tileAlphaBand, GCI_AlphaBand);

                if (GDALRasterIO(tileAlphaBand, GF_Write, g.w.x, g.w.y, g.w.xsize,
                                 g.w.ysize, alphaBuffer.get(), g.w.xsize, g.w.ysize,
                                 GDT_Byte, 0, 0) != CE_None)
                {
                    throw GDALException("Cannot write tile alpha data");
                }

                LOGD << "Wrote tile alpha";
            }
            else
            {
                // TODO: readraster query in memory scaled to tilesize
                throw GDALException("Not implemented");
            }
        }
        else
        {
            throw GDALException("Geoquery out of bounds");
        }

        const GDALDatasetH outDs = GDALCreateCopy(pngDrv, tilePath.c_str(), dsTile, FALSE,
                                                  nullptr, nullptr, nullptr);
        if (outDs == nullptr)
            throw GDALException("Cannot create output dataset " + tilePath);

        GDALFlushCache(outDs);
        GDALClose(outDs);
        GDALClose(dsTile);

        if (outBuffer != nullptr)
        {
            vsi_l_offset bufSize;
            *outBuffer = VSIGetMemFileBuffer(tilePath.c_str(), &bufSize, TRUE);
            if (bufSize > std::numeric_limits<int>::max())
                throw GDALException("Exceeded max buf size");
            *outBufferSize = bufSize;
            return "";
        }
        else
        {
            return tilePath;
        }
    }

    std::string GDALTiler::tile(int tz, int tx, int ty,
                                const ThumbVisParams &visParams,
                                uint8_t **outBuffer, int *outBufferSize)
    {
        // If no vis params set, fall through to standard tile
        if (visParams.preset.empty() && visParams.bands.empty() &&
            visParams.formula.empty()) {
            return tile(tz, tx, ty, outBuffer, outBufferSize);
        }

        std::string tilePath = getTilePath(tz, tx, ty, true);

        if (tms) {
            ty = tmsToXYZ(ty, tz);
        }

        BoundingBox<Projected2Di> tMinMax = getMinMaxCoordsForZ(tz);
        if (!tMinMax.contains(tx, ty))
            throw GDALException("Out of bounds");

        const int totalBands = GDALGetRasterCount(inputDataset);
        BoundingBox<Projected2D> b = mercator.tileBounds(tx, ty, tz);
        const int querySize = tileSize;
        GQResult g = geoQuery(inputDataset, b.min.x, b.max.y, b.max.x, b.min.y, querySize);

        if (g.r.xsize == 0 || g.r.ysize == 0 || g.w.xsize == 0 || g.w.ysize == 0)
            throw GDALException("Geoquery out of bounds");

        const size_t wSize = static_cast<size_t>(g.w.xsize) * g.w.ysize;

        // --- Formula mode ---
        if (!visParams.formula.empty()) {
            auto& ve = VegetationEngine::instance();

            BandFilter bf;
            if (!visParams.bandFilter.empty()) {
                bf = VegetationEngine::parseFilter(visParams.bandFilter, totalBands);
            } else {
                bf = ve.autoDetectFilter(inputPath);
            }

            // Read all bands as float
            std::vector<std::vector<float>> bandStorage(totalBands);
            std::vector<float*> bandPtrs(totalBands);
            int alphaBandIdx = -1;
            std::vector<int> bandHasNodata(totalBands, 0);
            std::vector<double> bandNodataVal(totalBands, 0.0);
            for (int b2 = 0; b2 < totalBands; b2++) {
                bandStorage[b2].resize(wSize);
                bandPtrs[b2] = bandStorage[b2].data();
                GDALRasterBandH hBand = GDALGetRasterBand(inputDataset, b2 + 1);
                if (GDALRasterIO(hBand, GF_Read, g.r.x, g.r.y, g.r.xsize, g.r.ysize,
                                 bandPtrs[b2], g.w.xsize, g.w.ysize,
                                 GDT_Float32, 0, 0) != CE_None) {
                    throw GDALException("Cannot read band " + std::to_string(b2 + 1));
                }
                if (GDALGetRasterColorInterpretation(hBand) == GCI_AlphaBand) {
                    alphaBandIdx = b2;
                }
                bandNodataVal[b2] = GDALGetRasterNoDataValue(hBand, &bandHasNodata[b2]);
            }

            // Pre-mask transparent and nodata pixels
            float nodata = -9999.0f;
            for (size_t i = 0; i < wSize; i++) {
                bool masked = false;
                if (alphaBandIdx >= 0 && bandPtrs[alphaBandIdx][i] == 0.0f) {
                    masked = true;
                }
                if (!masked) {
                    for (int b2 = 0; b2 < totalBands; b2++) {
                        if (b2 == alphaBandIdx) continue;
                        if (bandHasNodata[b2] &&
                            static_cast<double>(bandPtrs[b2][i]) == bandNodataVal[b2]) {
                            masked = true;
                            break;
                        }
                    }
                }
                if (masked) {
                    for (int b2 = 0; b2 < totalBands; b2++) {
                        bandPtrs[b2][i] = nodata;
                    }
                }
            }

            // Apply formula
            std::vector<float> result(wSize);
            const auto* formulaPtr = ve.getFormula(visParams.formula);
            if (!formulaPtr)
                throw InvalidArgsException("Unknown formula: " + visParams.formula);

            ve.applyFormula(*formulaPtr, bf, bandPtrs, result.data(), wSize, nodata);

            // Determine rescale range
            float rMin, rMax;
            if (!visParams.rescale.empty()) {
                auto commaPos = visParams.rescale.find(',');
                if (commaPos == std::string::npos)
                    throw InvalidArgsException("Invalid rescale format: " + visParams.rescale);
                rMin = std::stof(visParams.rescale.substr(0, commaPos));
                rMax = std::stof(visParams.rescale.substr(commaPos + 1));
            } else if (formulaPtr->hasRange && formulaPtr->rangeMin != formulaPtr->rangeMax) {
                rMin = static_cast<float>(formulaPtr->rangeMin);
                rMax = static_cast<float>(formulaPtr->rangeMax);
            } else {
                // Compute p2-p98
                std::vector<float> valid;
                valid.reserve(wSize);
                for (size_t i = 0; i < wSize; i++) {
                    if (result[i] != nodata && !std::isnan(result[i]))
                        valid.push_back(result[i]);
                }
                if (!valid.empty()) {
                    std::sort(valid.begin(), valid.end());
                    rMin = valid[static_cast<size_t>(valid.size() * 0.02)];
                    rMax = valid[std::min(valid.size() - 1, static_cast<size_t>(valid.size() * 0.98))];
                } else {
                    rMin = 0; rMax = 1;
                }
            }

            // Apply colormap
            std::string cmId = visParams.colormap.empty() ? "rdylgn" : visParams.colormap;
            const auto* cmap = ve.getColormap(cmId);
            if (!cmap)
                throw InvalidArgsException("Unknown colormap: " + cmId);

            std::vector<uint8_t> rgba(wSize * 4);
            ve.applyColormap(result.data(), rgba.data(), wSize, *cmap, rMin, rMax, nodata);

            // Create output tile (RGBA)
            GDALDatasetH dsTile = GDALCreate(memDrv, "", tileSize, tileSize, 4, GDT_Byte, nullptr);
            if (!dsTile) throw GDALException("Cannot create tile dataset");

            for (int ch = 0; ch < 4; ch++) {
                std::vector<uint8_t> chanData(wSize);
                for (size_t i = 0; i < wSize; i++) chanData[i] = rgba[i * 4 + ch];
                GDALRasterBandH hBand = GDALGetRasterBand(dsTile, ch + 1);
                GDALRasterIO(hBand, GF_Write, g.w.x, g.w.y, g.w.xsize, g.w.ysize,
                             chanData.data(), g.w.xsize, g.w.ysize, GDT_Byte, 0, 0);
            }
            GDALSetRasterColorInterpretation(GDALGetRasterBand(dsTile, 4), GCI_AlphaBand);

            GDALDatasetH outDs = GDALCreateCopy(pngDrv, tilePath.c_str(), dsTile, FALSE,
                                                nullptr, nullptr, nullptr);
            if (!outDs) {
                GDALClose(dsTile);
                throw GDALException("Cannot create output tile");
            }
            GDALFlushCache(outDs);
            GDALClose(outDs);
            GDALClose(dsTile);

            if (outBuffer != nullptr) {
                vsi_l_offset bufSize;
                *outBuffer = VSIGetMemFileBuffer(tilePath.c_str(), &bufSize, TRUE);
                if (bufSize > std::numeric_limits<int>::max())
                    throw GDALException("Exceeded max buf size");
                *outBufferSize = static_cast<int>(bufSize);
                return "";
            }
            return tilePath;
        }

        // --- Band selection mode ---
        std::vector<int> selectedBands;
        if (!visParams.bands.empty()) {
            std::istringstream ss(visParams.bands);
            std::string token;
            while (std::getline(ss, token, ',')) {
                int bIdx = std::stoi(token);
                if (bIdx < 1 || bIdx > totalBands)
                    throw InvalidArgsException("Band index " + std::to_string(bIdx) + " out of range");
                selectedBands.push_back(bIdx);
            }
        } else if (!visParams.preset.empty()) {
            auto& spm = SensorProfileManager::instance();
            auto mapping = spm.getBandMappingForPreset(inputPath, visParams.preset);
            selectedBands = {mapping.r, mapping.g, mapping.b};
        } else {
            // Auto-detect sensor
            auto& spm = SensorProfileManager::instance();
            auto detection = spm.detectSensor(inputPath);
            if (detection.detected) {
                selectedBands = {detection.defaultBandMapping.r,
                                 detection.defaultBandMapping.g,
                                 detection.defaultBandMapping.b};
            }
        }

        int outBands = selectedBands.empty() ? std::min(3, nBands) : static_cast<int>(selectedBands.size());
        outBands = std::min(3, outBands);

        GDALDatasetH dsTile = GDALCreate(memDrv, "", tileSize, tileSize, outBands + 1,
                                         GDT_Byte, nullptr);
        if (!dsTile) throw GDALException("Cannot create tile dataset");

        const GDALDataType type = GDALGetRasterDataType(GDALGetRasterBand(inputDataset, 1));

        if (!selectedBands.empty()) {
            // Read selected bands individually
            for (int i = 0; i < outBands; i++) {
                int srcBand = selectedBands[i];
                GDALRasterBandH hSrcBand = GDALGetRasterBand(inputDataset, srcBand);

                if (type != GDT_Byte) {
                    // Read as float, compute stats, rescale to byte
                    std::vector<float> fBuf(wSize);
                    if (GDALRasterIO(hSrcBand, GF_Read, g.r.x, g.r.y, g.r.xsize, g.r.ysize,
                                     fBuf.data(), g.w.xsize, g.w.ysize, GDT_Float32, 0, 0) != CE_None)
                        throw GDALException("Cannot read band " + std::to_string(srcBand));

                    // Get band statistics for percentile stretch
                    GDALDatasetH statsDs = origDataset != nullptr ? origDataset : inputDataset;
                    GDALRasterBandH hStatsBand = GDALGetRasterBand(statsDs, srcBand);
                    double bMin, bMax, bMean, bStdDev;
                    CPLErr statsRes = GDALGetRasterStatistics(hStatsBand, TRUE, FALSE, &bMin, &bMax, &bMean, &bStdDev);
                    if (statsRes != CE_None) {
                        GDALComputeRasterStatistics(hStatsBand, TRUE, &bMin, &bMax, &bMean, &bStdDev, nullptr, nullptr);
                    }

                    // Use p2-p98 approximation from mean/stddev
                    double sMin = bMean - 2.33 * bStdDev;
                    double sMax = bMean + 2.33 * bStdDev;
                    sMin = std::max(sMin, bMin);
                    sMax = std::min(sMax, bMax);
                    if (sMin >= sMax) { sMin = bMin; sMax = bMax; }
                    if (sMin >= sMax) sMax = sMin + 1.0;

                    std::vector<uint8_t> byteBuf(wSize);
                    double range = sMax - sMin;
                    for (size_t px = 0; px < wSize; px++) {
                        double v = std::max(sMin, std::min(sMax, static_cast<double>(fBuf[px])));
                        byteBuf[px] = static_cast<uint8_t>(255.0 * (v - sMin) / range);
                    }

                    GDALRasterBandH hDstBand = GDALGetRasterBand(dsTile, i + 1);
                    GDALRasterIO(hDstBand, GF_Write, g.w.x, g.w.y, g.w.xsize, g.w.ysize,
                                 byteBuf.data(), g.w.xsize, g.w.ysize, GDT_Byte, 0, 0);
                } else {
                    // Byte data: read directly
                    std::vector<uint8_t> buf(wSize);
                    if (GDALRasterIO(hSrcBand, GF_Read, g.r.x, g.r.y, g.r.xsize, g.r.ysize,
                                     buf.data(), g.w.xsize, g.w.ysize, GDT_Byte, 0, 0) != CE_None)
                        throw GDALException("Cannot read band " + std::to_string(srcBand));

                    GDALRasterBandH hDstBand = GDALGetRasterBand(dsTile, i + 1);
                    GDALRasterIO(hDstBand, GF_Write, g.w.x, g.w.y, g.w.xsize, g.w.ysize,
                                 buf.data(), g.w.xsize, g.w.ysize, GDT_Byte, 0, 0);
                }
            }
        } else {
            // No band selection: use standard path (first N bands)
            std::unique_ptr<uint8_t[]> buffer(
                new uint8_t[GDALGetDataTypeSizeBytes(type) * outBands * wSize]);

            if (GDALDatasetRasterIO(inputDataset, GF_Read, g.r.x, g.r.y, g.r.xsize,
                                    g.r.ysize, buffer.get(), g.w.xsize, g.w.ysize, type,
                                    outBands, nullptr, 0, 0, 0) != CE_None) {
                GDALClose(dsTile);
                throw GDALException("Cannot read input dataset window");
            }

            if (type != GDT_Byte && type != GDT_Unknown) {
                std::unique_ptr<uint8_t[]> scaledBuffer(new uint8_t[GDALGetDataTypeSizeBytes(GDT_Byte) * outBands * wSize]);
                size_t bufSize = wSize * outBands;

                double globalMin = std::numeric_limits<double>::max();
                double globalMax = std::numeric_limits<double>::lowest();
                for (int i = 0; i < outBands; i++) {
                    double bMin2, bMax2;
                    GDALDatasetH ds = origDataset != nullptr ? origDataset : inputDataset;
                    GDALRasterBandH hBand = GDALGetRasterBand(ds, i + 1);
                    CPLErr statsRes = GDALGetRasterStatistics(hBand, TRUE, FALSE, &bMin2, &bMax2, nullptr, nullptr);
                    if (statsRes == CE_Warning) {
                        double bMean2, bStdDev2;
                        GDALGetRasterStatistics(hBand, TRUE, TRUE, &bMin2, &bMax2, &bMean2, &bStdDev2);
                        GDALSetRasterStatistics(hBand, bMin2, bMax2, bMean2, bStdDev2);
                    } else if (statsRes == CE_Failure) {
                        throw GDALException("Cannot compute band statistics");
                    }
                    globalMin = std::min(globalMin, bMin2);
                    globalMax = std::max(globalMax, bMax2);
                }

                switch (type) {
                case GDT_UInt16: rescale<uint16_t>(buffer.get(), scaledBuffer.get(), bufSize, globalMin, globalMax); break;
                case GDT_Int16: rescale<int16_t>(buffer.get(), scaledBuffer.get(), bufSize, globalMin, globalMax); break;
                case GDT_UInt32: rescale<uint32_t>(buffer.get(), scaledBuffer.get(), bufSize, globalMin, globalMax); break;
                case GDT_Int32: rescale<int32_t>(buffer.get(), scaledBuffer.get(), bufSize, globalMin, globalMax); break;
                case GDT_Float32: rescale<float>(buffer.get(), scaledBuffer.get(), bufSize, globalMin, globalMax); break;
                case GDT_Float64: rescale<double>(buffer.get(), scaledBuffer.get(), bufSize, globalMin, globalMax); break;
                default: break;
                }
                buffer = std::move(scaledBuffer);
            }

            if (GDALDatasetRasterIO(dsTile, GF_Write, g.w.x, g.w.y, g.w.xsize,
                                    g.w.ysize, buffer.get(), g.w.xsize, g.w.ysize,
                                    GDT_Byte, outBands, nullptr, 0, 0, 0) != CE_None) {
                GDALClose(dsTile);
                throw GDALException("Cannot write tile data");
            }
        }

        // Alpha
        GDALRasterBandH alphaBand = FindAlphaBand(inputDataset);
        if (alphaBand == nullptr) {
            GDALRasterBandH raster = GDALGetRasterBand(inputDataset, 1);
            alphaBand = GDALGetMaskBand(raster);
        }

        std::vector<uint8_t> alphaBuffer(wSize);
        if (GDALRasterIO(alphaBand, GF_Read, g.r.x, g.r.y, g.r.xsize, g.r.ysize,
                         alphaBuffer.data(), g.w.xsize, g.w.ysize, GDT_Byte, 0, 0) != CE_None) {
            GDALClose(dsTile);
            throw GDALException("Cannot read alpha data");
        }

        GDALRasterBandH tileAlpha = GDALGetRasterBand(dsTile, outBands + 1);
        GDALSetRasterColorInterpretation(tileAlpha, GCI_AlphaBand);
        if (GDALRasterIO(tileAlpha, GF_Write, g.w.x, g.w.y, g.w.xsize, g.w.ysize,
                         alphaBuffer.data(), g.w.xsize, g.w.ysize, GDT_Byte, 0, 0) != CE_None) {
            GDALClose(dsTile);
            throw GDALException("Cannot write alpha data");
        }

        GDALDatasetH outDs = GDALCreateCopy(pngDrv, tilePath.c_str(), dsTile, FALSE,
                                            nullptr, nullptr, nullptr);
        if (!outDs) {
            GDALClose(dsTile);
            throw GDALException("Cannot create output tile");
        }
        GDALFlushCache(outDs);
        GDALClose(outDs);
        GDALClose(dsTile);

        if (outBuffer != nullptr) {
            vsi_l_offset bufSize;
            *outBuffer = VSIGetMemFileBuffer(tilePath.c_str(), &bufSize, TRUE);
            if (bufSize > std::numeric_limits<int>::max())
                throw GDALException("Exceeded max buf size");
            *outBufferSize = static_cast<int>(bufSize);
            return "";
        }
        return tilePath;
    }

    template <typename T>
    void GDALTiler::rescale(uint8_t *buffer, uint8_t *dstBuffer, size_t bufsize, double bMin, double bMax)
    {
        T *ptr = reinterpret_cast<T *>(buffer);

        // Avoid divide by zero
        if (bMin == bMax)
            bMax += 0.1;

        LOGD << "Min: " << bMin << " | Max: " << bMax;

        // Can still happen according to GDAL for very large values
        if (bMin == bMax)
            throw GDALException(
                "Cannot scale values due to source min/max being equal");

        double deltamm = bMax - bMin;

        for (size_t i = 0; i < bufsize; i++)
        {
            double v = std::max(bMin, std::min(bMax, static_cast<double>(ptr[i])));
            dstBuffer[i] = static_cast<uint8_t>(255.0 * (v - bMin) / deltamm);
        }
    }

    GDALDatasetH GDALTiler::createWarpedVRT(const GDALDatasetH &src,
                                            const OGRSpatialReferenceH &srs,
                                            GDALResampleAlg resampling)
    {
        char *dstWkt = nullptr;
        if (OSRExportToWkt(srs, &dstWkt) != OGRERR_NONE)
            throw GDALException("Cannot export dst WKT " + inputPath +
                                ". Is PROJ available?");
        const char *srcWkt = GDALGetProjectionRef(src);

        GDALWarpOptions *opts = GDALCreateWarpOptions();

        // If the dataset does not have alpha, add it
        bool hasAlpha = FindAlphaBand(src) != nullptr;
        if (!hasAlpha)
        {
            opts->nDstAlphaBand = GDALGetRasterCount(src) + 1;
        }

        const GDALDatasetH warpedVrt = GDALAutoCreateWarpedVRT(
            src, srcWkt, dstWkt, resampling, 0.001, opts);

        CPLFree(dstWkt);

        if (warpedVrt == nullptr)
        {
            GDALDestroyWarpOptions(opts);
            throw GDALException("Cannot create warped VRT");
        }

        GDALDestroyWarpOptions(opts);

        return warpedVrt;
    }

    GQResult GDALTiler::geoQuery(GDALDatasetH ds, double ulx, double uly, double lrx,
                                 double lry, int querySize)
    {
        GQResult o;
        double geo[6];
        if (GDALGetGeoTransform(ds, geo) != CE_None)
            throw GDALException("Cannot fetch geotransform geo");

        // Check for division by zero
        if (std::abs(geo[1]) < std::numeric_limits<double>::epsilon() ||
            std::abs(geo[5]) < std::numeric_limits<double>::epsilon())
        {
            throw GDALException("Invalid geotransform: pixel size is zero");
        }

        o.r.x = static_cast<int>((ulx - geo[0]) / geo[1] + 0.001);
        o.r.y = static_cast<int>((uly - geo[3]) / geo[5] + 0.001);
        o.r.xsize = static_cast<int>((lrx - ulx) / geo[1] + 0.5);
        o.r.ysize = static_cast<int>((lry - uly) / geo[5] + 0.5);

        if (querySize == 0)
        {
            o.w.xsize = o.r.xsize;
            o.w.ysize = o.r.ysize;
        }
        else
        {
            o.w.xsize = querySize;
            o.w.ysize = querySize;
        }

        o.w.x = 0;
        if (o.r.x < 0)
        {
            const int rxShift = std::abs(o.r.x);
            if (o.r.xsize > 0)
            {
                o.w.x = static_cast<int>(o.w.xsize * (static_cast<double>(rxShift) /
                                                      static_cast<double>(o.r.xsize)));
                o.w.xsize = o.w.xsize - o.w.x;
                o.r.xsize =
                    o.r.xsize -
                    static_cast<int>(o.r.xsize * (static_cast<double>(rxShift) /
                                                  static_cast<double>(o.r.xsize)));
            }
            o.r.x = 0;
        }

        const int rasterXSize = GDALGetRasterXSize(ds);
        const int rasterYSize = GDALGetRasterYSize(ds);

        if (o.r.x + o.r.xsize > rasterXSize)
        {
            if (o.r.xsize > 0)
            {
                o.w.xsize = static_cast<int>(
                    o.w.xsize *
                    (static_cast<double>(rasterXSize) - static_cast<double>(o.r.x)) /
                    static_cast<double>(o.r.xsize));
            }
            o.r.xsize = rasterXSize - o.r.x;
        }

        o.w.y = 0;
        if (o.r.y < 0)
        {
            const int ryShift = std::abs(o.r.y);
            if (o.r.ysize > 0)
            {
                o.w.y = static_cast<int>(o.w.ysize * (static_cast<double>(ryShift) /
                                                      static_cast<double>(o.r.ysize)));
                o.w.ysize = o.w.ysize - o.w.y;
                o.r.ysize =
                    o.r.ysize -
                    static_cast<int>(o.r.ysize * (static_cast<double>(ryShift) /
                                                  static_cast<double>(o.r.ysize)));
            }
            o.r.y = 0;
        }

        if (o.r.y + o.r.ysize > rasterYSize)
        {
            if (o.r.ysize > 0)
            {
                o.w.ysize = static_cast<int>(
                    o.w.ysize *
                    (static_cast<double>(rasterYSize) - static_cast<double>(o.r.y)) /
                    static_cast<double>(o.r.ysize));
            }
            o.r.ysize = rasterYSize - o.r.y;
        }

        return o;
    }

    GDALRasterBandH GDALTiler::FindAlphaBand(const GDALDatasetH &dataset)
    {
        // If the dataset does not have alpha, add it
        const int numBands = GDALGetRasterCount(dataset);
        for (int n = 0; n < numBands; n++)
        {
            GDALRasterBandH b = GDALGetRasterBand(dataset, n + 1);
            if (GDALGetRasterColorInterpretation(b) == GCI_AlphaBand)
            {
                return b;
            }
        }
        return nullptr;
    }

} // namespace ddb
