/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "exif.h"
#include "logger.h"
#include "timezone.h"
#include "dsmservice.h"
#include "sensor_data.h"

namespace ddb
{

    Exiv2::ExifData::const_iterator ExifParser::findExifKey(const std::string &key)
    {
        return findExifKey({key});
    }

    // Find the first available key, or exifData::end() if none exist
    Exiv2::ExifData::const_iterator ExifParser::findExifKey(const std::initializer_list<std::string> &keys)
    {
        for (auto &k : keys)
        {
            auto it = exifData.findKey(Exiv2::ExifKey(k));
            if (it != exifData.end())
                return it;
        }
        return exifData.end();
    }

    Exiv2::XmpData::const_iterator ExifParser::findXmpKey(const std::string &key)
    {
        return findXmpKey({key});
    }

    // Find the first available key, or xmpData::end() if none exist
    Exiv2::XmpData::const_iterator ExifParser::findXmpKey(const std::initializer_list<std::string> &keys)
    {
        for (auto &k : keys)
        {
            try
            {
                auto it = xmpData.findKey(Exiv2::XmpKey(k));
                if (it != xmpData.end())
                    return it;
            }
            catch (Exiv2::Error &)
            {
                // Do nothing
            }
        }
        return xmpData.end();
    }

    ImageSize ExifParser::extractImageSize()
    {
        //    auto imgWidth = findExifKey({"Exif.Photo.PixelXDimension", "Exif.Image.ImageWidth"});
        //    auto imgHeight = findExifKey({"Exif.Photo.PixelYDimension", "Exif.Image.ImageLength"});

        //    if (imgWidth != exifData.end() && imgHeight != exifData.end()) {
        //        return ImageSize(static_cast<int>(imgWidth->toInt64()), static_cast<int>(imgHeight->toInt64()));
        //    }

        return ImageSize(image->pixelWidth(), image->pixelHeight());
    }

    ImageSize ExifParser::extractVideoSize()
    {
        auto xmpWidth = findXmpKey({"Xmp.video.Width"});
        auto xmpHeight = findXmpKey({"Xmp.video.Height"});
        if (xmpWidth != xmpData.end() && xmpHeight != xmpData.end())
        {
            try
            {
                // Convert XMP width and height to integers
                std::string widthStr = xmpWidth->toString();
                std::string heightStr = xmpHeight->toString();

                // Validate that strings contain valid integer format
                for (char c : widthStr)
                    if (!std::isdigit(c) && c != '-' && c != '+')
                        throw std::invalid_argument("Width is not a valid integer: " + widthStr);

                for (char c : heightStr)
                    if (!std::isdigit(c) && c != '-' && c != '+')
                        throw std::invalid_argument("Height is not a valid integer: " + heightStr);

                int width = std::stoi(widthStr);
                int height = std::stoi(heightStr);

                // Additional validation of reasonable values
                if (width <= 0 || height <= 0)
                    throw std::invalid_argument("Width or height is not positive");

                if (width > 100000 || height > 100000) // Arbitrary large limit
                    throw std::invalid_argument("Width or height exceeds reasonable limits");

                return ImageSize(width, height);
            }
            catch (const std::invalid_argument &ia)
            {
                LOGD << "Cannot parse XMP video width/height: " << ia.what();
                return ImageSize(0, 0);
            }
            catch (const std::out_of_range &oor)
            {
                LOGD << "XMP video width/height out of range";
                return ImageSize(0, 0);
            }
        }

        return ImageSize(0, 0);
    }

    std::string ExifParser::extractMake()
    {
        auto k = findExifKey({"Exif.Photo.LensMake", "Exif.Image.Make"});

        if (k != exifData.end())
        {
            return k->toString();
        }
        else
        {
            return "unknown";
        }
    }

    std::string ExifParser::extractModel()
    {
        auto k = findExifKey({"Exif.Image.Model", "Exif.Photo.LensModel"});

        if (k != exifData.end())
        {
            return k->toString();
        }
        else
        {
            return "unknown";
        }
    }

    // Extract "${make} ${model}" lowercase
    std::string ExifParser::extractSensor()
    {
        std::string make = extractMake();
        std::string model = extractModel();
        utils::toLower(make);
        utils::toLower(model);

        if (make != "unknown")
        {
            size_t pos = std::string::npos;

            // Remove duplicate make string from model (if any)
            while ((pos = model.find(make)) != std::string::npos)
            {
                model.erase(pos, make.length());
            }
        }

        utils::trim(make);
        utils::trim(model);

        return make + " " + model;
    }

    bool ExifParser::computeFocal(Focal &f)
    {
        SensorSize r;

        if (extractSensorSize(r))
        {
            double sensorWidth = r.width;
            auto focal35 = findExifKey("Exif.Photo.FocalLengthIn35mmFilm");
            auto focal = findExifKey("Exif.Photo.FocalLength");

            if (focal35 != exifData.end() && focal35->toFloat() > 0)
            {
                f.length35 = static_cast<double>(focal35->toFloat());
                f.length = (f.length35 / 36.0) * sensorWidth;
            }
            else if (focal != exifData.end() && focal->toFloat() > 0)
            {
                f.length = static_cast<double>(focal->toFloat());
                f.length35 = (36.0 * f.length) / sensorWidth;
            }

            return true;
        }

        return false;
    }

    // Extracts sensor sizes (in mm). Returns 0 on failure
    bool ExifParser::extractSensorSize(SensorSize &r)
    {
        auto fUnit = findExifKey("Exif.Photo.FocalPlaneResolutionUnit");
        auto fXRes = findExifKey("Exif.Photo.FocalPlaneXResolution");
        auto fYRes = findExifKey("Exif.Photo.FocalPlaneYResolution");

        if (fUnit != exifData.end() && fXRes != exifData.end() && fYRes != exifData.end())
        {
            long resolutionUnit = fUnit->toInt64();
            double mmPerUnit = getMmPerUnit(resolutionUnit);
            if (mmPerUnit != 0.0)
            {
                auto imsize = extractImageSize();

                double xUnitsPerPixel = 1.0 / static_cast<double>(fXRes->toFloat());
                r.width = imsize.width * xUnitsPerPixel * mmPerUnit;

                double yUnitsPerPixel = 1.0 / static_cast<double>(fYRes->toFloat());
                r.height = imsize.height * yUnitsPerPixel * mmPerUnit;

                return true; // Good, exit here
            }
        }

        // If we reached this point, we fallback to database lookup
        std::string sensor = extractSensor();
        if (SensorData::contains(sensor))
        {
            r.width = SensorData::getFocal(sensor);

            // TODO: is this the best way?
            auto imsize = extractImageSize();
            r.height = (r.width / imsize.width) * imsize.height;
            return true;
        }

        return false;
    }

    // Length of resolution unit in millimiters
    // https://www.sno.phy.queensu.ca/~phil/exiftool/TagNames/EXIF.html
    inline double ExifParser::getMmPerUnit(long resolutionUnit)
    {
        if (resolutionUnit == 2)
        {
            return 25.4; // mm in 1 inch
        }
        else if (resolutionUnit == 3)
        {
            return 10.0; //  mm in 1 cm
        }
        else if (resolutionUnit == 4)
        {
            return 1.0; // mm in 1 mm
        }
        else if (resolutionUnit == 5)
        {
            return 0.001; // mm in 1 um
        }
        else
        {
            LOGE << "Unknown EXIF resolution unit: " << resolutionUnit;
            return 0.0;
        }
    }

    // Extract geolocation information
    bool ExifParser::extractGeo(GeoLocation &geo)
    {
        auto latitude = findExifKey({"Exif.GPSInfo.GPSLatitude"});
        auto latitudeRef = findExifKey({"Exif.GPSInfo.GPSLatitudeRef"});
        auto longitude = findExifKey({"Exif.GPSInfo.GPSLongitude"});
        auto longitudeRef = findExifKey({"Exif.GPSInfo.GPSLongitudeRef"});

        if (latitude != exifData.end() && longitude != exifData.end())
        {
            geo.latitude = geoToDecimal(latitude, latitudeRef);
            geo.longitude = geoToDecimal(longitude, longitudeRef);

            auto altitude = findExifKey({"Exif.GPSInfo.GPSAltitude"});
            if (altitude != exifData.end())
            {
                geo.altitude = evalFrac(altitude->toRational());

                auto altitudeRef = findExifKey({"Exif.GPSInfo.GPSAltitudeRef"});
                if (altitudeRef != exifData.end())
                {
                    geo.altitude *= altitudeRef->toInt64() == 1 ? -1 : 1;
                }
            }

            auto xmpAltitude = findXmpKey({"Xmp.drone-dji.AbsoluteAltitude"});
            if (xmpAltitude != xmpData.end())
            {
                geo.altitude = static_cast<double>(xmpAltitude->toFloat());
            }

            // Use DJI's XMP tags for lat/lon, if available
            // certain models (e.g. Mavic Air) do not have sufficient
            // precision in the EXIF coordinates
            auto xmpLatitude = findXmpKey({"Xmp.drone-dji.Latitude"});
            if (xmpLatitude != xmpData.end())
            {
                geo.latitude = xmpLatitude->toFloat();
            }
            auto xmpLongitude = findXmpKey({"Xmp.drone-dji.Longitude"});
            if (xmpLongitude != xmpData.end())
            {
                geo.longitude = xmpLongitude->toFloat();
            }

            return true;
        }

        // Fallback: DJI XMP tags without EXIF GPS (some models only write XMP)
        {
            auto xmpLat = findXmpKey({"Xmp.drone-dji.Latitude"});
            auto xmpLon = findXmpKey({"Xmp.drone-dji.Longitude"});
            if (xmpLat != xmpData.end() && xmpLon != xmpData.end())
            {
                geo.latitude = static_cast<double>(xmpLat->toFloat());
                geo.longitude = static_cast<double>(xmpLon->toFloat());

                auto xmpAlt = findXmpKey({"Xmp.drone-dji.AbsoluteAltitude"});
                if (xmpAlt != xmpData.end())
                    geo.altitude = static_cast<double>(xmpAlt->toFloat());

                return true;
            }
        }

        auto gpsCoordinates = findXmpKey({"Xmp.video.GPSCoordinates"});
        if (gpsCoordinates != xmpData.end())
        {
            // Xmp.video.GPSCoordinates +46.839139-91.999828+25.700
            // [+-]lat[+-]lon[+-]alt
            std::string gps = gpsCoordinates->toString();
            if (gps.length() < 1)
            {
                LOGD << "Invalid GPS coordinates (empty)";
                return false;
            }
            if (gps[0] != '+' && gps[0] != '-')
            {
                LOGD << "Invalid GPS coordinates (" << gps << " doesn't start with +-)";
                return false;
            }

            int component = 0;
            std::string buf = "";
            buf += gps[0];
            gps = gps + "$";

            for (unsigned long i = 1; i < gps.length(); i++)
            {
                if (std::isdigit(gps[i]) || gps[i] == ',' || gps[i] == '.')
                {
                    buf += gps[i];
                }
                else if (gps[i] == '+' || gps[i] == '-' || gps[i] == '$')
                {
                    try
                    {
                        double val = std::stod(buf);
                        if (component == 0)
                        {
                            geo.latitude = val;
                        }
                        else if (component == 1)
                        {
                            geo.longitude = val;
                        }
                        else if (component == 2)
                        {
                            geo.altitude = val;
                        }
                        else
                        {
                            LOGD << "Ignoring additional GPS coordinates " << gps;
                        }
                    }
                    catch (const std::invalid_argument &ia)
                    {
                        LOGD << "Cannot parse GPS coordinates " << gps;
                        return false;
                    }

                    component++;
                    buf = gps[i];
                }
            }

            LOGD << "Parsed " << component << " GPS components";

            return component >= 2;
        }

        return false;
    }

    bool ExifParser::extractRelAltitude(double &relAltitude)
    {
        // Some drones have a value for relative altitude
        auto k = findXmpKey("Xmp.drone-dji.RelativeAltitude");
        if (k != xmpData.end())
        {
            relAltitude = static_cast<double>(k->toFloat());
            return true;
        }

        // For others, we lookup an estimate from a world DSM source
        GeoLocation geo;
        if (extractGeo(geo) && geo.altitude > 0)
        {
            relAltitude = geo.altitude - static_cast<double>(DSMService::get()->getAltitude(geo.latitude, geo.longitude));
            return true;
        }

        relAltitude = 0.0;
        return false; // Not available
    }

    // Converts a geotag location to decimal degrees
    inline double ExifParser::geoToDecimal(const Exiv2::ExifData::const_iterator &geoTag, const Exiv2::ExifData::const_iterator &geoRefTag)
    {
        if (geoTag == exifData.end())
            return 0.0;

        // N/S, W/E
        double sign = 1.0;
        if (geoRefTag != exifData.end())
        {
            std::string ref = geoRefTag->toString();
            utils::toUpper(ref);
            if (ref == "S" || ref == "W")
                sign = -1.0;
        }

        //    LOGD << geoTag->toRational(0).first << "/" << geoTag->toRational(0).second << " "
        //         << geoTag->toRational(1).first << "/" << geoTag->toRational(1).second << " "
        //         << geoTag->toRational(2).first << "/" << geoTag->toRational(2).second;

        double degrees = evalFrac(geoTag->toRational(0));
        double minutes = evalFrac(geoTag->toRational(1));
        double seconds = evalFrac(geoTag->toRational(2));

        return sign * (degrees + minutes / 60.0 + seconds / 3600.0);
    }

    // Evaluates a rational
    double ExifParser::evalFrac(const Exiv2::Rational &rational)
    {
        if (rational.second == 0)
            return 0.0;
        return static_cast<double>(rational.first) / static_cast<double>(rational.second);
    }

    // Helper: parse SubSecTime string into milliseconds
    double ExifParser::parseSubSec(const Exiv2::ExifData::const_iterator &subsec)
    {
        if (subsec == exifData.end() || subsec->count() == 0)
            return 0.0;

        double ss = static_cast<double>(subsec->toInt64());
        size_t numDigits = subsec->toString().length();

        // ."1" --> "100"
        // ."12" --> "120"
        // ."12345" --> "123.45"
        if (numDigits == 0)
            return 0.0;
        else if (numDigits == 1)
            return ss * 100.0;
        else if (numDigits == 2)
            return ss * 10.0;
        else if (numDigits == 3)
            return ss;
        else
            return ss / static_cast<double>(pow(10, numDigits - 3));
    }

    // Helper: parse OffsetTime string (e.g. "+02:00", "-05:30") into a UTC offset in seconds
    bool ExifParser::parseOffsetTime(const Exiv2::ExifData::const_iterator &offset, int &offsetSeconds)
    {
        if (offset == exifData.end())
            return false;

        std::string s = offset->toString();
        if (s.length() < 5) // Minimum: "+HH:MM" is 6 chars, but some write "+HHMM"
            return false;

        int sign = 1;
        if (s[0] == '-')
            sign = -1;
        else if (s[0] != '+')
            return false;

        int hours = 0, minutes = 0;
        // Try "+HH:MM" format first, then "+HHMM"
        if (sscanf(s.c_str(), "%*c%d:%d", &hours, &minutes) == 2 ||
            sscanf(s.c_str(), "%*c%2d%2d", &hours, &minutes) == 2)
        {
            offsetSeconds = sign * (hours * 3600 + minutes * 60);
            return true;
        }

        return false;
    }

    // Extracts the best available capture timestamp (milliseconds from Jan 1st 1970 UTC).
    //
    // Priority cascade (inspired by OpenSfM):
    //   0. XMP video epoch (DateUTC / MediaCreateDate) — for video files
    //   1. GPS DateStamp + TimeStamp — always UTC, highest accuracy
    //   2. DateTime EXIF + OffsetTime — explicit timezone offset, accurate UTC conversion
    //   3. DateTime EXIF + geo-timezone lookup — fallback using geolocation
    //   4. DateTime EXIF naive (assume UTC) — last resort
    double ExifParser::extractCaptureTime()
    {
        // Priority 0: XMP video timestamps (Mac epoch)
        auto xmpDate = findXmpKey({"Xmp.video.DateUTC", "Xmp.video.MediaCreateDate"});
        if (xmpDate != xmpData.end())
        {
            try
            {
                // Number of seconds between Jan 1st 1904 and Jan 1st 1970
                const long TO_UNIX_EPOCH = 2082844800;
                const long d = xmpDate->toInt64();
                double captureTime = (d - TO_UNIX_EPOCH) * 1000.0;
                if (captureTime > 0)
                {
                    return captureTime;
                }
                else
                {
                    LOGD << "Cannot use XMP capture time (negative?)";
                }
            }
            catch (const std::invalid_argument &ia)
            {
                LOGD << "Cannot parse XMP capture time " << xmpDate->toString();
            }
        }

        // Priority 1: GPS DateStamp + TimeStamp (always UTC, most accurate)
        {
            auto datestamp = findExifKey("Exif.GPSInfo.GPSDateStamp");
            auto timestamp = findExifKey("Exif.GPSInfo.GPSTimeStamp");

            if (datestamp != exifData.end() && timestamp != exifData.end())
            {
                int year, month, day;
                if (sscanf(datestamp->toString().c_str(), "%d:%d:%d", &year, &month, &day) == 3)
                {
                    double hours = evalFrac(timestamp->toRational(0));
                    double minutes = evalFrac(timestamp->toRational(1));
                    double seconds = evalFrac(timestamp->toRational(2));

                    int h = static_cast<int>(hours);
                    int m = static_cast<int>(minutes);
                    int s = static_cast<int>(seconds);
                    double msecs = (seconds - s) * 1000.0;

                    cctz::time_zone utc = cctz::utc_time_zone();
                    double result = Timezone::getUTCEpoch(year, month, day, h, m, s, msecs, utc);
                    if (result > 0)
                    {
                        LOGD << "Using GPS timestamp as capture time (UTC)";
                        return result;
                    }
                }
                else
                {
                    LOGD << "Invalid GPS date stamp: " << datestamp->toString();
                }
            }
        }

        // Priority 2 & 3: DateTime EXIF with aligned SubSec and OffsetTime triples
        // Try each triple in order; within each, prefer OffsetTime (priority 2)
        // over geo-timezone lookup (priority 3)
        struct DateTimeTriple {
            const char *dateTimeKey;
            const char *subSecKey;
            const char *offsetKey;
        };

        const DateTimeTriple triples[] = {
            {"Exif.Photo.DateTimeOriginal",  "Exif.Photo.SubSecTimeOriginal",  "Exif.Photo.OffsetTimeOriginal"},
            {"Exif.Photo.DateTimeDigitized", "Exif.Photo.SubSecTimeDigitized", "Exif.Photo.OffsetTimeDigitized"},
            {"Exif.Image.DateTime",          "Exif.Photo.SubSecTime",          "Exif.Photo.OffsetTime"}
        };

        // First pass: try triples that have OffsetTime (most accurate after GPS)
        for (const auto &t : triples)
        {
            auto time = findExifKey(t.dateTimeKey);
            if (time == exifData.end())
                continue;

            auto offset = findExifKey(t.offsetKey);
            int offsetSecs = 0;
            if (!parseOffsetTime(offset, offsetSecs))
                continue; // No valid offset — skip in this pass

            int year, month, day, hour, minute, second;
            if (sscanf(time->toString().c_str(), "%d:%d:%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6)
                continue;

            double msecs = parseSubSec(findExifKey(t.subSecKey));

            // DateTime is in local time; apply offset to convert to UTC
            // OffsetTime "+02:00" means local = UTC + 2h, so UTC = local - offset
            cctz::time_zone utc = cctz::utc_time_zone();
            double result = Timezone::getUTCEpoch(year, month, day, hour, minute, second, msecs, utc);
            result -= offsetSecs * 1000.0;

            if (result > 0)
            {
                LOGD << "Using DateTime+OffsetTime as capture time";
                return result;
            }
        }

        // Second pass: try triples without OffsetTime, using geo-timezone lookup
        for (const auto &t : triples)
        {
            auto time = findExifKey(t.dateTimeKey);
            if (time == exifData.end())
                continue;

            int year, month, day, hour, minute, second;
            if (sscanf(time->toString().c_str(), "%d:%d:%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6)
            {
                LOGD << "Invalid date/time format: " << time->toString();
                continue;
            }

            double msecs = parseSubSec(findExifKey(t.subSecKey));

            cctz::time_zone tz = cctz::utc_time_zone();

            // Attempt to use geolocation information to
            // find the proper timezone and adjust the timestamp
            GeoLocation geo;
            if (extractGeo(geo))
            {
                tz = Timezone::lookupTimezone(geo.latitude, geo.longitude);
                LOGD << "Using DateTime+GeoTZ as capture time";
            }
            else
            {
                LOGD << "No geolocation for timezone lookup, assuming UTC";
            }

            return Timezone::getUTCEpoch(year, month, day, hour, minute, second, msecs, tz);
        }

        return 0.0;
    }

    int ExifParser::extractImageOrientation()
    {
        auto k = findExifKey({"Exif.Image.Orientation"});
        if (k != exifData.end())
        {
            return static_cast<int>(k->toInt64());
        }

        return 1;
    }

    bool ExifParser::extractCameraOrientation(CameraOrientation &cameraOri)
    {
        auto pk = findXmpKey({"Xmp.drone-dji.GimbalPitchDegree", "Xmp.Camera.Pitch"});
        auto yk = findXmpKey({"Xmp.drone-dji.GimbalYawDegree", "Xmp.drone-dji.FlightYawDegree", "Xmp.Camera.Yaw"});
        auto rk = findXmpKey({"Xmp.drone-dji.GimbalRollDegree", "Xmp.Camera.Roll"});

        if (pk == xmpData.end() || yk == xmpData.end() || rk == xmpData.end())
        {
            cameraOri.pitch = -90;
            cameraOri.yaw = 0;
            cameraOri.roll = 0;
            return false;
        }
        cameraOri.pitch = static_cast<double>(pk->toFloat());
        cameraOri.yaw = static_cast<double>(yk->toFloat());
        cameraOri.roll = static_cast<double>(rk->toFloat());

        // TODO: JSON orientation database
        if (extractMake() == "senseFly")
        {
            cameraOri.pitch += -90;
            cameraOri.roll *= -1;
        }

        return true;
    }

    bool ExifParser::extractFlightSpeed(FlightSpeed &speed)
    {
        // Priority 1: DJI XMP proprietary tags (3D vector components)
        auto xk = findXmpKey({"Xmp.drone-dji.FlightXSpeed"});
        auto yk = findXmpKey({"Xmp.drone-dji.FlightYSpeed"});
        auto zk = findXmpKey({"Xmp.drone-dji.FlightZSpeed"});

        if (xk != xmpData.end() && yk != xmpData.end() && zk != xmpData.end())
        {
            speed.x = static_cast<double>(xk->toFloat());
            speed.y = static_cast<double>(yk->toFloat());
            speed.z = static_cast<double>(zk->toFloat());
            return true;
        }

        // Priority 2: EXIF standard GPS speed (scalar only)
        auto gpsSpeed = findExifKey("Exif.GPSInfo.GPSSpeed");
        if (gpsSpeed != exifData.end())
        {
            double speedVal = evalFrac(gpsSpeed->toRational());

            auto gpsSpeedRef = findExifKey("Exif.GPSInfo.GPSSpeedRef");
            std::string ref = "K"; // Default: km/h
            if (gpsSpeedRef != exifData.end())
                ref = gpsSpeedRef->toString();

            // Convert to m/s
            double speedMs;
            if (ref == "K")
                speedMs = speedVal / 3.6;
            else if (ref == "M")
                speedMs = speedVal / 2.237;
            else if (ref == "N")
                speedMs = speedVal / 1.944;
            else
                speedMs = speedVal / 3.6; // Fallback km/h

            // Scalar speed: store as horizontal magnitude (x=speed, y=0, z=0)
            speed.x = speedMs;
            speed.y = 0;
            speed.z = 0;
            return true;
        }

        return false;
    }

    bool ExifParser::extractGpsAccuracy(GpsAccuracy &accuracy)
    {
        bool found = false;

        // Priority 1: XMP Camera namespace tags (senseFly/Parrot/MicaSense)
        auto xyAcc = findXmpKey({"Xmp.Camera.GPSXYAccuracy"});
        auto zAcc = findXmpKey({"Xmp.Camera.GPSZAccuracy"});

        if (xyAcc != xmpData.end())
        {
            accuracy.xyAccuracy = static_cast<double>(xyAcc->toFloat());
            found = true;
        }
        if (zAcc != xmpData.end())
        {
            accuracy.zAccuracy = static_cast<double>(zAcc->toFloat());
            found = true;
        }

        if (found) return true;

        // Priority 2: DJI RTK XMP tags
        auto rtkLon = findXmpKey({"Xmp.drone-dji.RtkStdLon"});
        auto rtkLat = findXmpKey({"Xmp.drone-dji.RtkStdLat"});
        auto rtkHgt = findXmpKey({"Xmp.drone-dji.RtkStdHgt"});

        if (rtkLon != xmpData.end() && rtkLat != xmpData.end())
        {
            double lon = static_cast<double>(rtkLon->toFloat());
            double lat = static_cast<double>(rtkLat->toFloat());
            accuracy.xyAccuracy = std::sqrt(lon * lon + lat * lat);
            found = true;
        }
        if (rtkHgt != xmpData.end())
        {
            accuracy.zAccuracy = static_cast<double>(rtkHgt->toFloat());
            found = true;
        }

        if (found) return true;

        // Priority 3: EXIF standard tags
        auto hPosError = findExifKey("Exif.GPSInfo.GPSHPositioningError");
        if (hPosError != exifData.end())
        {
            accuracy.xyAccuracy = evalFrac(hPosError->toRational());
            found = true;
        }

        auto gpsDop = findExifKey("Exif.GPSInfo.GPSDOP");
        if (gpsDop != exifData.end())
        {
            accuracy.dop = evalFrac(gpsDop->toRational());
            found = true;
        }

        return found;
    }

    bool ExifParser::extractGpsDirection(GpsDirection &direction)
    {
        auto imgDir = findExifKey("Exif.GPSInfo.GPSImgDirection");
        if (imgDir != exifData.end())
        {
            direction.imgDirection = evalFrac(imgDir->toRational());

            // Check reference: T = true north (default), M = magnetic north
            auto imgDirRef = findExifKey("Exif.GPSInfo.GPSImgDirectionRef");
            direction.imgDirectionRef = "T";
            if (imgDirRef != exifData.end())
                direction.imgDirectionRef = imgDirRef->toString();

            direction.hasImgDirection = true;
        }

        auto track = findExifKey("Exif.GPSInfo.GPSTrack");
        if (track != exifData.end())
        {
            direction.track = evalFrac(track->toRational());

            // Check reference: T = true north (default), M = magnetic north
            auto trackRef = findExifKey("Exif.GPSInfo.GPSTrackRef");
            direction.trackRef = "T";
            if (trackRef != exifData.end())
                direction.trackRef = trackRef->toString();

            direction.hasTrack = true;
        }

        return direction.hasData();
    }

    bool ExifParser::extractPanoramaInfo(PanoramaInfo &info)
    {
        auto imSize = extractImageSize();

        // Defaults
        info.projectionType = "equirectangular";
        info.croppedWidth = imSize.width;
        info.croppedHeight = imSize.height;
        info.croppedX = 0;
        info.croppedY = 0;
        info.poseHeading = 0.0f;
        info.posePitch = 0.0f;
        info.poseRoll = 0.0f;

        auto projectionType = findXmpKey({"Xmp.GPano.ProjectionType"});
        auto croppedWidth = findXmpKey({"Xmp.GPano.CroppedAreaImageWidthPixels"});
        auto croppedHeight = findXmpKey({"Xmp.GPano.CroppedAreaImageHeightPixels"});
        auto croppedX = findXmpKey({"Xmp.GPano.CroppedAreaLeftPixels"});
        auto croppedY = findXmpKey({"Xmp.GPano.CroppedAreaTopPixels"});
        auto poseHeading = findXmpKey({"Xmp.GPano.PoseHeadingDegrees"});
        auto posePitch = findXmpKey({"Xmp.GPano.PosePitchDegrees"});
        auto poseRoll = findXmpKey({"Xmp.GPano.PoseRollDegrees"});

        if (projectionType != xmpData.end())
            info.projectionType = projectionType->toString();
        if (croppedWidth != xmpData.end() && croppedHeight != xmpData.end())
        {
            info.croppedWidth = croppedWidth->toInt64();
            info.croppedHeight = croppedHeight->toInt64();
        }
        if (croppedX != xmpData.end())
            info.croppedX = croppedX->toInt64();
        if (croppedY != xmpData.end())
            info.croppedY = croppedY->toInt64();
        if (poseHeading != xmpData.end())
            info.poseHeading = poseHeading->toFloat();
        if (posePitch != xmpData.end())
            info.posePitch = posePitch->toFloat();
        if (poseRoll != xmpData.end())
            info.poseRoll = poseRoll->toFloat();

        return true;
    }

    void ExifParser::printAllTags()
    {
        Exiv2::ExifData::const_iterator end = exifData.end();
        for (Exiv2::ExifData::const_iterator i = exifData.begin(); i != end; ++i)
        {
            const char *tn = i->typeName();
            std::cout << i->key() << " "
                      << i->value()
                      << " | " << tn
                      << std::endl;
        }
        Exiv2::XmpData::const_iterator xend = xmpData.end();
        for (Exiv2::XmpData::const_iterator i = xmpData.begin(); i != xend; ++i)
        {
            const char *tn = i->typeName();
            std::cout << i->key() << " "
                      << i->value()
                      << " | " << tn
                      << std::endl;
        }
    }

    bool ExifParser::hasExif()
    {
        return !exifData.empty();
    }

    bool ExifParser::hasXmp()
    {
        return !xmpData.empty();
    }

    bool ExifParser::hasTags()
    {
        return this->hasExif() || this->hasXmp();
    }

}
