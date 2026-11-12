/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The merge of single-band rasters into one multi-band GeoTIFF. The validation, the alignment
// detection and the preview are in merge_multispectral_validate.cpp, _align.cpp and _preview.cpp.

#include "merge_multispectral.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <map>
#include <sstream>

#include "exceptions.h"
#include "exif.h"
#include "exifeditor.h"
#include "artefact_cache.h"
#include "gdal_raii.h"
#include "gtiff_options.h"
#include "logger.h"
#include "mathutil.h"
#include "merge_crop.h"
#include "merge_vrt.h"
#include "build_layout.h"
#include "mio.h"
#include "parallel.h"
#include "raster_artefacts.h"

namespace ddb {

namespace {

using merge::SeparateVrt;
using merge::buildSeparateVrt;
using merge::openAll;
using merge::buildAlignedVrt;

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

// What a merged raster keeps of the camera capture it was made from, read from the first band
// (all bands come from one capture) before the merge: the EXIF and XMP that the index reads to
// place the file (make, model, time, focal length, sensor, camera orientation, altitude)
struct CameraMetadata {
    // EXIF tags to set, in the text form of the tag
    exif::TagAssignments exifTags;
    // XMP properties that describe the capture, as (prefix, name, value)
    struct XmpProperty {
        std::string prefix, name, value;
    };
    std::vector<XmpProperty> xmp;
    // The namespace of each prefix of xmp, as the first band declares it
    std::map<std::string, std::string> namespaces;
};

// The EXIF tags of the capture that the merge copies: not the ones that describe one band (its
// size, strips, compression, colour space). The GPS position is written by the merge itself
// (it moves with the crop), and so are its own latitude, longitude and altitude tags.
constexpr const char *kCapturedExifTags[] = {
    "Exif.Image.Make", "Exif.Image.Model", "Exif.Image.DateTime", "Exif.Image.Orientation",
    "Exif.Photo.LensMake", "Exif.Photo.LensModel",
    "Exif.Photo.DateTimeOriginal", "Exif.Photo.DateTimeDigitized",
    "Exif.Photo.SubSecTime", "Exif.Photo.SubSecTimeOriginal", "Exif.Photo.SubSecTimeDigitized",
    "Exif.Photo.OffsetTime", "Exif.Photo.OffsetTimeOriginal", "Exif.Photo.OffsetTimeDigitized",
    "Exif.Photo.FocalLength", "Exif.Photo.FocalLengthIn35mmFilm",
    "Exif.Photo.FocalPlaneXResolution", "Exif.Photo.FocalPlaneYResolution",
    "Exif.Photo.FocalPlaneResolutionUnit",
    "Exif.GPSInfo.GPSDateStamp", "Exif.GPSInfo.GPSTimeStamp", "Exif.GPSInfo.GPSDOP",
    "Exif.GPSInfo.GPSMapDatum", "Exif.GPSInfo.GPSImgDirection", "Exif.GPSInfo.GPSImgDirectionRef",
    "Exif.GPSInfo.GPSTrack", "Exif.GPSInfo.GPSTrackRef"};

// The XMP properties of the capture that the merge copies: the camera orientation, the accuracy
// of the position and the altitudes. Not the ones of one band (its name, wavelength, principal
// point, vignetting), which differ between the bands.
struct CapturedXmpProperty {
    const char *prefix;
    const char *name;
    const char *fallbackNamespace;
};
constexpr CapturedXmpProperty kCapturedXmpProperties[] = {
    {"Camera", "Yaw", "http://pix4d.com/camera/1.0/"},
    {"Camera", "Pitch", "http://pix4d.com/camera/1.0/"},
    {"Camera", "Roll", "http://pix4d.com/camera/1.0/"},
    {"Camera", "GPSXYAccuracy", "http://pix4d.com/camera/1.0/"},
    {"Camera", "GPSZAccuracy", "http://pix4d.com/camera/1.0/"},
    {"drone-dji", "AbsoluteAltitude", "http://www.dji.com/drone-dji/1.0/"},
    {"drone-dji", "RelativeAltitude", "http://www.dji.com/drone-dji/1.0/"},
    {"drone-dji", "GimbalPitchDegree", "http://www.dji.com/drone-dji/1.0/"},
    {"drone-dji", "GimbalYawDegree", "http://www.dji.com/drone-dji/1.0/"},
    {"drone-dji", "GimbalRollDegree", "http://www.dji.com/drone-dji/1.0/"},
    {"drone-dji", "FlightYawDegree", "http://www.dji.com/drone-dji/1.0/"}};

// The namespace that the XMP packet declares for prefix (xmlns:prefix="..."), empty when it
// declares none
std::string declaredNamespace(const std::string &packet, const std::string &prefix) {
    const std::string marker = "xmlns:" + prefix + "=\"";
    const auto at = packet.find(marker);
    if (at == std::string::npos) return {};
    const auto from = at + marker.size();
    const auto to = packet.find('"', from);
    return to == std::string::npos ? std::string() : packet.substr(from, to - from);
}

// The XMP packet of the raster at path, as GDAL reads it (empty when it has none)
std::string xmpPacketOf(const std::string &path) {
    const auto dataset = raster::openInputRaster(path);
    if (!dataset) return {};
    const CPLStringList xmp(CSLDuplicate(GDALGetMetadata(datasetHandle(dataset), "xml:XMP")), TRUE);
    return xmp.Count() > 0 ? std::string(xmp[0]) : std::string();
}

CameraMetadata readCameraMetadata(const std::string &path) {
    CameraMetadata camera;
    // A file with no metadata the reader can give has none to keep
    const auto parser = readExifParser(io::fromUtf8(path));
    if (!parser) return camera;

    for (const char *key : kCapturedExifTags) {
        const auto tag = parser->findExifKey(key);
        if (tag && !tag->truncated() && !tag->toString().empty())
            camera.exifTags.emplace_back(key, tag->toString());
    }

    const std::string packet = xmpPacketOf(path);
    // The band sequences of the merged raster are in the namespace that the bands declare
    const std::string cameraNamespace = declaredNamespace(packet, "Camera");
    if (!cameraNamespace.empty()) camera.namespaces["Camera"] = cameraNamespace;
    for (const auto &property : kCapturedXmpProperties) {
        const auto tag = parser->findXmpKey(std::string("Xmp.") + property.prefix + "." + property.name);
        if (!tag || tag->truncated() || tag->toString().empty()) continue;
        camera.xmp.push_back({property.prefix, property.name, tag->toString()});
        if (!camera.namespaces.count(property.prefix)) {
            const std::string declared = declaredNamespace(packet, property.prefix);
            camera.namespaces[property.prefix] = declared.empty() ? property.fallbackNamespace : declared;
        }
    }
    return camera;
}

// The name of each band of the merged raster, in order: what the camera calls the band (the
// BandName of its XMP, which the alignment detection read), else the description of the band in
// its file; empty for a band that has neither
struct MergedBand {
    std::string name;
    int wavelength = 0;
};

std::vector<MergedBand> mergedBands(const std::vector<std::string> &inputPaths,
                                    const std::vector<BandAlignmentInfo> &alignBands) {
    std::vector<MergedBand> bands;
    for (size_t i = 0; i < inputPaths.size(); i++) {
        const auto dataset = raster::openInputRaster(inputPaths[i]);
        if (!dataset) throw GDALException("Cannot open " + inputPaths[i]);
        GDALDatasetH hDs = datasetHandle(dataset);
        const int count = GDALGetRasterCount(hDs);
        const BandAlignmentInfo *info = i < alignBands.size() ? &alignBands[i] : nullptr;

        for (int b = 1; b <= count; b++) {
            MergedBand band;
            // The camera names a band of a file that has only that band
            if (count == 1 && info) {
                band.name = info->bandName;
                band.wavelength = info->centralWavelength;
            }
            if (band.name.empty()) {
                const char *description = GDALGetDescription(GDALGetRasterBand(hDs, b));
                if (description) band.name = description;
            }
            bands.push_back(band);
        }
    }
    return bands;
}

std::string xmlEscape(const std::string &text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

// The XMP packet of the merged raster: the properties of the capture of the first band, and
// the name and wavelength of every band of the merged raster in order (as the cameras that
// write one file per band write their own, as a sequence per property). Empty when there is
// nothing to say.
std::string mergedXmpPacket(const CameraMetadata &camera, const std::vector<MergedBand> &bands) {
    bool anyName = false, anyWavelength = false;
    for (const auto &band : bands) {
        anyName = anyName || !band.name.empty();
        anyWavelength = anyWavelength || band.wavelength > 0;
    }
    if (camera.xmp.empty() && !anyName && !anyWavelength) return {};

    // The prefix of the band sequences is Camera, as the cameras write them
    std::map<std::string, std::string> namespaces = camera.namespaces;
    if ((anyName || anyWavelength) && !namespaces.count("Camera"))
        namespaces["Camera"] = "http://pix4d.com/camera/1.0/";

    std::ostringstream xml;
    xml << "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
        << "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
        << " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
        << "  <rdf:Description rdf:about=\"\"";
    for (const auto &[prefix, uri] : namespaces)
        xml << " xmlns:" << prefix << "=\"" << xmlEscape(uri) << "\"";
    xml << ">\n";
    for (const auto &property : camera.xmp)
        xml << "   <" << property.prefix << ":" << property.name << ">" << xmlEscape(property.value) << "</"
            << property.prefix << ":" << property.name << ">\n";
    if (anyName) {
        xml << "   <Camera:BandName><rdf:Seq>";
        for (const auto &band : bands) xml << "<rdf:li>" << xmlEscape(band.name) << "</rdf:li>";
        xml << "</rdf:Seq></Camera:BandName>\n";
    }
    if (anyWavelength) {
        xml << "   <Camera:CentralWavelength><rdf:Seq>";
        for (const auto &band : bands) xml << "<rdf:li>" << band.wavelength << "</rdf:li>";
        xml << "</rdf:Seq></Camera:CentralWavelength>\n";
    }
    xml << "  </rdf:Description>\n </rdf:RDF>\n</x:xmpmeta>\n<?xpacket end=\"w\"?>";
    return xml.str();
}

// Names the bands of the output and gives it the XMP of the capture: what the output has to say
// before it is closed (GTiff writes both in its own tags)
void describeMergedOutput(GDALDatasetH out, const std::vector<MergedBand> &bands, const std::string &xmpPacket) {
    const int count = GDALGetRasterCount(out);
    for (int i = 0; i < count && i < static_cast<int>(bands.size()); i++) {
        if (!bands[i].name.empty()) GDALSetDescription(GDALGetRasterBand(out, i + 1), bands[i].name.c_str());
    }
    if (!xmpPacket.empty()) {
        char *list[2] = {const_cast<char *>(xmpPacket.c_str()), nullptr};
        if (GDALSetMetadata(out, list, "xml:XMP") != CE_None) {
            LOGD << "Could not write the XMP of the capture to the merged output";
        }
    }
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
                         const std::string &outputPath,
                         bool overwrite) {
    if (inputPaths.size() < 2) throw InvalidArgsException("At least 2 files required");

    const fs::path target = fs::absolute(io::fromUtf8(outputPath));
    if (!target.has_filename())
        throw InvalidArgsException("Not the path of a file for a merged raster: " + outputPath);
    io::assureReplaceable(io::fromUtf8(outputPath), overwrite, io::fromUtf8(inputPaths));

    auto validation = validateMergeMultispectral(inputPaths);
    if (!validation.ok) {
        std::string errMsg = "Validation failed:";
        for (const auto &e : validation.errors) errMsg += "\n  - " + e;
        throw AppException(errMsg);
    }

    // Read GPS coordinates from the first input file before merge
    // All bands come from the same capture position, so we use the first file
    SourceGps srcGps = readSourceGps(inputPaths[0]);

    // The camera metadata of that capture, and how each band is called: the merged raster keeps
    // them, so that it is placed on the map and opened by the tools as a band of the capture is
    const CameraMetadata camera = readCameraMetadata(inputPaths[0]);
    const std::vector<MergedBand> bands = mergedBands(inputPaths, validation.alignment.bands);

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

    // The merge and its GPS go to a file of a private folder beside the output, which is removed
    // with whatever is left in it: an existing output is replaced only by a complete merge
    const io::ScopedTempDir staging = raster::makePrivateFolder(target.parent_path(), MergeStagingPrefix,
                                                                raster::PrivateFolderWriter::Host);
    const fs::path producedPath = staging.path() / target.filename();
    const std::string produced = io::toUtf8(producedPath);

    // Warp and compression threads: this one and those the CPU pool allows, held
    // until the output is closed
    const auto cpu = parallel::tryAcquireThreads(parallel::hardwareThreads());
    const MergeEncoding encoding{dt, useJpeg, parallel::gdalNumThreads(cpu)};
    GDALDatasetUniquePtr out;

    if (hasGeoTransform) {
        out = warpMerge(hVrt, produced, encoding);
    } else {
        LOGD << "No geotransform found, using GDALTranslate for merge";

        // Check if alignment shift should be applied
        std::optional<AlignmentCrop> crop;
        if (validation.alignment.correctionApplied)
            crop = planAlignmentCrop(validation.alignment.bands, validation.summary.sourceWidth,
                                     validation.summary.sourceHeight);

        if (crop) {
            // Close the original VRT and datasets; the shifted ones replace them
            merged.close();

            SeparateVrt aligned = buildAlignedVrt(inputPaths, *crop);
            out = translateMerge(aligned.handle(), produced, encoding);
            aligned.close();

            // GPS correction for center shift
            if (srcGps.present) {
                const auto geometry = readCameraGeometry(inputPaths[0], validation.summary.sourceWidth);
                correctGpsForCrop(srcGps.location, *crop, geometry);
            }
        } else {
            // Standard non-shifted merge path
            out = translateMerge(merged.handle(), produced, encoding);
        }
    }

    if (!out) {
        throw GDALException("Cannot create merged output: " + outputPath);
    }

    describeMergedOutput(datasetHandle(out), bands, mergedXmpPacket(camera, bands));
    closeOutputOrThrow(std::move(out), outputPath);

    // A no-op when the shift path already closed them
    merged.close();

    // Write the EXIF of the capture and the GPS coordinates from the source to the merged output. A
    // reader that cannot work (no metadata-util) propagates; a file that cannot be written does not
    // fail the merge.
    if (!camera.exifTags.empty()) {
        try {
            ExifEditor editor(produced);
            // One request writes all the tags and fails as a whole when a value is not valid: the
            // tags are then written one by one, so that one bad value costs only that tag
            if (!editor.SetTags(camera.exifTags).empty()) {
                for (const auto &tag : camera.exifTags) {
                    const auto failures = editor.SetTags({tag});
                    if (!failures.empty()) {
                        LOGD << "Could not write " << tag.first << " to " << outputPath << ": "
                             << failures.front().message;
                    }
                }
            }
        } catch (const HelperUnavailableException &) {
            throw;
        } catch (const std::exception &e) {
            LOGD << "Could not write the camera EXIF to " << outputPath << ": " << e.what();
        }
    }
    if (srcGps.present) {
        try {
            ExifEditor editor(produced);
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

    // The output may have appeared while the bands were merged: it is replaced only when asked
    io::assureReplaceable(io::fromUtf8(outputPath), overwrite);
    raster::publishArtefact(producedPath, target);

    LOGD << "Merged " << inputPaths.size() << " bands into " << outputPath;
}

} // namespace ddb
