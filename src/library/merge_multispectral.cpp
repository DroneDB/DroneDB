/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The merge of single-band rasters into one multi-band GeoTIFF. The validation, the alignment
// detection and the preview are in merge_multispectral_validate.cpp, _align.cpp and _preview.cpp.

#include "merge_multispectral.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include "exceptions.h"
#include "exif.h"
#include "exifeditor.h"
#include "artefact_cache.h"
#include "gdal_raii.h"
#include "gtiff_options.h"
#include "logger.h"
#include "mathutil.h"
#include "merge_vrt.h"
#include "mio.h"
#include "parallel.h"

namespace ddb {

namespace {

using merge::SeparateVrt;
using merge::buildSeparateVrt;
using merge::openAll;

// The creation options of the tiled GTiff that a merge writes (GDALTranslate and GDALWarp
// alike); numThreads is the GDAL thread option of the merge's CPU tokens
// (parallel::gdalNumThreads). JPEG at quality 90 for 8-bit 3- and 4-band outputs (JPEG_QUALITY,
// the GTiff key; it said QUALITY, which GTiff ignores, so the quality was always GDAL's 75), LZW
// with the predictor of the sample type for the rest (a predictor is not valid for JPEG).
GtiffOptions mergeCreationOptions(GDALDataType dt, bool useJpeg, const std::string &numThreads) {
    GtiffOptions creation;
    creation.tiled().add(numThreads).bigTiffIfSafer();
    if (useJpeg) {
        creation.compress(GtiffOptions::Codec::Jpeg).set("JPEG_QUALITY", "90");
    } else {
        creation.compress(GtiffOptions::Codec::Lzw).predictorFor(dt);
    }
    return creation;
}

// Options of the tiled GTiff that GDALTranslate writes for a merge
GdalOptionsPtr<GDALTranslateOptions> mergeTranslateOptions(GDALDataType dt, bool useJpeg,
                                                           const std::string &numThreads) {
    CPLStringList transArgs;
    transArgs.AddString("-of");
    transArgs.AddString("GTiff");
    mergeCreationOptions(dt, useJpeg, numThreads).appendTo(transArgs);

    return GdalOptionsPtr<GDALTranslateOptions>(GDALTranslateOptionsNew(transArgs.List(), nullptr));
}

// How the merged raster is written
struct MergeEncoding {
    GDALDataType dataType;
    bool useJpeg;
    // The GDAL thread option of the CPU tokens held for the merge (parallel::gdalNumThreads)
    std::string numThreads;
};

// The GPS position of the first input file (all bands come from the same capture position),
// read before the merge
struct SourceGps {
    GeoLocation location;
    bool present = false;
};

SourceGps readSourceGps(const std::string &path) {
    SourceGps gps;
    // A file with no metadata the reader can give has no position
    if (const auto parser = readExifParser(io::fromUtf8(path))) {
        gps.present = parser->extractGeo(gps.location);
        if (gps.present) {
            LOGD << "Read GPS from " << path
                 << ": lat=" << gps.location.latitude
                 << " lon=" << gps.location.longitude
                 << " alt=" << gps.location.altitude;
        }
    }
    return gps;
}

// The integer shift of every band against the reference band, and the margins that cropping to
// the area all bands cover takes off each side
struct AlignmentCrop {
    std::vector<int> dx, dy;
    int padLeft = 0, padRight = 0, padTop = 0, padBottom = 0;
    int outW = 0, outH = 0;
};

// Plans the crop of the validated alignment; nothing when the shift is so large that no area
// would be left
std::optional<AlignmentCrop> planAlignmentCrop(const MergeValidationResult &validation) {
    LOGD << "Applying band alignment shift correction (max shift: "
         << validation.alignment.maxShiftPixels << " px)";

    const auto &alignBands = validation.alignment.bands;
    const size_t N = alignBands.size();
    int w = validation.summary.width;
    int h = validation.summary.height;

    // Calculate integer shifts
    // Find REF (the band with shiftX=0 and shiftY=0)
    size_t REF = 0;
    for (size_t i = 0; i < N; i++) {
        if (alignBands[i].detected && alignBands[i].shiftX == 0 && alignBands[i].shiftY == 0) {
            REF = i;
            break;
        }
    }

    AlignmentCrop crop;
    crop.dx.assign(N, 0);
    crop.dy.assign(N, 0);
    for (size_t i = 0; i < N; i++) {
        if (i == REF || !alignBands[i].detected || alignBands[i].isThermal) continue;
        crop.dx[i] = static_cast<int>(std::round(alignBands[i].shiftX));
        crop.dy[i] = static_cast<int>(std::round(alignBands[i].shiftY));
    }

    // Calculate asymmetric padding (intersection area)
    int padLeft = *std::max_element(crop.dx.begin(), crop.dx.end());
    int padRight = -(*std::min_element(crop.dx.begin(), crop.dx.end()));
    int padTop = *std::max_element(crop.dy.begin(), crop.dy.end());
    int padBottom = -(*std::min_element(crop.dy.begin(), crop.dy.end()));
    crop.padLeft = std::max(0, padLeft);
    crop.padRight = std::max(0, padRight);
    crop.padTop = std::max(0, padTop);
    crop.padBottom = std::max(0, padBottom);

    crop.outW = w - crop.padLeft - crop.padRight;
    crop.outH = h - crop.padTop - crop.padBottom;

    LOGD << "Alignment crop: pad L=" << crop.padLeft << " R=" << crop.padRight
         << " T=" << crop.padTop << " B=" << crop.padBottom
         << " => output " << crop.outW << "x" << crop.outH
         << " (from " << w << "x" << h << ")";

    if (crop.outW <= 0 || crop.outH <= 0) {
        LOGD << "Shift too large, output would be empty. Falling back to no-shift merge.";
        return std::nullopt;
    }
    return crop;
}

// The VRT over the inputs cropped to the area every band covers, each band shifted into place
// with -srcwin
SeparateVrt buildAlignedVrt(const std::vector<std::string> &inputPaths, const AlignmentCrop &crop) {
    // Created via GDALTranslate with -srcwin (declared after their files: closed before they go)
    std::vector<VsiMemFile> shiftedFiles;
    std::vector<GDALDatasetUniquePtr> shiftedDatasets;

    for (size_t i = 0; i < inputPaths.size(); i++) {
        const auto src = raster::openInputRaster(inputPaths[i]);
        if (!src) {
            throw GDALException("Cannot open " + inputPaths[i]);
        }
        GDALDatasetH srcDs = datasetHandle(src);

        int srcX = crop.padLeft - crop.dx[i];
        int srcY = crop.padTop - crop.dy[i];

        int srcW = GDALGetRasterXSize(srcDs);
        int srcH = GDALGetRasterYSize(srcDs);
        if (srcX < 0 || srcY < 0 ||
            srcX + crop.outW > srcW || srcY + crop.outH > srcH) {
            LOGD << "Invalid aligned crop window for band " << i
                 << ": srcwin=(" << srcX << ", " << srcY << ", "
                 << crop.outW << ", " << crop.outH << "), dataset=("
                 << srcW << ", " << srcH << ")";
            throw GDALException("Invalid aligned crop window for band " + std::to_string(i));
        }

        VsiMemFile shiftedFile =
            VsiMemFile::unique("shifted_", "_" + std::to_string(i) + ".tif");

        CPLStringList tArgs;
        tArgs.AddString("-srcwin");
        tArgs.AddString(std::to_string(srcX).c_str());
        tArgs.AddString(std::to_string(srcY).c_str());
        tArgs.AddString(std::to_string(crop.outW).c_str());
        tArgs.AddString(std::to_string(crop.outH).c_str());

        const GdalOptionsPtr<GDALTranslateOptions> srcOpts(
            GDALTranslateOptionsNew(tArgs.List(), nullptr));

        auto shiftedDs = adoptDataset(
            GDALTranslate(shiftedFile.c_str(), srcDs, srcOpts.get(), nullptr));
        if (!shiftedDs) {
            throw GDALException("Cannot create shifted dataset for band " + std::to_string(i));
        }

        flushOrThrow(datasetHandle(shiftedDs), "the shifted band " + std::to_string(i));
        shiftedFiles.push_back(std::move(shiftedFile));
        shiftedDatasets.push_back(std::move(shiftedDs));
    }

    // Build VRT from shifted datasets
    return buildSeparateVrt(std::move(shiftedDatasets), std::move(shiftedFiles),
                            "_aligned.vrt", "Cannot build VRT for aligned merge");
}

// What the GPS correction of a cropped merge needs to know about the camera
struct CameraGeometry {
    double pixelPitch = 0, focalLength = 0, yawDeg = 0;
};

// Reads the geometry of the camera from the EXIF/XMP of path (the width of the raster in pixels
// gives the pixel pitch); what cannot be read stays 0
CameraGeometry readCameraGeometry(const std::string &path, int rasterWidth) {
    CameraGeometry camera;

    // A reader that cannot work (no metadata-util) is not a problem of this file and propagates
    const auto parser = readExifParser(io::fromUtf8(path));
    if (!parser) return camera;

    try {
        auto flIt = parser->findXmpKey("Xmp.Camera.PerspectiveFocalLength");
        if (flIt) {
            try { camera.focalLength = std::stod(flIt->toString()); }
            catch (const std::invalid_argument &) { /* non-numeric XMP, ignore */ }
            catch (const std::out_of_range &) { /* out of range, ignore */ }
        }
        if (camera.focalLength <= 0) {
            Focal f;
            if (parser->computeFocal(f)) camera.focalLength = f.length;
        }

        SensorSize ss;
        if (parser->extractSensorSize(ss) && rasterWidth > 0) {
            camera.pixelPitch = ss.width / rasterWidth;
        }

        CameraOrientation cameraOri;
        if (parser->extractCameraOrientation(cameraOri)) {
            camera.yawDeg = cameraOri.yaw;
        }
    } catch (const std::exception &e) {
        LOGD << "GPS correction: failed to read EXIF for shift: " << e.what();
    }
    return camera;
}

// Moves the position by the shift of the center of the crop (the cropped area is not centered on
// the original frame)
void correctGpsForCrop(GeoLocation &gps, const AlignmentCrop &crop, const CameraGeometry &camera) {
    double centerShiftXPx = (crop.padLeft - crop.padRight) / 2.0;
    double centerShiftYPx = (crop.padTop - crop.padBottom) / 2.0;

    if (gps.altitude > 0 && camera.focalLength > 0 && camera.pixelPitch > 0) {
        double gsd = gps.altitude * camera.pixelPitch / camera.focalLength;

        // Rotate pixel shift by camera yaw to convert from
        // image-space (col/row) to geographic (east/north).
        // Image axes: +X = right, +Y = down.
        // Yaw: 0 = north, 90 = east (CW from north).
        // Image-right direction in geo = bearing (yaw+90):
        //   unit vector = (cos(yaw), -sin(yaw))
        // Image-down direction in geo = bearing (yaw+180):
        //   unit vector = (-sin(yaw), -cos(yaw))
        double yawRad = ddb::deg2rad(camera.yawDeg);
        double shiftE =  centerShiftXPx * std::cos(yawRad) - centerShiftYPx * std::sin(yawRad);
        double shiftN = -centerShiftXPx * std::sin(yawRad) - centerShiftYPx * std::cos(yawRad);

        double dLat = (shiftN * gsd) / 111320.0;
        double dLon = (shiftE * gsd) /
            (111320.0 * std::cos(ddb::deg2rad(gps.latitude)));
        gps.latitude += dLat;
        gps.longitude += dLon;
        LOGD << "GPS corrected for alignment crop: dLat=" << dLat
             << " dLon=" << dLon << " yaw=" << camera.yawDeg;
    } else {
        LOGD << "Cannot compute GSD for GPS correction (alt="
             << gps.altitude << " fl=" << camera.focalLength << " pp=" << camera.pixelPitch << ")";
    }
}

// GDALWarp for georeferenced data (supports reprojection)
GDALDatasetUniquePtr warpMerge(GDALDatasetH hVrt, const std::string &outputPath, const MergeEncoding &enc) {
    CPLStringList warpArgs;
    warpArgs.AddString("-of");
    warpArgs.AddString("GTiff");
    warpArgs.AddString("-multi");
    warpArgs.AddString("-wo");
    warpArgs.AddString(enc.numThreads.c_str());
    mergeCreationOptions(enc.dataType, enc.useJpeg, enc.numThreads).appendTo(warpArgs);

    const GdalOptionsPtr<GDALWarpAppOptions> warpOpts(
        GDALWarpAppOptionsNew(warpArgs.List(), nullptr));

    return adoptDataset(GDALWarp(outputPath.c_str(), nullptr, 1, &hVrt, warpOpts.get(), nullptr));
}

// GDALTranslate for non-georeferenced data (no geotransform needed)
GDALDatasetUniquePtr translateMerge(GDALDatasetH hVrt, const std::string &outputPath, const MergeEncoding &enc) {
    const auto transOpts = mergeTranslateOptions(enc.dataType, enc.useJpeg, enc.numThreads);
    return adoptDataset(GDALTranslate(outputPath.c_str(), hVrt, transOpts.get(), nullptr));
}

}  // namespace

void mergeMultispectral(const std::vector<std::string> &inputPaths,
                         const std::string &outputPath) {
    if (inputPaths.size() < 2) throw InvalidArgsException("At least 2 files required");

    if (fs::exists(io::fromUtf8(outputPath))) {
        throw AppException("Output file already exists: " + outputPath);
    }

    auto validation = validateMergeMultispectral(inputPaths);
    if (!validation.ok) {
        std::string errMsg = "Validation failed:";
        for (const auto &e : validation.errors) errMsg += "\n  - " + e;
        throw AppException(errMsg);
    }

    // Read GPS coordinates from the first input file before merge
    // All bands come from the same capture position, so we use the first file
    SourceGps srcGps = readSourceGps(inputPaths[0]);

    // Build VRT with separate=TRUE
    SeparateVrt merged =
        buildSeparateVrt(openAll(inputPaths), {}, ".vrt", "Cannot build VRT for merge");
    GDALDatasetH hVrt = merged.handle();

    // Check if inputs have geotransform
    double checkGt[6];
    bool hasGeoTransform = (GDALGetGeoTransform(hVrt, checkGt) == CE_None);

    // Determine compression
    GDALDataType dt = GDALGetRasterDataType(GDALGetRasterBand(hVrt, 1));
    int nBands = GDALGetRasterCount(hVrt);
    bool useJpeg = (dt == GDT_Byte && (nBands == 3 || nBands == 4));

    // Warp and compression threads: this one and those the CPU pool allows, held
    // until the output is closed
    const auto cpu = parallel::tryAcquireThreads(parallel::hardwareThreads());
    const MergeEncoding encoding{dt, useJpeg, parallel::gdalNumThreads(cpu)};
    GDALDatasetUniquePtr out;

    if (hasGeoTransform) {
        out = warpMerge(hVrt, outputPath, encoding);
    } else {
        LOGD << "No geotransform found, using GDALTranslate for merge";

        // Check if alignment shift should be applied
        std::optional<AlignmentCrop> crop;
        if (validation.alignment.correctionApplied)
            crop = planAlignmentCrop(validation);

        if (crop) {
            // Close the original VRT and datasets; the shifted ones replace them
            merged.close();

            SeparateVrt aligned = buildAlignedVrt(inputPaths, *crop);
            out = translateMerge(aligned.handle(), outputPath, encoding);
            aligned.close();

            // GPS correction for center shift
            if (srcGps.present) {
                const auto camera = readCameraGeometry(inputPaths[0], validation.summary.width);
                correctGpsForCrop(srcGps.location, *crop, camera);
            }
        } else {
            // Standard non-shifted merge path
            out = translateMerge(merged.handle(), outputPath, encoding);
        }
    }

    if (!out) {
        throw GDALException("Cannot create merged output: " + outputPath);
    }

    closeOutputOrThrow(std::move(out), outputPath);

    // A no-op when the shift path already closed them
    merged.close();

    // Write GPS coordinates from source to merged output. A reader that cannot work (no metadata-util)
    // propagates; a file that cannot be written does not fail the merge.
    if (srcGps.present) {
        try {
            ExifEditor editor(outputPath);
            const auto failures = editor.SetGPS(srcGps.location.latitude, srcGps.location.longitude, srcGps.location.altitude);
            if (failures.empty())
                LOGD << "GPS coordinates written to merged output: " << outputPath;
            else
                LOGD << "Could not write GPS to " << outputPath << ": " << failures.front().message;
        } catch (const HelperUnavailableException &) {
            throw;
        } catch (const std::exception &e) {
            LOGD << "Could not write GPS to " << outputPath << ": " << e.what();
        }
    }

    LOGD << "Merged " << inputPaths.size() << " bands into " << outputPath;
}

} // namespace ddb
