/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include "gdal_inc.h"
#include "gdal_priv.h"
#include "gdal_utils.h"
#include "ogrsf_frmts.h"
#include "ogr_spatialref.h"
#include "cpl_conv.h"
#include "cpl_vsi.h"

#include "vector_query.h"
#include "exceptions.h"
#include "logger.h"
#include "utils.h"
#include "json.h"

namespace ddb
{

    namespace
    {

        struct FormatSpec
        {
            const char *driver;
            const char *vsiExt;
            const char *layerCreationOpts; // RFC7946=YES etc.
        };

        FormatSpec resolveOutputFormat(const std::string &fmt)
        {
            std::string f = fmt;
            std::transform(f.begin(), f.end(), f.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (f.empty() || f == "geojson" || f == "json")
                return {"GeoJSON", "geojson", "RFC7946=YES"};
            if (f == "gml")
                return {"GML", "gml", nullptr};
            throw InvalidArgsException("Unsupported output format: " + fmt);
        }

        OGRLayer *findLayer(GDALDataset *ds, const std::string &name)
        {
            if (!name.empty()) {
                OGRLayer *l = ds->GetLayerByName(name.c_str());
                if (!l)
                    throw InvalidArgsException("Layer not found: " + name);
                return l;
            }
            if (ds->GetLayerCount() == 0)
                throw InvalidArgsException("Vector has no layers");
            return ds->GetLayer(0);
        }

        std::string geomTypeName(OGRwkbGeometryType t)
        {
            return OGRGeometryTypeToName(t) ? OGRGeometryTypeToName(t) : "Unknown";
        }

        std::string srsAuthCode(OGRSpatialReference *srs)
        {
            if (!srs) return "";
            const char *authName = srs->GetAuthorityName(nullptr);
            const char *authCode = srs->GetAuthorityCode(nullptr);
            if (authName && authCode)
                return std::string(authName) + ":" + authCode;
            char *wkt = nullptr;
            srs->exportToWkt(&wkt);
            std::string out = wkt ? wkt : "";
            CPLFree(wkt);
            return out;
        }

        json layerToJson(OGRLayer *layer, bool includeFields)
        {
            json lj;
            lj["name"] = layer->GetName();
            lj["geometryType"] = geomTypeName(layer->GetGeomType());

            OGRSpatialReference *sr = layer->GetSpatialRef();
            lj["srs"] = srsAuthCode(sr);

            OGREnvelope env;
            if (layer->GetExtent(&env, TRUE) == OGRERR_NONE)
                lj["extent"] = {env.MinX, env.MinY, env.MaxX, env.MaxY};
            else
                lj["extent"] = nullptr;

            const GIntBig cnt = layer->GetFeatureCount(TRUE);
            lj["featureCount"] = static_cast<long long>(cnt);

            if (includeFields) {
                json fields = json::array();
                OGRFeatureDefn *defn = layer->GetLayerDefn();
                const int n = defn->GetFieldCount();
                for (int i = 0; i < n; i++) {
                    OGRFieldDefn *fd = defn->GetFieldDefn(i);
                    json fj;
                    fj["name"] = fd->GetNameRef();
                    fj["type"] = OGRFieldDefn::GetFieldTypeName(fd->GetType());
                    fj["width"] = fd->GetWidth();
                    fj["precision"] = fd->GetPrecision();
                    fields.push_back(fj);
                }
                lj["fields"] = fields;
            }
            return lj;
        }

    } // anonymous namespace

    // ---------------------------------------------------------------------

    std::string queryVector(const std::string &vectorPath,
                            const std::string &layerName,
                            const double *bbox,
                            const std::string &bboxSrs,
                            int maxFeatures,
                            int startIndex,
                            const std::string &outputFormat)
    {
        if (vectorPath.empty())
            throw InvalidArgsException("queryVector: vectorPath empty");
        if (maxFeatures < 0)
            throw InvalidArgsException("queryVector: maxFeatures < 0");
        if (startIndex < 0)
            throw InvalidArgsException("queryVector: startIndex < 0");

        const FormatSpec fmt = resolveOutputFormat(outputFormat);

        auto *hSrc = static_cast<GDALDataset *>(
            GDALOpenEx(vectorPath.c_str(),
                       GDAL_OF_VECTOR | GDAL_OF_READONLY,
                       nullptr, nullptr, nullptr));
        if (!hSrc)
            throw GDALException("Cannot open vector: " + vectorPath);

        OGRLayer *layer = nullptr;
        try {
            layer = findLayer(hSrc, layerName);
        } catch (...) {
            GDALClose(hSrc);
            throw;
        }

        // Apply spatial filter, reprojecting bbox to layer SRS if needed.
        if (bbox != nullptr) {
            const std::string requestedSrs =
                bboxSrs.empty() ? std::string("EPSG:4326") : bboxSrs;
            double minX = bbox[0], minY = bbox[1], maxX = bbox[2], maxY = bbox[3];

            OGRSpatialReference *layerSrs = layer->GetSpatialRef();
            if (layerSrs) {
                OGRSpatialReference reqSrs;
                if (reqSrs.SetFromUserInput(requestedSrs.c_str()) != OGRERR_NONE) {
                    GDALClose(hSrc);
                    throw InvalidArgsException("Invalid SRS: " + requestedSrs);
                }
                reqSrs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
                layerSrs->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
                if (!reqSrs.IsSame(layerSrs)) {
                    OGRCoordinateTransformation *t =
                        OGRCreateCoordinateTransformation(&reqSrs, layerSrs);
                    if (!t) {
                        GDALClose(hSrc);
                        throw GDALException(
                            "Cannot create bbox coordinate transformation from " +
                            requestedSrs + " to layer SRS");
                    }
                    // Transform all 4 corners
                    double xs[4] = {minX, maxX, maxX, minX};
                    double ys[4] = {minY, minY, maxY, maxY};
                    const int ok = t->Transform(4, xs, ys);
                    OCTDestroyCoordinateTransformation(
                        OGRCoordinateTransformation::ToHandle(t));
                    if (!ok) {
                        GDALClose(hSrc);
                        throw GDALException(
                            "bbox coordinate transformation failed (" +
                            requestedSrs + " -> layer SRS); refusing to apply "
                            "the bbox in the wrong CRS.");
                    }
                    minX = *std::min_element(xs, xs + 4);
                    maxX = *std::max_element(xs, xs + 4);
                    minY = *std::min_element(ys, ys + 4);
                    maxY = *std::max_element(ys, ys + 4);
                }
            }
            layer->SetSpatialFilterRect(minX, minY, maxX, maxY);
        }

        // Build GDALVectorTranslate options for selected layer + paging.
        const std::string vsiPath = "/vsimem/ddb-vquery-" +
                                    utils::generateRandomString(16) +
                                    "." + fmt.vsiExt;

        std::vector<std::string> argStore;
        argStore.emplace_back("-f"); argStore.emplace_back(fmt.driver);
        argStore.emplace_back("-t_srs"); argStore.emplace_back("EPSG:4326");
        if (fmt.layerCreationOpts) {
            argStore.emplace_back("-lco");
            argStore.emplace_back(fmt.layerCreationOpts);
        }
        // Pagination: when startIndex > 0 we cannot rely on -limit alone, so
        // we route the request through OGR SQL with LIMIT/OFFSET using the
        // SQLite dialect (works against any OGR data source). Otherwise we
        // keep the original layer-restricted invocation with -limit.
        const bool usePagedSql = (startIndex > 0);
        std::string sqlStmt;
        if (usePagedSql) {
            sqlStmt = std::string("SELECT * FROM \"") + layer->GetName() + "\"";
            if (maxFeatures > 0)
                sqlStmt += " LIMIT " + std::to_string(maxFeatures);
            sqlStmt += " OFFSET " + std::to_string(startIndex);
            argStore.emplace_back("-sql"); argStore.emplace_back(sqlStmt);
            argStore.emplace_back("-dialect"); argStore.emplace_back("SQLITE");
        } else {
            if (maxFeatures > 0) {
                argStore.emplace_back("-limit");
                argStore.emplace_back(std::to_string(maxFeatures));
            }
            // Restrict to a single layer (the one we want).
            argStore.emplace_back(layer->GetName());
        }

        std::vector<char *> argv;
        argv.reserve(argStore.size() + 1);
        for (auto &s : argStore) argv.push_back(const_cast<char *>(s.c_str()));
        argv.push_back(nullptr);

        GDALVectorTranslateOptions *opts =
            GDALVectorTranslateOptionsNew(argv.data(), nullptr);
        if (!opts) {
            GDALClose(hSrc);
            throw GDALException("Cannot build GDALVectorTranslate options");
        }

        // Re-open as ReservedHandle list (GDALDataset*[])
        GDALDatasetH srcArr[1] = {hSrc};
        int usageErr = 0;
        GDALDatasetH hOut = GDALVectorTranslate(vsiPath.c_str(), nullptr,
                                                1, srcArr, opts, &usageErr);
        GDALVectorTranslateOptionsFree(opts);
        GDALClose(hSrc);

        if (!hOut || usageErr) {
            if (hOut) GDALClose(hOut);
            VSIUnlink(vsiPath.c_str());
            throw GDALException("Vector translate failed for " + vectorPath);
        }
        GDALClose(hOut);

        vsi_l_offset size = 0;
        GByte *buf = VSIGetMemFileBuffer(vsiPath.c_str(), &size, TRUE);
        if (!buf) {
            throw GDALException("Empty vector query result");
        }
        std::string result(reinterpret_cast<char *>(buf),
                           static_cast<size_t>(size));
        VSIFree(buf);

        return result;
    }

    // ---------------------------------------------------------------------

    std::string describeVector(const std::string &vectorPath,
                               const std::string &layerName)
    {
        if (vectorPath.empty())
            throw InvalidArgsException("describeVector: vectorPath empty");

        auto *hSrc = static_cast<GDALDataset *>(
            GDALOpenEx(vectorPath.c_str(),
                       GDAL_OF_VECTOR | GDAL_OF_READONLY,
                       nullptr, nullptr, nullptr));
        if (!hSrc)
            throw GDALException("Cannot open vector: " + vectorPath);

        json out;
        const char *drvName = hSrc->GetDriverName();
        out["driver"] = drvName ? drvName : "";

        json layers = json::array();
        if (!layerName.empty()) {
            OGRLayer *l = hSrc->GetLayerByName(layerName.c_str());
            if (!l) {
                GDALClose(hSrc);
                throw InvalidArgsException("Layer not found: " + layerName);
            }
            layers.push_back(layerToJson(l, true));
        } else {
            const int n = hSrc->GetLayerCount();
            for (int i = 0; i < n; i++) {
                OGRLayer *l = hSrc->GetLayer(i);
                if (l) layers.push_back(layerToJson(l, true));
            }
        }
        out["layers"] = layers;

        GDALClose(hSrc);
        return out.dump();
    }

} // namespace ddb
