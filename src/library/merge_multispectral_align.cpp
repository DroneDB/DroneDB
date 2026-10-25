/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The alignment half of a multispectral merge: what the metadata of each band says about where
// its camera was (principal point, DJI relative optical center), and the pixel shift of each band
// against the reference band that follows from it.

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>

#include "exceptions.h"
#include "exif.h"
#include "logger.h"
#include "merge_multispectral.h"
#include "mio.h"
#include "sensor_data.h"

namespace ddb {

namespace {

bool parsePrincipalPoint(const std::string &ppStr, double &cx, double &cy) {
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

// What one band's metadata says about its camera
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

// The text of an XMP key, when the image has it
std::optional<std::string> xmpText(ExifParser &parser, const char *key) {
    const auto it = parser.findXmpKey(key);
    if (!it) return std::nullopt;
    return it->toString();
}

// A number out of XMP text; non-numeric or out-of-range text is ignored
std::optional<int> parseInt(const std::string &text) {
    try { return std::stoi(text); }
    catch (const std::invalid_argument &) { /* non-numeric XMP, ignore */ }
    catch (const std::out_of_range &) { /* out of range, ignore */ }
    return std::nullopt;
}

std::optional<double> parseDouble(const std::string &text) {
    try { return std::stod(text); }
    catch (const std::invalid_argument &) { /* non-numeric XMP, ignore */ }
    catch (const std::out_of_range &) { /* out of range, ignore */ }
    return std::nullopt;
}

// The pixel pitch of the band, from the first of four sources that can give it
void derivePixelPitch(ExifParser &parser, BandData &band, size_t index) {
    // Priority 1: FocalPlaneResolution
    auto fUnit = parser.findExifKey("Exif.Photo.FocalPlaneResolutionUnit");
    auto fXRes = parser.findExifKey("Exif.Photo.FocalPlaneXResolution");
    auto fYRes = parser.findExifKey("Exif.Photo.FocalPlaneYResolution");

    if (fUnit && fXRes && fYRes) {
        long unit = fUnit->toInt64();
        double rx = fXRes->toFloat();
        double ry = fYRes->toFloat();
        if (rx > 0 && ry > 0) {
            double mmPerUnit = parser.getMmPerUnit(unit);
            if (mmPerUnit > 0) {
                band.pixelPitchX = mmPerUnit / rx;
                band.pixelPitchY = mmPerUnit / ry;
                band.hasPixelPitch = true;
                LOGD << "Band " << index << " pixel pitch from FocalPlaneRes: "
                     << band.pixelPitchX * 1000.0 << " um x "
                     << band.pixelPitchY * 1000.0 << " um";
            }
        }
    }

    // Priority 2: CalibratedFocalLength (px) + PerspectiveFocalLength (mm)
    if (!band.hasPixelPitch && band.focalLengthMm > 0) {
        if (const auto text = xmpText(parser, "Xmp.drone-dji.CalibratedFocalLength")) {
            if (const auto cfl = parseDouble(*text)) {
                if (*cfl > 0) {
                    double pp = band.focalLengthMm / *cfl;
                    band.pixelPitchX = pp;
                    band.pixelPitchY = pp;
                    band.hasPixelPitch = true;
                    LOGD << "Band " << index << " pixel pitch from CalibratedFocalLength: "
                         << pp * 1000.0 << " um";
                }
            }
        }
    }

    // Priority 3: Sensor database lookup
    if (!band.hasPixelPitch && band.imageWidth > 0 && band.imageHeight > 0) {
        std::string sensor = parser.extractSensor();
        if (SensorData::contains(sensor)) {
            double sensorWidthMm = SensorData::getFocal(sensor);
            if (sensorWidthMm > 0) {
                band.pixelPitchX = sensorWidthMm / band.imageWidth;
                double aspect = static_cast<double>(band.imageWidth) / band.imageHeight;
                double sensorHeightMm = sensorWidthMm / aspect;
                band.pixelPitchY = sensorHeightMm / band.imageHeight;
                band.hasPixelPitch = true;
                LOGD << "Band " << index << " pixel pitch from sensor DB (" << sensor << "): "
                     << band.pixelPitchX * 1000.0 << " um";
            }
        }
    }

    // Priority 4: FocalLength + FocalLength35mmEquiv
    if (!band.hasPixelPitch && band.imageWidth > 0) {
        Focal f;
        if (parser.computeFocal(f) && f.length > 0 && f.length35 > 0) {
            double sensorWidthMm = f.length * 36.0 / f.length35;
            band.pixelPitchX = sensorWidthMm / band.imageWidth;
            band.pixelPitchY = band.pixelPitchX;
            band.hasPixelPitch = true;
            LOGD << "Band " << index << " pixel pitch from FocalLength35mm: "
                 << band.pixelPitchX * 1000.0 << " um (square pixel assumed)";
        }
    }
}

// The tags that are only logged
void logIgnoredTags(ExifParser &parser, const std::string &bandName) {
    // Log lens model
    if (const auto model = xmpText(parser, "Xmp.Camera.ModelType")) {
        LOGD << "Band " << bandName << ": lens model = " << *model;
    }

    // Log Sentera tags (not used)
    if (const auto alignMatrix = xmpText(parser, "Xmp.Sentera.AlignMatrix")) {
        LOGD << "Band " << bandName << ": Sentera AlignMatrix = " << *alignMatrix << " (not used)";
    }

    // Log RigRelatives (not used)
    if (const auto rigRelatives = xmpText(parser, "Xmp.MicaSense.RigRelatives")) {
        LOGD << "Band " << bandName << ": RigRelatives = " << *rigRelatives << " (not used)";
    }
}

// Reads the EXIF/XMP of one band into band and info; a band whose metadata cannot be read keeps
// its defaults
void readBand(const std::string &path, size_t index, BandData &band, BandAlignmentInfo &info) {
    // A file with no metadata the reader can give keeps the defaults
    const auto parserPtr = readExifParser(io::fromUtf8(path));
    if (!parserPtr) return;
    ExifParser &parser = *parserPtr;

    auto imgSize = parser.extractImageSize();
    band.imageWidth = imgSize.width;
    band.imageHeight = imgSize.height;
    info.imageWidth = imgSize.width;
    info.imageHeight = imgSize.height;

    // Read BandName
    if (const auto name = xmpText(parser, "Xmp.Camera.BandName")) {
        band.bandName = *name;
        info.bandName = band.bandName;
    }

    // Read CentralWavelength
    if (const auto text = xmpText(parser, "Xmp.Camera.CentralWavelength")) {
        if (const auto wavelength = parseInt(*text)) band.centralWavelength = *wavelength;
        info.centralWavelength = band.centralWavelength;
    }

    // Detect thermal band
    if (band.bandName == "LWIR" || band.centralWavelength > 7000) {
        info.isThermal = true;
    }

    // Read RigCameraIndex
    if (const auto text = xmpText(parser, "Xmp.Camera.RigCameraIndex")) {
        if (const auto rig = parseInt(*text)) band.rigCameraIndex = *rig;
    }

    // Read PrincipalPoint (mm)
    if (const auto text = xmpText(parser, "Xmp.Camera.PrincipalPoint")) {
        if (parsePrincipalPoint(*text, band.ppMmX, band.ppMmY)) {
            band.hasPP = true;
        }
    }

    // Read PerspectiveFocalLength (mm)
    if (const auto text = xmpText(parser, "Xmp.Camera.PerspectiveFocalLength")) {
        if (const auto focal = parseDouble(*text)) band.focalLengthMm = *focal;
    }

    // Read DJI RelativeOpticalCenter
    const auto rocX = xmpText(parser, "Xmp.drone-dji.RelativeOpticalCenterX");
    const auto rocY = xmpText(parser, "Xmp.drone-dji.RelativeOpticalCenterY");
    if (rocX && rocY) {
        const auto x = parseDouble(*rocX);
        const auto y = x ? parseDouble(*rocY) : std::nullopt;
        if (x && y) {
            band.relOptCenterX = *x;
            band.relOptCenterY = *y;
            band.hasRelOptCenter = true;
        }
    }

    derivePixelPitch(parser, band, index);

    // Do not mark detected here; detection status is set later
    // when a shift source is actually selected and shifts computed.
    // info.detected stays false (default) until then.

    logIgnoredTags(parser, info.bandName);
}

// The reference band the others are aligned to: Green when there is one
size_t selectReferenceBand(const std::vector<BandData> &bands) {
    size_t ref = 0;
    for (size_t i = 0; i < bands.size(); i++) {
        if (bands[i].bandName == "Green" ||
            (bands[i].centralWavelength >= 540 && bands[i].centralWavelength <= 570)) {
            ref = i;
            break;
        }
    }
    LOGD << "Alignment reference band: " << ref << " (" << bands[ref].bandName << ")";
    return ref;
}

// The shift of the principal point of band i against the reference, in pixels
void principalPointShift(const BandData &band, const BandData &ref, double &sx, double &sy) {
    double deltaCxMm = band.ppMmX - ref.ppMmX;
    double deltaCyMm = band.ppMmY - ref.ppMmY;
    sx = deltaCxMm / band.pixelPitchX;
    sy = deltaCyMm / band.pixelPitchY;
}

// The largest principal-point shift in pixels, and whether every band has what it takes to
// compute one (principal point and pixel pitch)
void principalPointExtent(const std::vector<BandData> &bands, size_t ref, double &maxPPShift,
                          bool &allHavePPAndPitch) {
    maxPPShift = 0;
    allHavePPAndPitch = true;
    for (size_t i = 0; i < bands.size(); i++) {
        if (!bands[i].hasPP || !bands[i].hasPixelPitch) {
            allHavePPAndPitch = false;
            continue;
        }
        if (i == ref) continue;
        if (!bands[ref].hasPP || !bands[ref].hasPixelPitch) {
            allHavePPAndPitch = false;
            continue;
        }
        double sx, sy;
        principalPointShift(bands[i], bands[ref], sx, sy);
        maxPPShift = std::max(maxPPShift, std::max(std::abs(sx), std::abs(sy)));
    }
}

void applyPrincipalPointShifts(const std::vector<BandData> &bands, size_t ref,
                               std::vector<BandAlignmentInfo> &alignInfo) {
    for (size_t i = 0; i < bands.size(); i++) {
        if (!bands[i].hasPP || !bands[i].hasPixelPitch) continue;
        if (!bands[ref].hasPP || !bands[ref].hasPixelPitch) continue;
        if (i == ref) {
            alignInfo[i].shiftX = 0;
            alignInfo[i].shiftY = 0;
            alignInfo[i].shiftSource = "PrincipalPoint";
            alignInfo[i].detected = true;
            continue;
        }
        principalPointShift(bands[i], bands[ref], alignInfo[i].shiftX, alignInfo[i].shiftY);
        alignInfo[i].shiftSource = "PrincipalPoint";
        alignInfo[i].detected = true;
    }
}

void applyDjiRelativeOpticalCenterShifts(const std::vector<BandData> &bands, size_t ref,
                                         std::vector<BandAlignmentInfo> &alignInfo) {
    // Find a reference band that actually has RelOC metadata
    size_t djiRef = ref;
    if (!bands[djiRef].hasRelOptCenter) {
        for (size_t i = 0; i < bands.size(); i++) {
            if (bands[i].hasRelOptCenter) { djiRef = i; break; }
        }
    }
    if (!bands[djiRef].hasRelOptCenter) {
        LOGD << "No band has DJI RelativeOpticalCenter metadata, skipping";
        return;
    }
    for (size_t i = 0; i < bands.size(); i++) {
        if (!bands[i].hasRelOptCenter) continue;
        double rocX = bands[i].relOptCenterX - bands[djiRef].relOptCenterX;
        double rocY = bands[i].relOptCenterY - bands[djiRef].relOptCenterY;
        alignInfo[i].shiftX = rocX;
        alignInfo[i].shiftY = rocY;
        alignInfo[i].shiftSource = "DJI_RelativeOpticalCenter";
        alignInfo[i].detected = true;
    }
}

// A shift of more than a tenth of the image is a metadata error, not a misalignment
void dropImplausibleShifts(std::vector<BandAlignmentInfo> &alignInfo) {
    const double MAX_PLAUSIBLE_SHIFT_RATIO = 0.10;
    for (auto &info : alignInfo) {
        if (!info.detected) continue;
        double maxDimPx = static_cast<double>(std::max(info.imageWidth, info.imageHeight));
        double maxShiftPx = MAX_PLAUSIBLE_SHIFT_RATIO * maxDimPx;
        if (std::abs(info.shiftX) > maxShiftPx ||
            std::abs(info.shiftY) > maxShiftPx) {
            LOGD << "Implausible shift for band " << info.bandName
                 << ": (" << info.shiftX << ", " << info.shiftY << ") px - ignoring";
            info.detected = false;
            info.shiftX = 0;
            info.shiftY = 0;
        }
    }
}

}  // namespace

std::vector<BandAlignmentInfo> detectBandAlignment(const std::vector<std::string> &inputPaths) {
    const size_t N = inputPaths.size();
    std::vector<BandAlignmentInfo> alignInfo(N);

    if (N < 2) return alignInfo;

    std::vector<BandData> bands(N);
    for (size_t i = 0; i < N; i++)
        readBand(inputPaths[i], i, bands[i], alignInfo[i]);

    const size_t REF = selectReferenceBand(bands);

    double maxPPShift;
    bool allHavePPAndPitch;
    principalPointExtent(bands, REF, maxPPShift, allHavePPAndPitch);

    // Check DJI RelOC availability
    bool hasDjiRelOC = false;
    for (size_t i = 0; i < N; i++) {
        if (bands[i].hasRelOptCenter) { hasDjiRelOC = true; break; }
    }

    // Source selection
    // Priority: DJI RelativeOpticalCenter → PrincipalPoint → none
    if (hasDjiRelOC) {
        applyDjiRelativeOpticalCenterShifts(bands, REF, alignInfo);
    } else if (maxPPShift > 1.5 && allHavePPAndPitch) {
        applyPrincipalPointShifts(bands, REF, alignInfo);
    }

    dropImplausibleShifts(alignInfo);

    // Log results
    for (size_t i = 0; i < N; i++) {
        LOGD << "Band " << i << " (" << alignInfo[i].bandName << ")"
             << " detected=" << alignInfo[i].detected
             << " shift=(" << alignInfo[i].shiftX << ", " << alignInfo[i].shiftY << ") px"
             << " source=" << alignInfo[i].shiftSource;
    }

    return alignInfo;
}

}  // namespace ddb
