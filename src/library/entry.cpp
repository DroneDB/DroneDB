/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <exiv2/exiv2.hpp>
#include <cmath>
#include "dbops.h"
#include "entry.h"
#include "entry_handler.h"

#include "ddb.h"
#include "constants.h"

#include "mio.h"
#include "utils.h"
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
                throw FSException(path.string() + " does not exist");
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
                LOGD << "Cannot check " << path.string() << " .ddb presence: " << e.what();
            }
        }
        else
        {
            if (entry.hash == "" && withHash)
                entry.hash = Hash::fileSHA256(path.string());

            entry.size = p.getSize();
            entry.type = fingerprint(p.get(), ctx);

            entryHandler(entry.type).parseMetadata(path, entry, ctx);
        }
    }

    Geographic2D getRasterCoordinate(OGRCoordinateTransformationH hTransform, double *geotransform, double x, double y)
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
        double bottom = relAltitude * tan(utils::deg2rad(90.0 + pitch) - 0.5 * yView);
        double top = relAltitude * tan(utils::deg2rad(90.0 + pitch) + 0.5 * yView);
        double left = relAltitude * tan(utils::deg2rad(cameraOri.roll) - 0.5 * xView);
        double right = relAltitude * tan(utils::deg2rad(cameraOri.roll) + 0.5 * xView);
        // ... of picture.

        //    LOGD << "xView: " << utils::rad2deg(xView);
        //    LOGD << "yView: " << utils::rad2deg(yView);
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

    void Entry::toJSON(json &j) const
    {
        j["path"] = this->path;
        if (this->hash != "")
            j["hash"] = this->hash;
        j["type"] = this->type;
        if (!this->properties.empty())
            j["properties"] = this->properties;
        j["mtime"] = this->mtime;
        j["size"] = this->size;
        j["depth"] = this->depth;

        if (!this->point_geom.empty())
            j["point_geom"] = this->point_geom.toGeoJSON();
        if (!this->polygon_geom.empty())
            j["polygon_geom"] = this->polygon_geom.toGeoJSON();

        if (!this->meta.empty())
            j["meta"] = this->meta;
    }

    std::string Entry::toJSONString() const
    {
        json j;
        toJSON(j);
        std::stringstream s;
        s << j;
        return s.str();
    }

    void Entry::fromJSON(const json &j)
    {
        j.at("path").get_to(this->path);
        if (j.contains("hash") && !j.at("hash").is_null())
            j.at("hash").get_to(this->hash);
        j.at("type").get_to(this->type);
        j.at("size").get_to(this->size);
        j.at("depth").get_to(this->depth);
        j.at("mtime").get_to(this->mtime);

        // TODO: Add
        // j.at("properties").get_to(e.properties);
    }

    bool Entry::toGeoJSON(json &j, BasicGeometryType type)
    {
        // Only export entries that have valid geometries
        std::vector<BasicGeometry *> geoms;
        if (!point_geom.empty() && (type == BasicGeometryType::BGAuto || type == BasicGeometryType::BGPoint))
            geoms.push_back(&point_geom);
        if (!polygon_geom.empty() && (type == BasicGeometryType::BGAuto || type == BasicGeometryType::BGPolygon))
            geoms.push_back(&polygon_geom);
        if (geoms.size() == 0)
            return false;

        json p;
        p["path"] = this->path;
        if (this->hash != "")
            p["hash"] = this->hash;
        p["type"] = this->type;
        p["mtime"] = this->mtime;
        p["size"] = this->size;

        // Populate properties
        for (json::iterator it = this->properties.begin(); it != this->properties.end(); ++it)
        {
            p[it.key()] = it.value();
        }

        // QGIS does not support GeometryCollections, so we only output the first
        // available geometry
        j = geoms[0]->toGeoJSON();
        j["properties"] = p;
        if (!this->meta.empty())
            j["properties"]["meta"] = this->meta;

        return true;
    }

    std::string Entry::toString()
    {
        std::ostringstream s;
        s << "Path: " << this->path << "\n";
        if (this->hash != "")
            s << "SHA256: " << this->hash << "\n";
        s << "Type: " << typeToHuman(this->type) << " (" << this->type << ")" << "\n";

        for (json::iterator it = this->properties.begin(); it != this->properties.end(); ++it)
        {
            std::string k = it.key();
            if (k.length() > 0)
                k[0] = utils::toUpperChar(k[0]);

            if (k == "Bands")
            {
                s << k << ": " << it.value().size() << " [";
                for (json::iterator b = it.value().begin(); b != it.value().end(); b++)
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
            const auto db = ddb::open(ddbPath.string(), false);

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
