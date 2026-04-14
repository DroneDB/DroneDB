/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "merge_multispectral.h"
#include "gdal_inc.h"
#include "exceptions.h"
#include "logger.h"
#include "mio.h"
#include "utils.h"
#include "exif.h"
#include "exifeditor.h"
#include "sensor_data.h"

#include <cmath>
#include <sstream>
#include <algorithm>
#include <numeric>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace ddb {

static std::string gdalTypeName(GDALDataType dt) {
    switch (dt) {
        case GDT_Byte: return "Byte";
        case GDT_UInt16: return "UInt16";
        case GDT_Int16: return "Int16";
        case GDT_UInt32: return "UInt32";
        case GDT_Int32: return "Int32";
        case GDT_Float32: return "Float32";
        case GDT_Float64: return "Float64";
        default: return "Unknown";
    }
}

static bool parsePrincipalPoint(const std::string &ppStr, double &cx, double &cy) {
    auto pos = ppStr.find(',');
    if (pos == std::string::npos) return false;
    try {
        cx = std::stod(ppStr.substr(0, pos));
        cy = std::stod(ppStr.substr(pos + 1));
        return true;
    } catch (...) {
        return false;
    }
}

std::vector<BandAlignmentInfo> detectBandAlignment(const std::vector<std::string> &inputPaths) {
    const size_t N = inputPaths.size();
    std::vector<BandAlignmentInfo> alignInfo(N);

    if (N < 2) return alignInfo;

    struct BandData {
        double ppMmX = 0, ppMmY = 0;
        bool hasPP = false;
        double pixelPitchX = 0, pixelPitchY = 0;
        bool hasPixelPitch = false;
        double focalLengthMm = 0;
        int centralWavelength = 0;
        std::string bandName;
        int rigCameraIndex = -1;
        int imageWidth = 0, imageHeight = 0;
        double relOptCenterX = 0, relOptCenterY = 0;
        bool hasRelOptCenter = false;
    };

    std::vector<BandData> bands(N);

    for (size_t i = 0; i < N; i++) {
        try {
            auto exivImage = Exiv2::ImageFactory::open(inputPaths[i]);
            if (!exivImage.get()) continue;
            exivImage->readMetadata();
            ExifParser parser(exivImage.get());

            auto imgSize = parser.extractImageSize();
            bands[i].imageWidth = imgSize.width;
            bands[i].imageHeight = imgSize.height;
            alignInfo[i].imageWidth = imgSize.width;
            alignInfo[i].imageHeight = imgSize.height;

            // Read BandName
            auto bandNameIt = parser.findXmpKey("Xmp.Camera.BandName");
            if (bandNameIt != parser.xmpEnd()) {
                bands[i].bandName = bandNameIt->toString();
                alignInfo[i].bandName = bands[i].bandName;
            }

            // Read CentralWavelength
            auto cwIt = parser.findXmpKey("Xmp.Camera.CentralWavelength");
            if (cwIt != parser.xmpEnd()) {
                try { bands[i].centralWavelength = std::stoi(cwIt->toString()); } catch (...) {}
                alignInfo[i].centralWavelength = bands[i].centralWavelength;
            }

            // Detect thermal band
            if (bands[i].bandName == "LWIR" || bands[i].centralWavelength > 7000) {
                alignInfo[i].isThermal = true;
            }

            // Read RigCameraIndex
            auto rigIt = parser.findXmpKey("Xmp.Camera.RigCameraIndex");
            if (rigIt != parser.xmpEnd()) {
                try { bands[i].rigCameraIndex = std::stoi(rigIt->toString()); } catch (...) {}
            }

            // Read PrincipalPoint (mm)
            auto ppIt = parser.findXmpKey("Xmp.Camera.PrincipalPoint");
            if (ppIt != parser.xmpEnd()) {
                std::string ppStr = ppIt->toString();
                if (parsePrincipalPoint(ppStr, bands[i].ppMmX, bands[i].ppMmY)) {
                    bands[i].hasPP = true;
                }
            }

            // Read PerspectiveFocalLength (mm)
            auto flIt = parser.findXmpKey("Xmp.Camera.PerspectiveFocalLength");
            if (flIt != parser.xmpEnd()) {
                try { bands[i].focalLengthMm = std::stod(flIt->toString()); } catch (...) {}
            }

            // Read DJI RelativeOpticalCenter
            auto rocXIt = parser.findXmpKey("Xmp.drone-dji.RelativeOpticalCenterX");
            auto rocYIt = parser.findXmpKey("Xmp.drone-dji.RelativeOpticalCenterY");
            if (rocXIt != parser.xmpEnd() && rocYIt != parser.xmpEnd()) {
                try {
                    bands[i].relOptCenterX = std::stod(rocXIt->toString());
                    bands[i].relOptCenterY = std::stod(rocYIt->toString());
                    bands[i].hasRelOptCenter = true;
                } catch (...) {}
            }

            // --- Pixel Pitch derivation (Priority chain) ---

            // Priority 1: FocalPlaneResolution
            auto fUnit = parser.findExifKey("Exif.Photo.FocalPlaneResolutionUnit");
            auto fXRes = parser.findExifKey("Exif.Photo.FocalPlaneXResolution");
            auto fYRes = parser.findExifKey("Exif.Photo.FocalPlaneYResolution");

            if (fUnit != parser.exifEnd() &&
                fXRes != parser.exifEnd() &&
                fYRes != parser.exifEnd()) {
                long unit = fUnit->toInt64();
                double rx = fXRes->toFloat();
                double ry = fYRes->toFloat();
                if (rx > 0 && ry > 0) {
                    double mmPerUnit = parser.getMmPerUnit(unit);
                    if (mmPerUnit > 0) {
                        bands[i].pixelPitchX = mmPerUnit / rx;
                        bands[i].pixelPitchY = mmPerUnit / ry;
                        bands[i].hasPixelPitch = true;
                        LOGD << "Band " << i << " pixel pitch from FocalPlaneRes: "
                             << bands[i].pixelPitchX * 1000.0 << " um x "
                             << bands[i].pixelPitchY * 1000.0 << " um";
                    }
                }
            }

            // Priority 2: CalibratedFocalLength (px) + PerspectiveFocalLength (mm)
            if (!bands[i].hasPixelPitch && bands[i].focalLengthMm > 0) {
                auto cflIt = parser.findXmpKey("Xmp.drone-dji.CalibratedFocalLength");
                if (cflIt != parser.xmpEnd()) {
                    try {
                        double cfl = std::stod(cflIt->toString());
                        if (cfl > 0) {
                            double pp = bands[i].focalLengthMm / cfl;
                            bands[i].pixelPitchX = pp;
                            bands[i].pixelPitchY = pp;
                            bands[i].hasPixelPitch = true;
                            LOGD << "Band " << i << " pixel pitch from CalibratedFocalLength: "
                                 << pp * 1000.0 << " um";
                        }
                    } catch (...) {}
                }
            }

            // Priority 3: Sensor database lookup
            if (!bands[i].hasPixelPitch && bands[i].imageWidth > 0 && bands[i].imageHeight > 0) {
                std::string sensor = parser.extractSensor();
                if (SensorData::contains(sensor)) {
                    double sensorWidthMm = SensorData::getFocal(sensor);
                    if (sensorWidthMm > 0) {
                        bands[i].pixelPitchX = sensorWidthMm / bands[i].imageWidth;
                        double aspect = static_cast<double>(bands[i].imageWidth) / bands[i].imageHeight;
                        double sensorHeightMm = sensorWidthMm / aspect;
                        bands[i].pixelPitchY = sensorHeightMm / bands[i].imageHeight;
                        bands[i].hasPixelPitch = true;
                        LOGD << "Band " << i << " pixel pitch from sensor DB (" << sensor << "): "
                             << bands[i].pixelPitchX * 1000.0 << " um";
                    }
                }
            }

            // Priority 4: FocalLength + FocalLength35mmEquiv
            if (!bands[i].hasPixelPitch && bands[i].imageWidth > 0) {
                Focal f;
                if (parser.computeFocal(f) && f.length > 0 && f.length35 > 0) {
                    double sensorWidthMm = f.length * 36.0 / f.length35;
                    bands[i].pixelPitchX = sensorWidthMm / bands[i].imageWidth;
                    bands[i].pixelPitchY = bands[i].pixelPitchX;
                    bands[i].hasPixelPitch = true;
                    LOGD << "Band " << i << " pixel pitch from FocalLength35mm: "
                         << bands[i].pixelPitchX * 1000.0 << " um (square pixel assumed)";
                }
            }

            // Do not mark detected here; detection status is set later
            // when a shift source is actually selected and shifts computed.
            // alignInfo[i].detected stays false (default) until then.

            // Log lens model
            auto modelTypeIt = parser.findXmpKey("Xmp.Camera.ModelType");
            if (modelTypeIt != parser.xmpEnd()) {
                LOGD << "Band " << alignInfo[i].bandName << ": lens model = " << modelTypeIt->toString();
            }

            // Log Sentera tags (not used)
            auto alignMatrixIt = parser.findXmpKey("Xmp.Sentera.AlignMatrix");
            if (alignMatrixIt != parser.xmpEnd()) {
                LOGD << "Band " << alignInfo[i].bandName << ": Sentera AlignMatrix = "
                     << alignMatrixIt->toString() << " (not used)";
            }

            // Log RigRelatives (not used)
            auto rigRelIt = parser.findXmpKey("Xmp.MicaSense.RigRelatives");
            if (rigRelIt != parser.xmpEnd()) {
                LOGD << "Band " << alignInfo[i].bandName << ": RigRelatives = "
                     << rigRelIt->toString() << " (not used)";
            }

        } catch (const Exiv2::Error &e) {
            LOGD << "Could not read EXIF/XMP from " << inputPaths[i] << ": " << e.what();
            continue;
        }
    }

    // Select reference band: prefer Green
    size_t REF = 0;
    for (size_t i = 0; i < N; i++) {
        if (bands[i].bandName == "Green" ||
            (bands[i].centralWavelength >= 540 && bands[i].centralWavelength <= 570)) {
            REF = i;
            break;
        }
    }
    LOGD << "Alignment reference band: " << REF << " (" << bands[REF].bandName << ")";

    // Calculate max PP shift
    double maxPPShift = 0;
    bool allHavePPAndPitch = true;
    for (size_t i = 0; i < N; i++) {
        if (!bands[i].hasPP || !bands[i].hasPixelPitch) {
            allHavePPAndPitch = false;
            continue;
        }
        if (i == REF) continue;
        if (!bands[REF].hasPP || !bands[REF].hasPixelPitch) {
            allHavePPAndPitch = false;
            continue;
        }
        double deltaCxMm = bands[i].ppMmX - bands[REF].ppMmX;
        double deltaCyMm = bands[i].ppMmY - bands[REF].ppMmY;
        double sx = deltaCxMm / bands[i].pixelPitchX;
        double sy = deltaCyMm / bands[i].pixelPitchY;
        maxPPShift = std::max(maxPPShift, std::max(std::abs(sx), std::abs(sy)));
    }

    // Check DJI RelOC availability
    bool hasDjiRelOC = false;
    for (size_t i = 0; i < N; i++) {
        if (bands[i].hasRelOptCenter) { hasDjiRelOC = true; break; }
    }

    // Source selection
    enum ShiftSource { SOURCE_NONE, SOURCE_PRINCIPAL_POINT, SOURCE_DJI_RELATIVE_OC };
    ShiftSource useSource = SOURCE_NONE;

    if (maxPPShift > 1.5 && allHavePPAndPitch) {
        useSource = SOURCE_PRINCIPAL_POINT;
    } else if (hasDjiRelOC) {
        useSource = SOURCE_DJI_RELATIVE_OC;
    }

    // Apply selected source
    if (useSource == SOURCE_PRINCIPAL_POINT) {
        for (size_t i = 0; i < N; i++) {
            if (!bands[i].hasPP || !bands[i].hasPixelPitch) continue;
            if (!bands[REF].hasPP || !bands[REF].hasPixelPitch) continue;
            if (i == REF) {
                alignInfo[i].shiftX = 0;
                alignInfo[i].shiftY = 0;
                alignInfo[i].shiftSource = "PrincipalPoint";
                alignInfo[i].detected = true;
                continue;
            }
            double deltaCxMm = bands[i].ppMmX - bands[REF].ppMmX;
            double deltaCyMm = bands[i].ppMmY - bands[REF].ppMmY;
            alignInfo[i].shiftX = deltaCxMm / bands[i].pixelPitchX;
            alignInfo[i].shiftY = deltaCyMm / bands[i].pixelPitchY;
            alignInfo[i].shiftSource = "PrincipalPoint";
            alignInfo[i].detected = true;
        }
    } else if (useSource == SOURCE_DJI_RELATIVE_OC) {
        // Find a reference band that actually has RelOC metadata
        size_t djiRef = REF;
        if (!bands[djiRef].hasRelOptCenter) {
            for (size_t i = 0; i < N; i++) {
                if (bands[i].hasRelOptCenter) { djiRef = i; break; }
            }
        }
        if (!bands[djiRef].hasRelOptCenter) {
            LOGD << "No band has DJI RelativeOpticalCenter metadata, skipping";
        } else {
            for (size_t i = 0; i < N; i++) {
                if (!bands[i].hasRelOptCenter) continue;
                double rocX = bands[i].relOptCenterX - bands[djiRef].relOptCenterX;
                double rocY = bands[i].relOptCenterY - bands[djiRef].relOptCenterY;
                alignInfo[i].shiftX = rocX;
                alignInfo[i].shiftY = rocY;
                alignInfo[i].shiftSource = "DJI_RelativeOpticalCenter";
                alignInfo[i].detected = true;
            }
        }
    }

    // Validate plausible shifts
    const double MAX_PLAUSIBLE_SHIFT_RATIO = 0.10;
    for (size_t i = 0; i < N; i++) {
        if (!alignInfo[i].detected) continue;
        double maxDimPx = static_cast<double>(std::max(alignInfo[i].imageWidth, alignInfo[i].imageHeight));
        double maxShiftPx = MAX_PLAUSIBLE_SHIFT_RATIO * maxDimPx;
        if (std::abs(alignInfo[i].shiftX) > maxShiftPx ||
            std::abs(alignInfo[i].shiftY) > maxShiftPx) {
            LOGD << "Implausible shift for band " << alignInfo[i].bandName
                 << ": (" << alignInfo[i].shiftX << ", " << alignInfo[i].shiftY << ") px — ignoring";
            alignInfo[i].detected = false;
            alignInfo[i].shiftX = 0;
            alignInfo[i].shiftY = 0;
        }
    }

    // Log results
    for (size_t i = 0; i < N; i++) {
        LOGD << "Band " << i << " (" << alignInfo[i].bandName << ")"
             << " detected=" << alignInfo[i].detected
             << " shift=(" << alignInfo[i].shiftX << ", " << alignInfo[i].shiftY << ") px"
             << " source=" << alignInfo[i].shiftSource;
    }

    return alignInfo;
}

MergeValidationResult validateMergeMultispectral(const std::vector<std::string> &inputPaths) {
    MergeValidationResult result;

    if (inputPaths.size() < 2) {
        result.errors.push_back("At least 2 files required");
        return result;
    }

    // Build display names (filename only) for cleaner messages
    std::vector<std::string> displayNames;
    displayNames.reserve(inputPaths.size());
    for (const auto &p : inputPaths) {
        displayNames.push_back(fs::path(p).filename().string());
    }

    // Open first file as reference
    GDALDatasetH hRef = GDALOpen(inputPaths[0].c_str(), GA_ReadOnly);
    if (!hRef) {
        result.errors.push_back("Cannot open " + displayNames[0]);
        return result;
    }

    int refWidth = GDALGetRasterXSize(hRef);
    int refHeight = GDALGetRasterYSize(hRef);
    GDALDataType refType = GDALGetRasterDataType(GDALGetRasterBand(hRef, 1));

    double refGt[6] = {0, 1, 0, 0, 0, -1};
    bool hasGeoTransform = (GDALGetGeoTransform(hRef, refGt) == CE_None);
    if (!hasGeoTransform) {
        result.warnings.push_back("Cannot read geotransform from reference file: " + displayNames[0]);
    }

    const char *refProj = GDALGetProjectionRef(hRef);
    OGRSpatialReferenceH refSrs = OSRNewSpatialReference(refProj);

    result.summary.crs = refProj ? std::string(refProj) : "";
    result.summary.width = refWidth;
    result.summary.height = refHeight;
    result.summary.pixelSizeX = refGt[1];
    result.summary.pixelSizeY = refGt[5];
    result.summary.dataType = gdalTypeName(refType);
    result.summary.totalBands = 0;

    // Check skew on reference
    if (refGt[2] != 0 || refGt[4] != 0) {
        result.errors.push_back("Reference file has non-zero skew (rotation not supported)");
    }

    int totalBands = GDALGetRasterCount(hRef);

    if (GDALGetRasterCount(hRef) > 1) {
        result.warnings.push_back(displayNames[0] + " has " + std::to_string(GDALGetRasterCount(hRef)) + " bands");
    }

    for (size_t i = 1; i < inputPaths.size(); i++) {
        GDALDatasetH hDs = GDALOpen(inputPaths[i].c_str(), GA_ReadOnly);
        if (!hDs) {
            result.errors.push_back("Cannot open " + displayNames[i]);
            continue;
        }

        // CRS check
        const char *proj = GDALGetProjectionRef(hDs);
        OGRSpatialReferenceH srs = OSRNewSpatialReference(proj);
        if (!OSRIsSame(refSrs, srs)) {
            result.errors.push_back("CRS mismatch: " + displayNames[i]);
        }
        OSRDestroySpatialReference(srs);

        // Dimension check
        if (GDALGetRasterXSize(hDs) != refWidth || GDALGetRasterYSize(hDs) != refHeight) {
            result.errors.push_back("Dimension mismatch: " + displayNames[i] +
                " (" + std::to_string(GDALGetRasterXSize(hDs)) + "x" + std::to_string(GDALGetRasterYSize(hDs)) +
                " vs " + std::to_string(refWidth) + "x" + std::to_string(refHeight) + ")");
        }

        // Data type check
        GDALDataType dt = GDALGetRasterDataType(GDALGetRasterBand(hDs, 1));
        if (dt != refType) {
            result.errors.push_back("Data type mismatch: " + displayNames[i] +
                " (" + gdalTypeName(dt) + " vs " + gdalTypeName(refType) + ")");
        }

        // Geotransform check
        double gt[6];
        if (GDALGetGeoTransform(hDs, gt) != CE_None) {
            result.warnings.push_back("Cannot read geotransform from: " + displayNames[i]);
        } else {
            // Skew check
            if (gt[2] != 0 || gt[4] != 0) {
                result.errors.push_back(displayNames[i] + " has non-zero skew");
            }

            // Pixel size tolerance (relative)
            double pxDiffX = std::abs(gt[1] - refGt[1]);
            double pxDiffY = std::abs(gt[5] - refGt[5]);
            double relDiffX = (std::abs(refGt[1]) > 0) ? pxDiffX / std::abs(refGt[1]) : 0;
            double relDiffY = (std::abs(refGt[5]) > 0) ? pxDiffY / std::abs(refGt[5]) : 0;

            if (relDiffX > 0.05 || relDiffY > 0.05) {
                result.errors.push_back("Pixel size mismatch > 5%: " + displayNames[i]);
            } else if (relDiffX > 0.01 || relDiffY > 0.01) {
                result.warnings.push_back("Resolution differs slightly for " + displayNames[i] + ", bands will be resampled");
            }

            // Origin tolerance (1 pixel)
            double origDiffX = std::abs(gt[0] - refGt[0]);
            double origDiffY = std::abs(gt[3] - refGt[3]);
            if (origDiffX > std::abs(refGt[1]) || origDiffY > std::abs(refGt[5])) {
                result.errors.push_back("Origin mismatch > 1 pixel: " + displayNames[i]);
            }
        }

        if (GDALGetRasterCount(hDs) > 1) {
            result.warnings.push_back(displayNames[i] + " has " + std::to_string(GDALGetRasterCount(hDs)) + " bands");
        }

        totalBands += GDALGetRasterCount(hDs);
        GDALClose(hDs);
    }

    OSRDestroySpatialReference(refSrs);
    GDALClose(hRef);

    result.summary.totalBands = totalBands;
    result.summary.estimatedSize = static_cast<size_t>(refWidth) * refHeight * totalBands * GDALGetDataTypeSizeBytes(refType);
    result.ok = result.errors.empty();

    // Band alignment detection
    auto alignInfo = detectBandAlignment(inputPaths);

    int detectedCount = 0;
    for (const auto &a : alignInfo) {
        if (a.detected) detectedCount++;
    }

    if (detectedCount >= 2) {
        result.alignment.detected = true;

        double maxShift = 0;
        std::string source;
        for (const auto &a : alignInfo) {
            if (!a.detected) continue;
            double s = std::max(std::abs(a.shiftX), std::abs(a.shiftY));
            if (s > maxShift) maxShift = s;
            if (source.empty() && !a.shiftSource.empty()) source = a.shiftSource;
        }
        result.alignment.maxShiftPixels = maxShift;
        result.alignment.shiftSource = source;
        result.alignment.bands = alignInfo;

        // Check if files are already georeferenced (have valid CRS + non-identity geotransform)
        bool isGeoreferenced = hasGeoTransform &&
            refProj != nullptr && std::string(refProj).length() > 0 &&
            !(refGt[0] == 0 && refGt[1] == 1 && refGt[2] == 0 &&
              refGt[3] == 0 && refGt[4] == 0 && refGt[5] == -1);

        // Check all same dimensions
        bool allSameDims = true;
        for (const auto &a : alignInfo) {
            if (a.imageWidth != refWidth || a.imageHeight != refHeight) {
                allSameDims = false;
                break;
            }
        }

        // Check no thermal with different resolution
        bool hasThermalDiffRes = false;
        for (const auto &a : alignInfo) {
            if (a.isThermal && (a.imageWidth != refWidth || a.imageHeight != refHeight)) {
                hasThermalDiffRes = true;
                break;
            }
        }

        // Determine if correction should be applied
        result.alignment.correctionApplied = !isGeoreferenced &&
            detectedCount >= 2 &&
            maxShift > 0.5 &&
            allSameDims &&
            !hasThermalDiffRes;

        // Warnings
        if (maxShift > 2.0) {
            int shiftRounded = static_cast<int>(std::round(maxShift));
            result.warnings.push_back(
                "Band misalignment detected (~" + std::to_string(shiftRounded) +
                " pixels). Bands from multi-camera sensors (e.g. MicaSense, DJI Multispectral) "
                "are captured from slightly different viewpoints. For accurate vegetation indices "
                "(NDVI, NDRE, etc.), process raw imagery with a photogrammetry tool (e.g. OpenDroneMap, "
                "Pix4D) before merging. The merge will proceed with approximate pixel-shift correction.");
        } else if (maxShift > 0.5) {
            int shiftRounded = static_cast<int>(std::round(maxShift));
            result.warnings.push_back(
                "Minor band misalignment detected (~" + std::to_string(shiftRounded) +
                " pixels). Approximate correction will be applied.");
        }

        // Partial metadata warning
        if (detectedCount > 0 && detectedCount < static_cast<int>(alignInfo.size())) {
            result.warnings.push_back(
                "Band alignment metadata found for " + std::to_string(detectedCount) + "/" +
                std::to_string(alignInfo.size()) + " bands. Correction will only be applied to "
                "bands with calibration data. Results may be inconsistent.");
        }

        // LWIR warning
        for (const auto &a : alignInfo) {
            if (a.isThermal) {
                result.warnings.push_back(
                    "Thermal (LWIR) band detected. Thermal bands have different optics and cannot "
                    "be aligned using PrincipalPoint shift. Consider excluding the thermal band from the merge.");
                break;
            }
        }
    }

    return result;
}

void previewMergeMultispectral(const std::vector<std::string> &inputPaths,
                                const std::vector<int> &previewBands,
                                int thumbSize,
                                uint8_t **outBuffer, int *outBufferSize) {
    if (inputPaths.empty()) throw InvalidArgsException("No input paths");
    if (previewBands.size() < 3) throw InvalidArgsException("Need at least 3 preview bands");

    // Build VRT with separate=TRUE
    char **vrtArgs = nullptr;
    vrtArgs = CSLAddString(vrtArgs, "-separate");
    vrtArgs = CSLAddString(vrtArgs, "-r");
    vrtArgs = CSLAddString(vrtArgs, "average");

    GDALBuildVRTOptions *vrtOpts = GDALBuildVRTOptionsNew(vrtArgs, nullptr);
    CSLDestroy(vrtArgs);

    std::vector<GDALDatasetH> datasets;
    for (const auto &p : inputPaths) {
        GDALDatasetH ds = GDALOpen(p.c_str(), GA_ReadOnly);
        if (!ds) {
            for (auto d : datasets) GDALClose(d);
            GDALBuildVRTOptionsFree(vrtOpts);
            throw GDALException("Cannot open " + p);
        }
        datasets.push_back(ds);
    }

    std::string vsiVrtPath = "/vsimem/" + utils::generateRandomString(16) + ".vrt";
    GDALDatasetH hVrt = GDALBuildVRT(vsiVrtPath.c_str(), static_cast<int>(datasets.size()),
                                      datasets.data(), nullptr, vrtOpts, nullptr);
    GDALBuildVRTOptionsFree(vrtOpts);

    if (!hVrt) {
        for (auto d : datasets) GDALClose(d);
        throw GDALException("Cannot build VRT");
    }
    GDALFlushCache(hVrt);

    // Translate with band selection and scaling
    char **tArgs = nullptr;
    tArgs = CSLAddString(tArgs, "-of");
    tArgs = CSLAddString(tArgs, "WEBP");
    tArgs = CSLAddString(tArgs, "-outsize");
    tArgs = CSLAddString(tArgs, std::to_string(thumbSize).c_str());
    tArgs = CSLAddString(tArgs, std::to_string(thumbSize).c_str());
    tArgs = CSLAddString(tArgs, "-ot");
    tArgs = CSLAddString(tArgs, "Byte");
    tArgs = CSLAddString(tArgs, "-scale");

    for (int band : previewBands) {
        tArgs = CSLAddString(tArgs, "-b");
        tArgs = CSLAddString(tArgs, std::to_string(band).c_str());
    }

    GDALTranslateOptions *transOpts = GDALTranslateOptionsNew(tArgs, nullptr);
    CSLDestroy(tArgs);

    std::string vsiOutPath = "/vsimem/" + utils::generateRandomString(16) + ".webp";
    GDALDatasetH hOut = GDALTranslate(vsiOutPath.c_str(), hVrt, transOpts, nullptr);
    GDALTranslateOptionsFree(transOpts);

    if (!hOut) {
        GDALClose(hVrt);
        for (auto d : datasets) GDALClose(d);
        VSIUnlink(vsiVrtPath.c_str());
        throw GDALException("Cannot generate preview");
    }

    GDALFlushCache(hOut);
    GDALClose(hOut);
    GDALClose(hVrt);
    for (auto d : datasets) GDALClose(d);

    // Read memory buffer
    vsi_l_offset bufSize;
    *outBuffer = VSIGetMemFileBuffer(vsiOutPath.c_str(), &bufSize, TRUE);
    *outBufferSize = static_cast<int>(bufSize);

    VSIUnlink(vsiVrtPath.c_str());
}

void mergeMultispectral(const std::vector<std::string> &inputPaths,
                         const std::string &outputPath) {
    if (inputPaths.size() < 2) throw InvalidArgsException("At least 2 files required");

    if (fs::exists(outputPath)) {
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
    GeoLocation srcGps;
    bool hasGps = false;
    try {
        auto exivImage = Exiv2::ImageFactory::open(inputPaths[0]);
        if (exivImage.get()) {
            exivImage->readMetadata();
            ExifParser parser(exivImage.get());
            hasGps = parser.extractGeo(srcGps);
            if (hasGps) {
                LOGD << "Read GPS from " << inputPaths[0]
                     << ": lat=" << srcGps.latitude
                     << " lon=" << srcGps.longitude
                     << " alt=" << srcGps.altitude;
            }
        }
    } catch (const Exiv2::Error &e) {
        LOGD << "Could not read EXIF GPS from " << inputPaths[0] << ": " << e.what();
    }

    // Build VRT with separate=TRUE
    char **vrtArgs = nullptr;
    vrtArgs = CSLAddString(vrtArgs, "-separate");
    vrtArgs = CSLAddString(vrtArgs, "-r");
    vrtArgs = CSLAddString(vrtArgs, "average");

    GDALBuildVRTOptions *vrtOpts = GDALBuildVRTOptionsNew(vrtArgs, nullptr);
    CSLDestroy(vrtArgs);

    std::vector<GDALDatasetH> datasets;
    for (const auto &p : inputPaths) {
        GDALDatasetH ds = GDALOpen(p.c_str(), GA_ReadOnly);
        if (!ds) {
            for (auto d : datasets) GDALClose(d);
            GDALBuildVRTOptionsFree(vrtOpts);
            throw GDALException("Cannot open " + p);
        }
        datasets.push_back(ds);
    }

    std::string vsiVrtPath = "/vsimem/" + utils::generateRandomString(16) + ".vrt";
    GDALDatasetH hVrt = GDALBuildVRT(vsiVrtPath.c_str(), static_cast<int>(datasets.size()),
                                      datasets.data(), nullptr, vrtOpts, nullptr);
    GDALBuildVRTOptionsFree(vrtOpts);

    if (!hVrt) {
        for (auto d : datasets) GDALClose(d);
        throw GDALException("Cannot build VRT for merge");
    }
    GDALFlushCache(hVrt);

    // Check if inputs have geotransform
    double checkGt[6];
    bool hasGeoTransform = (GDALGetGeoTransform(hVrt, checkGt) == CE_None);

    // Determine compression
    GDALDataType dt = GDALGetRasterDataType(GDALGetRasterBand(hVrt, 1));
    int nBands = GDALGetRasterCount(hVrt);
    bool useJpeg = (dt == GDT_Byte && (nBands == 3 || nBands == 4));

    GDALDatasetH hOut = nullptr;

    // Determine predictor type based on data type
    // PREDICTOR=2 (horizontal differencing) for integer types
    // PREDICTOR=3 (floating point) for float types
    const char *predictor = (dt == GDT_Float32 || dt == GDT_Float64) ? "PREDICTOR=3" : "PREDICTOR=2";

    if (hasGeoTransform) {
        // Use GDALWarp for georeferenced data (supports reprojection)
        char **warpArgs = nullptr;
        warpArgs = CSLAddString(warpArgs, "-of");
        warpArgs = CSLAddString(warpArgs, "GTiff");
        warpArgs = CSLAddString(warpArgs, "-multi");
        warpArgs = CSLAddString(warpArgs, "-wo");
        warpArgs = CSLAddString(warpArgs, "NUM_THREADS=ALL_CPUS");
        warpArgs = CSLAddString(warpArgs, "-co");
        warpArgs = CSLAddString(warpArgs, "NUM_THREADS=ALL_CPUS");
        warpArgs = CSLAddString(warpArgs, "-co");
        warpArgs = CSLAddString(warpArgs, "TILED=YES");
        warpArgs = CSLAddString(warpArgs, "-co");
        warpArgs = CSLAddString(warpArgs, "BIGTIFF=IF_SAFER");
        warpArgs = CSLAddString(warpArgs, "-co");
        warpArgs = CSLAddString(warpArgs, predictor);

        if (useJpeg) {
            warpArgs = CSLAddString(warpArgs, "-co");
            warpArgs = CSLAddString(warpArgs, "COMPRESS=JPEG");
            warpArgs = CSLAddString(warpArgs, "-co");
            warpArgs = CSLAddString(warpArgs, "QUALITY=90");
        } else {
            warpArgs = CSLAddString(warpArgs, "-co");
            warpArgs = CSLAddString(warpArgs, "COMPRESS=LZW");
        }

        GDALWarpAppOptions *warpOpts = GDALWarpAppOptionsNew(warpArgs, nullptr);
        CSLDestroy(warpArgs);

        hOut = GDALWarp(outputPath.c_str(), nullptr, 1, &hVrt, warpOpts, nullptr);
        GDALWarpAppOptionsFree(warpOpts);
    } else {
        // Use GDALTranslate for non-georeferenced data (no geotransform needed)
        LOGD << "No geotransform found, using GDALTranslate for merge";

        // Check if alignment shift should be applied
        bool applyShift = validation.alignment.correctionApplied;

        if (applyShift) {
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

            std::vector<int> dxVec(N, 0), dyVec(N, 0);
            for (size_t i = 0; i < N; i++) {
                if (i == REF || !alignBands[i].detected || alignBands[i].isThermal) continue;
                dxVec[i] = static_cast<int>(std::round(alignBands[i].shiftX));
                dyVec[i] = static_cast<int>(std::round(alignBands[i].shiftY));
            }

            // Calculate asymmetric padding (intersection area)
            int padLeft = *std::max_element(dxVec.begin(), dxVec.end());
            int padRight = -(*std::min_element(dxVec.begin(), dxVec.end()));
            int padTop = *std::max_element(dyVec.begin(), dyVec.end());
            int padBottom = -(*std::min_element(dyVec.begin(), dyVec.end()));
            padLeft = std::max(0, padLeft);
            padRight = std::max(0, padRight);
            padTop = std::max(0, padTop);
            padBottom = std::max(0, padBottom);

            int outW = w - padLeft - padRight;
            int outH = h - padTop - padBottom;

            LOGD << "Alignment crop: pad L=" << padLeft << " R=" << padRight
                 << " T=" << padTop << " B=" << padBottom
                 << " => output " << outW << "x" << outH
                 << " (from " << w << "x" << h << ")";

            if (outW <= 0 || outH <= 0) {
                LOGD << "Shift too large, output would be empty. Falling back to no-shift merge.";
                applyShift = false;
            } else {
                // Close the original VRT and datasets; we'll create shifted ones
                GDALClose(hVrt);
                for (auto d : datasets) GDALClose(d);
                VSIUnlink(vsiVrtPath.c_str());
                datasets.clear();

                // Create shifted datasets via GDALTranslate with -srcwin
                std::vector<GDALDatasetH> shiftedDatasets;
                std::vector<std::string> shiftedPaths;

                for (size_t i = 0; i < N; i++) {
                    GDALDatasetH srcDs = GDALOpen(inputPaths[i].c_str(), GA_ReadOnly);
                    if (!srcDs) {
                        for (auto d : shiftedDatasets) GDALClose(d);
                        for (const auto &p : shiftedPaths) VSIUnlink(p.c_str());
                        throw GDALException("Cannot open " + inputPaths[i]);
                    }

                    int srcX = padLeft - dxVec[i];
                    int srcY = padTop - dyVec[i];

                    int srcW = GDALGetRasterXSize(srcDs);
                    int srcH = GDALGetRasterYSize(srcDs);
                    if (srcX < 0 || srcY < 0 ||
                        srcX + outW > srcW || srcY + outH > srcH) {
                        LOGD << "Invalid aligned crop window for band " << i
                             << ": srcwin=(" << srcX << ", " << srcY << ", "
                             << outW << ", " << outH << "), dataset=("
                             << srcW << ", " << srcH << ")";
                        GDALClose(srcDs);
                        for (auto d : shiftedDatasets) GDALClose(d);
                        for (const auto &p : shiftedPaths) VSIUnlink(p.c_str());
                        throw GDALException("Invalid aligned crop window for band " + std::to_string(i));
                    }

                    std::string shiftedPath = "/vsimem/shifted_" +
                        utils::generateRandomString(8) + "_" + std::to_string(i) + ".tif";

                    char **tArgs = nullptr;
                    tArgs = CSLAddString(tArgs, "-srcwin");
                    tArgs = CSLAddString(tArgs, std::to_string(srcX).c_str());
                    tArgs = CSLAddString(tArgs, std::to_string(srcY).c_str());
                    tArgs = CSLAddString(tArgs, std::to_string(outW).c_str());
                    tArgs = CSLAddString(tArgs, std::to_string(outH).c_str());

                    GDALTranslateOptions *srcOpts = GDALTranslateOptionsNew(tArgs, nullptr);
                    CSLDestroy(tArgs);

                    GDALDatasetH shiftedDs = GDALTranslate(shiftedPath.c_str(), srcDs, srcOpts, nullptr);
                    GDALTranslateOptionsFree(srcOpts);
                    GDALClose(srcDs);

                    if (!shiftedDs) {
                        for (auto d : shiftedDatasets) GDALClose(d);
                        for (const auto &p : shiftedPaths) VSIUnlink(p.c_str());
                        throw GDALException("Cannot create shifted dataset for band " + std::to_string(i));
                    }

                    GDALFlushCache(shiftedDs);
                    shiftedDatasets.push_back(shiftedDs);
                    shiftedPaths.push_back(shiftedPath);
                }

                // Build VRT from shifted datasets
                char **vrtArgs2 = nullptr;
                vrtArgs2 = CSLAddString(vrtArgs2, "-separate");
                vrtArgs2 = CSLAddString(vrtArgs2, "-r");
                vrtArgs2 = CSLAddString(vrtArgs2, "average");

                GDALBuildVRTOptions *vrtOpts2 = GDALBuildVRTOptionsNew(vrtArgs2, nullptr);
                CSLDestroy(vrtArgs2);

                vsiVrtPath = "/vsimem/" + utils::generateRandomString(16) + "_aligned.vrt";
                hVrt = GDALBuildVRT(vsiVrtPath.c_str(), static_cast<int>(shiftedDatasets.size()),
                                    shiftedDatasets.data(), nullptr, vrtOpts2, nullptr);
                GDALBuildVRTOptionsFree(vrtOpts2);

                if (!hVrt) {
                    for (auto d : shiftedDatasets) GDALClose(d);
                    for (const auto &p : shiftedPaths) VSIUnlink(p.c_str());
                    throw GDALException("Cannot build VRT for aligned merge");
                }
                GDALFlushCache(hVrt);

                // Translate to output
                char **transArgs = nullptr;
                transArgs = CSLAddString(transArgs, "-of");
                transArgs = CSLAddString(transArgs, "GTiff");
                transArgs = CSLAddString(transArgs, "-co");
                transArgs = CSLAddString(transArgs, "TILED=YES");
                transArgs = CSLAddString(transArgs, "-co");
                transArgs = CSLAddString(transArgs, "NUM_THREADS=ALL_CPUS");
                transArgs = CSLAddString(transArgs, "-co");
                transArgs = CSLAddString(transArgs, "BIGTIFF=IF_SAFER");
                transArgs = CSLAddString(transArgs, "-co");
                transArgs = CSLAddString(transArgs, predictor);

                if (useJpeg) {
                    transArgs = CSLAddString(transArgs, "-co");
                    transArgs = CSLAddString(transArgs, "COMPRESS=JPEG");
                    transArgs = CSLAddString(transArgs, "-co");
                    transArgs = CSLAddString(transArgs, "QUALITY=90");
                } else {
                    transArgs = CSLAddString(transArgs, "-co");
                    transArgs = CSLAddString(transArgs, "COMPRESS=LZW");
                }

                GDALTranslateOptions *transOpts = GDALTranslateOptionsNew(transArgs, nullptr);
                CSLDestroy(transArgs);

                hOut = GDALTranslate(outputPath.c_str(), hVrt, transOpts, nullptr);
                GDALTranslateOptionsFree(transOpts);

                GDALClose(hVrt);
                for (auto d : shiftedDatasets) GDALClose(d);
                for (const auto &p : shiftedPaths) VSIUnlink(p.c_str());
                VSIUnlink(vsiVrtPath.c_str());

                // GPS correction for center shift
                if (hasGps) {
                    double centerShiftXPx = (padLeft - padRight) / 2.0;
                    double centerShiftYPx = (padTop - padBottom) / 2.0;

                    // Try to compute GSD for GPS correction
                    double pixelPitch = 0, focalLength = 0;
                    try {
                        auto exivImg = Exiv2::ImageFactory::open(inputPaths[0]);
                        if (exivImg.get()) {
                            exivImg->readMetadata();
                            ExifParser p(exivImg.get());

                            auto flIt = p.findXmpKey("Xmp.Camera.PerspectiveFocalLength");
                            if (flIt != p.xmpEnd()) {
                                try { focalLength = std::stod(flIt->toString()); } catch (...) {}
                            }
                            if (focalLength <= 0) {
                                Focal f;
                                if (p.computeFocal(f)) focalLength = f.length;
                            }

                            SensorSize ss;
                            if (p.extractSensorSize(ss) && w > 0) {
                                pixelPitch = ss.width / w;
                            }
                        }
                    } catch (...) {}

                    if (srcGps.altitude > 0 && focalLength > 0 && pixelPitch > 0) {
                        double gsd = srcGps.altitude * pixelPitch / focalLength;
                        double dLat = -(centerShiftYPx * gsd) / 111320.0;
                        double dLon = (centerShiftXPx * gsd) /
                            (111320.0 * std::cos(srcGps.latitude * M_PI / 180.0));
                        srcGps.latitude += dLat;
                        srcGps.longitude += dLon;
                        LOGD << "GPS corrected for alignment crop: dLat=" << dLat << " dLon=" << dLon;
                    } else {
                        LOGD << "Cannot compute GSD for GPS correction (alt="
                             << srcGps.altitude << " fl=" << focalLength << " pp=" << pixelPitch << ")";
                    }
                }

                // hOut is set, datasets already cleaned up in shift path
                // Fall through to GPS writing below
            }
        }

        if (!applyShift) {
            // Standard non-shifted merge path
            char **transArgs = nullptr;
            transArgs = CSLAddString(transArgs, "-of");
            transArgs = CSLAddString(transArgs, "GTiff");
            transArgs = CSLAddString(transArgs, "-co");
            transArgs = CSLAddString(transArgs, "TILED=YES");
            transArgs = CSLAddString(transArgs, "-co");
            transArgs = CSLAddString(transArgs, "NUM_THREADS=ALL_CPUS");
            transArgs = CSLAddString(transArgs, "-co");
            transArgs = CSLAddString(transArgs, "BIGTIFF=IF_SAFER");
            transArgs = CSLAddString(transArgs, "-co");
            transArgs = CSLAddString(transArgs, predictor);

            if (useJpeg) {
                transArgs = CSLAddString(transArgs, "-co");
                transArgs = CSLAddString(transArgs, "COMPRESS=JPEG");
                transArgs = CSLAddString(transArgs, "-co");
                transArgs = CSLAddString(transArgs, "QUALITY=90");
            } else {
                transArgs = CSLAddString(transArgs, "-co");
                transArgs = CSLAddString(transArgs, "COMPRESS=LZW");
            }

            GDALTranslateOptions *transOpts = GDALTranslateOptionsNew(transArgs, nullptr);
            CSLDestroy(transArgs);

            hOut = GDALTranslate(outputPath.c_str(), hVrt, transOpts, nullptr);
            GDALTranslateOptionsFree(transOpts);
        }
    }

    if (!hOut) {
        if (!datasets.empty()) {
            if (hVrt) GDALClose(hVrt);
            for (auto d : datasets) GDALClose(d);
        }
        VSIUnlink(vsiVrtPath.c_str());
        throw GDALException("Cannot create merged output: " + outputPath);
    }

    GDALFlushCache(hOut);
    GDALClose(hOut);

    // Clean up only if datasets haven't been cleaned up by the shift path
    if (!datasets.empty()) {
        if (hVrt) GDALClose(hVrt);
        for (auto d : datasets) GDALClose(d);
        VSIUnlink(vsiVrtPath.c_str());
    }

    // Write GPS coordinates from source to merged output
    if (hasGps) {
        try {
            ExifEditor editor(outputPath);
            editor.SetGPS(srcGps.latitude, srcGps.longitude, srcGps.altitude);
            LOGD << "GPS coordinates written to merged output: " << outputPath;
        } catch (const std::exception &e) {
            LOGD << "Could not write GPS to " << outputPath << ": " << e.what();
        }
    }

    LOGD << "Merged " << inputPaths.size() << " bands into " << outputPath;
}

} // namespace ddb
