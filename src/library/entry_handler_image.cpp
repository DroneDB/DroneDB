/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Image, GeoImage, Panorama, GeoPanorama, Video and GeoVideo entries: photos and videos read
// through their EXIF/XMP tags. The Image handler also detects GeoRaster TIFFs.

#include <exiv2/exiv2.hpp>
#include <memory>

#include "entry.h"
#include "entry_handlers_builtin.h"
#include "gdal_raii.h"
#include "mio.h"
#include "thermal.h"

namespace ddb {

namespace {

// The image family is told apart by reading the file: a TIFF with a projection is a
// GeoRaster, an image twice as wide as high a Panorama, EXIF/XMP GPS tags make it Geo*.
EntryType detectImageFamily(const fs::path &path, FingerprintContext &ctx)
{
    EntryType type = EntryType::Generic;
    io::Path p(path);

    bool jpg = p.checkExtension({"jpg", "jpeg"});
    bool dng = p.checkExtension({"dng"});
    bool tif = p.checkExtension({"tif", "tiff"});
    bool nongeoImage = p.checkExtension({"png", "gif", "webp"});
    bool video = p.checkExtension({"mp4", "mov", "webm", "m4v", "avi", "mkv"});

    bool georaster = false;

    if (tif)
    {
        const auto dataset = adoptDataset(GDALOpen(p.string().c_str(), GA_ReadOnly));
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
            LOGD << "Cannot open " << p.string().c_str() << " for georaster test";
        }
    }

    bool image = (jpg || tif || dng || nongeoImage) && !georaster;

    if (image || video)
    {
        // A normal image or video by default (refined later)
        type = image ? EntryType::Image : EntryType::Video;

        try
        {
            ctx.exivImage = Exiv2::ImageFactory::open(path.string());
            if (!ctx.exivImage.get())
                throw IndexException("Cannot open " + path.string());

            ctx.exivImage->readMetadata();
            ctx.parser = std::make_unique<ExifParser>(ctx.exivImage.get());
            ctx.populated = true;

            if (type == EntryType::Image)
            {
                // Panorama? Exiv2 reports 0x0 when the dimensions are unknown
                // (EXIF-less or truncated files): never divide by the height.
                const auto w = static_cast<uint64_t>(ctx.exivImage->pixelWidth());
                const auto h = static_cast<uint64_t>(ctx.exivImage->pixelHeight());
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
        catch (Exiv2::Error &)
        {
            LOGD << "Cannot read EXIF data: " << path.string();
            ctx.populated = false;
        }
    }
    else if (georaster)
    {
        type = EntryType::GeoRaster;
    }

    return type;
}

// EXIF/XMP tags: capture time, camera, orientation, GPS location and footprint
void parseImageMetadata(const fs::path &path, Entry &entry, FingerprintContext &ctx)
{
    bool pano = entry.type == EntryType::Panorama || entry.type == EntryType::GeoPanorama;
    bool image = entry.type == EntryType::Image || entry.type == EntryType::GeoImage || pano;
    bool video = entry.type == EntryType::Video || entry.type == EntryType::GeoVideo;

    // Reuse ExifParser from context if available (populated by fingerprint)
    ExifParser *e = nullptr;
    std::unique_ptr<Exiv2::Image> localExivImage;
    std::unique_ptr<ExifParser> localParser;

    if (ctx.populated && ctx.parser)
    {
        e = ctx.parser.get();
    }
    else
    {
        // Fallback: open file if context not populated
        try
        {
            localExivImage = Exiv2::ImageFactory::open(path.string());
            if (!localExivImage.get())
                throw IndexException("Cannot open " + path.string());

            localExivImage->readMetadata();
            localParser = std::make_unique<ExifParser>(localExivImage.get());
            e = localParser.get();
        }
        catch (Exiv2::Error &)
        {
            LOGD << "Cannot read EXIF data: " << path.string();
            e = nullptr;
        }
    }

    // Extract image dimensions even without EXIF tags,
    // since pixelWidth()/pixelHeight() read from the file structure.
    // Note: video dimensions still require XMP metadata (Xmp.video.Width/Height);
    // metadata-independent video probing is not currently supported.
    if (e)
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

    if (e && e->hasTags())
    {
        SensorSize sensorSize;
        Focal focal;
        CameraOrientation cameraOri;

        entry.properties["captureTime"] = e->extractCaptureTime();

        if (image)
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

        // Reuse geo from context if already extracted by fingerprint
        GeoLocation geo;
        bool hasGeo = false;
        if (ctx.populated && ctx.hasGeo)
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
    }
    else if (e)
    {
        LOGD << "No XMP/EXIF data found in " << path.string();
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

    void parseMetadata(const fs::path& path, Entry& entry, FingerprintContext& ctx) const override {
        parseImageMetadata(path, entry, ctx);
    }

    // Panoramas and videos have no thumbnail of their own
    ThumbnailSource thumbnailSource() const override {
        return type() == EntryType::Image || type() == EntryType::GeoImage ? ThumbnailSource::File
                                                                          : ThumbnailSource::None;
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
        default: throw InvalidArgsException("Not an image or video type: " + std::to_string(type));
    }
}

}  // namespace

std::unique_ptr<EntryHandler> makeImageEntryHandler(EntryType type) {
    return std::make_unique<ImageEntryHandler>(type, imageTypeName(type));
}

}  // namespace ddb
