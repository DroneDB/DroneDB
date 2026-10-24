/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <exiv2/exiv2.hpp>
#include <cmath>
#include "dbops.h"
#include "json.h"
#include "stringutil.h"
#include "mathutil.h"
#include "entry.h"
#include "entry_handler.h"

#include "ddb.h"
#include "constants.h"

#include "mio.h"
#include "gdal_priv.h"
#include "cpl_conv.h"
#include "ogr_spatialref.h"
#include "ogr_api.h"

namespace ddb
{

    void parseEntry(const fs::path &path, const fs::path &rootDirectory, Entry &entry, bool withHash)
    {
        FingerprintContext ctx;
        parseEntry(path, rootDirectory, entry, withHash, ctx);
    }

    void parseEntry(const fs::path &path, const fs::path &rootDirectory, Entry &entry, bool withHash, FingerprintContext &ctx)
    {
        entry.type = EntryType::Undefined;

        try
        {
            if (!fs::exists(path))
                throw FSException(io::toUtf8(path) + " does not exist");
        }
        catch (const fs::filesystem_error &e)
        {
            // Yes Windows will throw an exception in some cases :/
            throw FSException(e.what());
        }

        // Parse file
        io::Path p = io::Path(path);
        io::Path relPath = p.relativeTo(rootDirectory);

        entry.path = relPath.generic();
        entry.depth = relPath.depth();
        if (entry.mtime == 0)
            entry.mtime = p.getModifiedTime();

        if (fs::is_directory(path))
        {
            entry.type = EntryType::Directory;
            entry.hash = "";
            entry.size = 0;

            // Check for DroneDB dir
            try
            {
                if (fs::exists(path / DDB_FOLDER / DDB_DATABASE_FILE))
                {
                    parseDroneDBEntry(path, entry);
                }
            }
            catch (const fs::filesystem_error &e)
            {
                LOGD << "Cannot check " << io::toUtf8(path) << " .ddb presence: " << e.what();
            }
        }
        else
        {
            if (entry.hash == "" && withHash)
                entry.hash = Hash::fileSHA256(io::toUtf8(path));

            entry.size = p.getSize();
            entry.type = fingerprint(p.get(), ctx);

            entryHandler(entry.type).parseMetadata(path, entry, ctx);
        }
    }

    Geographic2D getRasterCoordinate(OGRCoordinateTransformationH hTransform, const double *geotransform, double x, double y)
    {
        double dfGeoX = geotransform[0] + geotransform[1] * x + geotransform[2] * y;
        double dfGeoY = geotransform[3] + geotransform[4] * x + geotransform[5] * y;

        if (OCTTransform(hTransform, 1, &dfGeoX, &dfGeoY, nullptr))
        {

            return Geographic2D(dfGeoX, dfGeoY);
        }
        else
        {
            throw GDALException("Cannot get raster coordinates of corner " + std::to_string(x) + "," + std::to_string(y));
        }
    }

    // Adapted from https://github.com/mountainunicycler/dronecamerafov/tree/master
    void calculateFootprint(const SensorSize &sensorSize, const GeoLocation &geo, const Focal &focal, const CameraOrientation &cameraOri, double relAltitude, BasicGeometry &geom)
    {
        auto utmZone = getUTMZone(geo.latitude, geo.longitude);

        auto center = toUTM(geo.latitude, geo.longitude, utmZone);

        double groundHeight = geo.altitude != 0.0 ? geo.altitude - relAltitude : relAltitude;

        // Field of view

        // Wide
        double xView = 2.0 * atan(sensorSize.width / (2.0 * focal.length));

        // Tall
        double yView = 2.0 * atan(sensorSize.height / (2.0 * focal.length));

        // Cap pitch to 60 degrees
        double pitch = cameraOri.pitch;
        if (pitch > -30)
        {
            LOGD << "Pitch cap exceeded (" << pitch << ") using nadir";
            pitch = -90; // set to nadir
        }

        // From drone to...
        double bottom = relAltitude * tan(ddb::deg2rad(90.0 + pitch) - 0.5 * yView);
        double top = relAltitude * tan(ddb::deg2rad(90.0 + pitch) + 0.5 * yView);
        double left = relAltitude * tan(ddb::deg2rad(cameraOri.roll) - 0.5 * xView);
        double right = relAltitude * tan(ddb::deg2rad(cameraOri.roll) + 0.5 * xView);
        // ... of picture.

        //    LOGD << "xView: " << ddb::rad2deg(xView);
        //    LOGD << "yView: " << ddb::rad2deg(yView);
        //    LOGD << "bottom: " << bottom;
        //    LOGD << "top: " << top;
        //    LOGD << "left: " << left;
        //    LOGD << "right: " << right;

        // Corners aligned north
        auto upperLeft = Projected2D(center.x + left, center.y + top);
        auto upperRight = Projected2D(center.x + right, center.y + top);
        auto lowerLeft = Projected2D(center.x + left, center.y + bottom);
        auto lowerRight = Projected2D(center.x + right, center.y + bottom);

        // Rotate
        upperLeft.rotate(center, -cameraOri.yaw);
        upperRight.rotate(center, -cameraOri.yaw);
        lowerLeft.rotate(center, -cameraOri.yaw);
        lowerRight.rotate(center, -cameraOri.yaw);

        //    LOGD << "UL: " << upperLeft;
        //    LOGD << "UR: " << upperRight;
        //    LOGD << "LL: " << lowerLeft;
        //    LOGD << "LR: " << lowerRight;

        // Convert to geographic
        auto ul = fromUTM(upperLeft, utmZone);
        auto ur = fromUTM(upperRight, utmZone);
        auto ll = fromUTM(lowerLeft, utmZone);
        auto lr = fromUTM(lowerRight, utmZone);

        geom.addPoint(ul.longitude, ul.latitude, groundHeight);
        geom.addPoint(ll.longitude, ll.latitude, groundHeight);
        geom.addPoint(lr.longitude, lr.latitude, groundHeight);
        geom.addPoint(ur.longitude, ur.latitude, groundHeight);
        geom.addPoint(ul.longitude, ul.latitude, groundHeight);
    }

    void to_json(json &j, const Entry &e)
    {
        j["path"] = e.path;
        if (e.hash != "")
            j["hash"] = e.hash;
        j["type"] = e.type;
        if (!e.properties.empty())
            j["properties"] = e.properties;
        j["mtime"] = e.mtime;
        j["size"] = e.size;
        j["depth"] = e.depth;

        if (!e.point_geom.empty())
            j["point_geom"] = e.point_geom.toGeoJSON();
        if (!e.polygon_geom.empty())
            j["polygon_geom"] = e.polygon_geom.toGeoJSON();

        if (!e.meta.empty())
            j["meta"] = e.meta;
    }

    void from_json(const json &j, Entry &e)
    {
        j.at("path").get_to(e.path);
        if (j.contains("hash") && !j.at("hash").is_null())
            j.at("hash").get_to(e.hash);
        j.at("type").get_to(e.type);
        j.at("size").get_to(e.size);
        j.at("depth").get_to(e.depth);
        j.at("mtime").get_to(e.mtime);

        // TODO: Add
        // j.at("properties").get_to(e.properties);
    }

    std::optional<json> Entry::toGeoJSON(BasicGeometryType type) const
    {
        // Only export entries that have valid geometries
        std::vector<const BasicGeometry *> geoms;
        if (!point_geom.empty() && (type == BasicGeometryType::BGAuto || type == BasicGeometryType::BGPoint))
            geoms.push_back(&point_geom);
        if (!polygon_geom.empty() && (type == BasicGeometryType::BGAuto || type == BasicGeometryType::BGPolygon))
            geoms.push_back(&polygon_geom);
        if (geoms.size() == 0)
            return std::nullopt;

        json p;
        p["path"] = this->path;
        if (this->hash != "")
            p["hash"] = this->hash;
        p["type"] = this->type;
        p["mtime"] = this->mtime;
        p["size"] = this->size;

        // Populate properties
        for (auto it = this->properties.begin(); it != this->properties.end(); ++it)
        {
            p[it.key()] = it.value();
        }

        // QGIS does not support GeometryCollections, so we only output the first
        // available geometry
        json j = geoms[0]->toGeoJSON();
        j["properties"] = p;
        if (!this->meta.empty())
            j["properties"]["meta"] = this->meta;

        // Constructed in place: a json converts to std::optional<json> in two ways (the optional's
        // converting constructor and json's own conversion operator), and compilers disagree on which
        return std::optional<json>(std::in_place, std::move(j));
    }

    std::string Entry::toString() const
    {
        std::ostringstream s;
        s << "Path: " << this->path << "\n";
        if (this->hash != "")
            s << "SHA256: " << this->hash << "\n";
        s << "Type: " << typeToHuman(this->type) << " (" << entryTypeValue(this->type) << ")" << "\n";

        for (auto it = this->properties.begin(); it != this->properties.end(); ++it)
        {
            std::string k = it.key();
            if (k.length() > 0)
                k[0] = str::toUpperChar(k[0]);

            if (k == "Bands")
            {
                s << k << ": " << it.value().size() << " [";
                for (auto b = it.value().begin(); b != it.value().end(); b++)
                {
                    auto band = b.value();
                    s << band["colorInterp"].get<std::string>() << "(" << band["type"].get<std::string>() << ")";
                    if (b + 1 != it.value().end())
                        s << ",";
                }
                s << "]\n";
            }
            else
            {
                s << k << ": " << (it.value().is_string() ? it.value().get<std::string>() : it.value().dump()) << "\n";
            }
        }

        s << "Modified Time: " << this->mtime << "\n";
        s << "Size: " << io::bytesToHuman(this->size) << "\n";
        // s << "Tree Depth: " << this->depth << "\n";
        if (!this->point_geom.empty())
            s << "Point Geometry: " << this->point_geom << "\n";
        if (!this->polygon_geom.empty())
            s << "Polygon Geometry: " << this->polygon_geom << "\n";
        if (!this->meta.empty())
            s << "Meta: " << this->meta.dump(4) << "\n";
        return s.str();
    }

    void parseDroneDBEntry(const fs::path &ddbPath, Entry &entry)
    {
        try
        {
            const auto db = ddb::open(io::toUtf8(ddbPath), false);

            // The size of the database is the sum of all entries' sizes
            auto q = db->query("SELECT SUM(size) FROM entries");
            if (q->fetch())
                entry.size = q->getInt64(0);

            entry.properties = db->getProperties();
            entry.type = EntryType::DroneDB;
        }
        catch (AppException &e)
        {
            LOGD << e.what();
            entry.type = EntryType::Directory;
        }
    }

    EntryType fingerprint(const fs::path &path)
    {
        FingerprintContext ctx;
        return fingerprint(path, ctx);
    }

    EntryType fingerprint(const fs::path &path, FingerprintContext &ctx)
    {
        ctx.populated = false;
        ctx.hasGeo = false;
        ctx.exivImage.reset();
        ctx.parser.reset();

        if (const EntryHandler *handler = findEntryHandlerByExtension(path))
            return handler->detect(path, ctx);

        return EntryType::Generic;
    }

}
