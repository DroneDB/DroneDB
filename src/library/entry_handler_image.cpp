/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Image, GeoImage, Panorama, GeoPanorama, Video and GeoVideo entries: photos and videos read
// through their EXIF/XMP tags. The Image handler also detects GeoRaster TIFFs.

#include <memory>

#include "entry.h"
#include "entry_handlers_builtin.h"
#include "gdal_raii.h"
#include "mio.h"
#include "thermal.h"

namespace ddb {

namespace {

// The extensions of the family, by what detection does with them
bool isTiff(const io::Path &p)
{
    return p.checkExtension({"tif", "tiff"});
}

bool isOtherImage(const io::Path &p)
{
    return p.checkExtension({"jpg", "jpeg", "dng", "png", "gif", "webp"});
}

bool isVideo(const io::Path &p)
{
    return p.checkExtension({"mp4", "mov", "webm", "m4v", "avi", "mkv"});
}

// Detection reads the metadata of every such file, whatever it holds. Not of every TIFF: GDAL
// looks at a TIFF first, and one with a projection is a GeoRaster, whose metadata is not read.
bool detectionAlwaysReadsMetadata(const io::Path &p)
{
    return isOtherImage(p) || isVideo(p);
}

// The image family is told apart by reading the file: a TIFF with a projection is a
// GeoRaster, an image twice as wide as high a Panorama, EXIF/XMP GPS tags make it Geo*.
EntryType detectImageFamily(const fs::path &path, FingerprintContext &ctx)
{
    EntryType type = EntryType::Generic;
    io::Path p(path);

    bool tif = isTiff(p);
    bool video = isVideo(p);

    bool georaster = false;

    if (tif)
    {
        const auto dataset = adoptDataset(GDALOpen(io::toUtf8(path).c_str(), GA_ReadOnly));
        if (dataset)
        {
            const char *proj = GDALGetProjectionRef(datasetHandle(dataset));
            if (proj != NULL)
            {
                georaster = std::string(proj) != "";
            }
        }
        else
        {
            LOGD << "Cannot open " << io::toUtf8(path) << " for georaster test";
        }
    }

    bool image = (tif || isOtherImage(p)) && !georaster;

    if (image || video)
    {
        // A normal image or video by default (refined later)
        type = image ? EntryType::Image : EntryType::Video;

        // A file the reader has no metadata for (not an image, damaged metadata) stays a plain
        // image or video, without EXIF. The read prefetch() started, when there is one.
        ctx.parser = ctx.pendingRead.valid() ? readExifParser(path, ctx.pendingRead) : readExifParser(path);
        ctx.metadataRead = true;

        if (ctx.parser)
        {
            if (type == EntryType::Image)
            {
                // Panorama? The reader reports 0x0 when the dimensions are unknown
                // (EXIF-less or truncated files): never divide by the height.
                const auto size = ctx.parser->extractImageSize();
                const auto w = static_cast<uint64_t>(size.width);
                const auto h = static_cast<uint64_t>(size.height);
                if (h > 0 && w >= 2 * h)
                    type = EntryType::Panorama;
            }

            if (ctx.parser->hasTags())
            {
                if (ctx.parser->extractGeo(ctx.geo))
                {
                    ctx.hasGeo = true;
                    if (type == EntryType::Image)
                        type = EntryType::GeoImage;
                    else if (type == EntryType::Video)
                        type = EntryType::GeoVideo;
                    else if (type == EntryType::Panorama)
                        type = EntryType::GeoPanorama;
                }
                else
                {
                    // Not a georeferenced image, just a plain image
                }
            }
        }
    }
    else if (georaster)
    {
        type = EntryType::GeoRaster;
    }

    return type;
}

// The camera of an image: orientation, make and model (a thermal sensor is marked), the sensor
// size, the focal length and the orientation of the camera; sensorSize, focal and cameraOri
// also feed the footprint
void readCamera(ExifParser *e, Entry &entry, SensorSize &sensorSize, Focal &focal, CameraOrientation &cameraOri)
{
    entry.properties["orientation"] = e->extractImageOrientation();
    entry.properties["make"] = e->extractMake();
    entry.properties["model"] = e->extractModel();
    entry.properties["sensor"] = e->extractSensor();

    // Detect thermal sensor
    {
        std::string make = entry.properties.value("make", "");
        std::string model = entry.properties.value("model", "");
        if (isThermalImageFromExif(make, model)) {
            entry.properties["sensorCategory"] = "thermal";
            LOGD << "Detected thermal sensor: " << make << " " << model;
        }
    }

    if (e->extractSensorSize(sensorSize))
    {
        entry.properties["sensorWidth"] = sensorSize.width;
        entry.properties["sensorHeight"] = sensorSize.height;
    }

    if (e->computeFocal(focal))
    {
        entry.properties["focalLength"] = focal.length;
        entry.properties["focalLength35"] = focal.length35;
    }

    bool hasCameraOri = e->extractCameraOrientation(cameraOri);
    entry.properties["cameraYaw"] = cameraOri.yaw;
    entry.properties["cameraPitch"] = cameraOri.pitch;
    entry.properties["cameraRoll"] = cameraOri.roll;
    entry.properties["hasCameraOrientation"] = hasCameraOri;
    LOGD << "Camera Orientation: " << cameraOri;
}

// What the drone says about the flight: speed, GPS accuracy, GPS direction and track
void readFlight(ExifParser *e, Entry &entry)
{
    // Extract flight speed
    FlightSpeed flightSpeed;
    if (e->extractFlightSpeed(flightSpeed))
    {
        entry.properties["flightSpeed"] = flightSpeed.horizontal();
        entry.properties["flightSpeed3D"] = flightSpeed.magnitude();
        entry.properties["flightSpeedX"] = flightSpeed.x;
        entry.properties["flightSpeedY"] = flightSpeed.y;
        entry.properties["flightSpeedZ"] = flightSpeed.z;
        LOGD << "Flight Speed: " << flightSpeed;
    }

    // Extract GPS accuracy
    GpsAccuracy gpsAccuracy;
    if (e->extractGpsAccuracy(gpsAccuracy))
    {
        if (gpsAccuracy.xyAccuracy >= 0) entry.properties["gpsXYAccuracy"] = gpsAccuracy.xyAccuracy;
        if (gpsAccuracy.zAccuracy >= 0) entry.properties["gpsZAccuracy"] = gpsAccuracy.zAccuracy;
        if (gpsAccuracy.dop >= 0) entry.properties["gpsDop"] = gpsAccuracy.dop;
        LOGD << "GPS Accuracy: " << gpsAccuracy;
    }

    // Extract GPS direction and track
    GpsDirection gpsDirection;
    if (e->extractGpsDirection(gpsDirection))
    {
        if (gpsDirection.hasImgDirection)
        {
            entry.properties["gpsImgDirection"] = gpsDirection.imgDirection;
            entry.properties["gpsImgDirectionRef"] = gpsDirection.imgDirectionRef;
        }
        if (gpsDirection.hasTrack)
        {
            entry.properties["gpsTrack"] = gpsDirection.track;
            entry.properties["gpsTrackRef"] = gpsDirection.trackRef;
        }
        LOGD << "GPS Direction: " << gpsDirection;
    }
}

void readPanorama(ExifParser *e, Entry &entry)
{
    PanoramaInfo pInfo;
    if (e->extractPanoramaInfo(pInfo))
    {
        entry.properties["projectionType"] = pInfo.projectionType;
        entry.properties["croppedWidth"] = pInfo.croppedWidth;
        entry.properties["croppedHeight"] = pInfo.croppedHeight;
        entry.properties["croppedX"] = pInfo.croppedX;
        entry.properties["croppedY"] = pInfo.croppedY;
        entry.properties["poseHeading"] = pInfo.poseHeading;
        entry.properties["posePitch"] = pInfo.posePitch;
        entry.properties["poseRoll"] = pInfo.poseRoll;
    }
}

// The EXIF/XMP parser of an image, with what keeps it alive
struct ImageParser
{
    std::unique_ptr<ExifParser> owned; // null when the parser belongs to the fingerprint context
    ExifParser *parser = nullptr;      // null when the file has no metadata
};

// The parser of the fingerprint context when the fingerprint read the file (null when the file
// has no metadata: asking again would get the same answer), else one that reads the file
ImageParser openImageParser(const fs::path &path, FingerprintContext &ctx)
{
    ImageParser result;
    if (ctx.metadataRead)
    {
        result.parser = ctx.parser.get();
        return result;
    }

    // Fallback: the entry is parsed without the fingerprint having read the file
    result.owned = readExifParser(path);
    result.parser = result.owned.get();
    return result;
}

// The dimensions, even without EXIF tags, since the reader takes them from the file structure. Video dimensions still require XMP metadata (Xmp.video.Width/Height);
// metadata-independent video probing is not currently supported.
void readDimensions(ExifParser *e, bool image, bool video, Entry &entry)
{
    ImageSize imageSize(0, 0);
    if (image)
        imageSize = e->extractImageSize();
    else if (video)
        imageSize = e->extractVideoSize();

    if (imageSize.width > 0 && imageSize.height > 0)
    {
        entry.properties["width"] = imageSize.width;
        entry.properties["height"] = imageSize.height;
    }
}

// EXIF/XMP tags: capture time, camera, orientation, GPS location and footprint
void parseImageMetadata(const fs::path &path, Entry &entry, FingerprintContext &ctx)
{
    bool pano = entry.type == EntryType::Panorama || entry.type == EntryType::GeoPanorama;
    bool image = entry.type == EntryType::Image || entry.type == EntryType::GeoImage || pano;
    bool video = entry.type == EntryType::Video || entry.type == EntryType::GeoVideo;

    const ImageParser imageParser = openImageParser(path, ctx);
    ExifParser *e = imageParser.parser;

    if (e)
        readDimensions(e, image, video, entry);

    if (e && e->hasTags())
    {
        SensorSize sensorSize;
        Focal focal;
        CameraOrientation cameraOri;

        entry.properties["captureTime"] = e->extractCaptureTime();

        if (image)
        {
            readCamera(e, entry, sensorSize, focal, cameraOri);
            readFlight(e, entry);
        }

        // Reuse geo from context if already extracted by fingerprint
        GeoLocation geo;
        bool hasGeo = false;
        if (ctx.hasGeo)
        {
            geo = ctx.geo;
            hasGeo = true;
        }
        else
        {
            hasGeo = e->extractGeo(geo);
        }

        if (hasGeo)
        {
            entry.point_geom.addPoint(geo.longitude, geo.latitude, geo.altitude);
            LOGD << "POINT GEOM: " << entry.point_geom.toWkt();

            // e->printAllTags();

            // Estimate image footprint
            if (image && !pano)
            {
                double relAltitude = 0.0;

                if (e->extractRelAltitude(relAltitude) && sensorSize.width > 0.0 && focal.length > 0.0)
                {
                    calculateFootprint(sensorSize, geo, focal, cameraOri, relAltitude, entry.polygon_geom);
                }
            }
        }

        if (pano)
            readPanorama(e, entry);
    }
    else if (e)
    {
        LOGD << "No XMP/EXIF data found in " << io::toUtf8(path);
    }
}

class ImageEntryHandler : public EntryHandler {
  public:
    ImageEntryHandler(EntryType type, std::string name) : EntryHandler(type, std::move(name)) {}

    // The Image handler claims every extension of the family (GeoRaster TIFFs included)
    std::vector<std::string> extensions() const override {
        if (type() != EntryType::Image)
            return {};
        return {"jpg", "jpeg", "dng", "tif", "tiff", "png", "gif", "webp",
                "mp4", "mov", "webm", "m4v", "avi", "mkv"};
    }

    EntryType detect(const fs::path& path, FingerprintContext& ctx) const override {
        return detectImageFamily(path, ctx);
    }

    void prefetch(const fs::path& path, FingerprintContext& ctx) const override {
        // A read left from another file goes first: it may hold the last helper
        ctx.pendingRead = exif::PendingRead();
        if (detectionAlwaysReadsMetadata(io::Path(path)))
            ctx.pendingRead = exif::metadataReader().begin(path);
    }

    void parseMetadata(const fs::path& path, Entry& entry, FingerprintContext& ctx) const override {
        parseImageMetadata(path, entry, ctx);
    }

    // Images and panoramas are rendered from the file (a panorama is an image twice as wide as
    // high); videos have no thumbnail of their own
    ThumbnailSource thumbnailSource() const override {
        switch (type()) {
            case EntryType::Image:
            case EntryType::GeoImage:
            case EntryType::Panorama:
            case EntryType::GeoPanorama:
                return ThumbnailSource::File;
            default:
                return ThumbnailSource::None;
        }
    }
};

std::string imageTypeName(EntryType type) {
    switch (type) {
        case EntryType::Image: return "Image";
        case EntryType::GeoImage: return "GeoImage";
        case EntryType::Panorama: return "Panorama";
        case EntryType::GeoPanorama: return "GeoPanorama";
        case EntryType::Video: return "Video";
        case EntryType::GeoVideo: return "GeoVideo";
        default: throw InvalidArgsException("Not an image or video type: " + std::to_string(entryTypeValue(type)));
    }
}

}  // namespace

std::unique_ptr<EntryHandler> makeImageEntryHandler(EntryType type) {
    return std::make_unique<ImageEntryHandler>(type, imageTypeName(type));
}

}  // namespace ddb
