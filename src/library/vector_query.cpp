/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <algorithm>
#include <memory>
#include <optional>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include "gdal_raii.h"
#include "stringutil.h"
#include "gdal_priv.h"
#include "gdal_utils.h"
#include "ogrsf_frmts.h"
#include "ogr_spatialref.h"
#include "cpl_conv.h"
#include "cpl_vsi.h"

#include "vector_query.h"
#include "raster_cache.h"
#include "exceptions.h"
#include "logger.h"
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
            const std::string f = str::toLowerCopy(fmt);
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

        std::string srsAuthCode(const OGRSpatialReference *srs)
        {
            if (!srs) return "";
            const char *authName = srs->GetAuthorityName(nullptr);
            const char *authCode = srs->GetAuthorityCode(nullptr);
            if (authName && authCode)
                return std::string(authName) + ":" + authCode;
            char *wkt = nullptr;
            srs->exportToWkt(&wkt);
            return takeGdalString(wkt);
        }

        json layerToJson(OGRLayer *layer, bool includeFields)
        {
            json lj;
            lj["name"] = layer->GetName();
            lj["geometryType"] = geomTypeName(layer->GetGeomType());

            const OGRSpatialReference *sr = layer->GetSpatialRef();
            lj["srs"] = srsAuthCode(sr);

            OGREnvelope env;
            if (layer->GetExtent(&env, TRUE) == OGRERR_NONE)
                lj["extent"] = {env.MinX, env.MinY, env.MaxX, env.MaxY};
            else
                lj["extent"] = nullptr;

            const GIntBig cnt = layer->GetFeatureCount(FALSE);
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

    namespace
    {
        // A layer of a cached vector dataset; clears the filters set on it when done, so
        // the next user of the dataset starts clean
        class LayerScope
        {
            DatasetLease dataset_;
            OGRLayer *layer_ = nullptr;

        public:
            LayerScope(const std::string &vectorPath, const std::string &layerName)
                : dataset_(RasterCache::instance().acquireVectorDataset(vectorPath))
            {
                layer_ = findLayer(GDALDataset::FromHandle(dataset_->handle()), layerName);
            }
            ~LayerScope()
            {
                layer_->SetSpatialFilter(nullptr);
                layer_->SetAttributeFilter(nullptr);
                layer_->ResetReading();
            }
            LayerScope(const LayerScope &) = delete;
            LayerScope &operator=(const LayerScope &) = delete;

            GDALDatasetH dataset() const { return dataset_->handle(); }
            OGRLayer *operator->() const { return layer_; }
            OGRLayer *get() const { return layer_; }
        };

        // query.bbox in the layer's SRS (corners reprojected from query.bboxSrs)
        std::optional<OGREnvelope> layerFilterRect(OGRLayer *layer, const VectorQuery &query)
        {
            if (!query.bbox)
                return std::nullopt;

            const std::string requestedSrs = query.bboxSrs.empty() ? std::string("EPSG:4326") : query.bboxSrs;
            OGREnvelope rect;
            rect.MinX = (*query.bbox)[0];
            rect.MinY = (*query.bbox)[1];
            rect.MaxX = (*query.bbox)[2];
            rect.MaxY = (*query.bbox)[3];

            const OGRSpatialReference *layerSrsRef = layer->GetSpatialRef();
            if (!layerSrsRef)
                return rect;

            // GetSpatialRef() returns a const pointer (GDAL >= 3.x); take a mutable copy so
            // we can normalize the axis mapping strategy without mutating the dataset's
            // shared SRS object.
            OGRSpatialReference layerSrs(*layerSrsRef);
            OGRSpatialReference reqSrs;
            if (reqSrs.SetFromUserInput(requestedSrs.c_str()) != OGRERR_NONE)
                throw InvalidArgsException("Invalid SRS: " + requestedSrs);
            reqSrs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
            layerSrs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
            if (reqSrs.IsSame(&layerSrs))
                return rect;

            std::unique_ptr<OGRCoordinateTransformation> t(OGRCreateCoordinateTransformation(&reqSrs, &layerSrs));
            if (!t)
                throw GDALException("Cannot create bbox coordinate transformation from " + requestedSrs +
                                    " to layer SRS");
            // Transform all 4 corners
            double xs[4] = {rect.MinX, rect.MaxX, rect.MaxX, rect.MinX};
            double ys[4] = {rect.MinY, rect.MinY, rect.MaxY, rect.MaxY};
            if (!t->Transform(4, xs, ys))
                throw GDALException("bbox coordinate transformation failed (" + requestedSrs +
                                    " -> layer SRS); refusing to apply the bbox in the wrong CRS.");
            rect.MinX = *std::min_element(xs, xs + 4);
            rect.MaxX = *std::max_element(xs, xs + 4);
            rect.MinY = *std::min_element(ys, ys + 4);
            rect.MaxY = *std::max_element(ys, ys + 4);
            return rect;
        }

        // "<column> IN (1,2,3)" selecting query.fids, or "" for every feature
        std::string fidPredicate(const VectorQuery &query, const std::string &column)
        {
            if (query.fids.empty())
                return "";
            std::string in;
            for (const auto fid : query.fids)
                in += (in.empty() ? "" : ",") + std::to_string(fid);
            return column + " IN (" + in + ")";
        }

        // The attribute filter of query: its feature ids and its where expression
        std::string attributeFilter(const VectorQuery &query, const std::string &fidColumn)
        {
            const std::string fids = fidPredicate(query, fidColumn);
            if (query.where.empty())
                return fids;
            if (fids.empty())
                return "(" + query.where + ")";
            return fids + " AND (" + query.where + ")";
        }

        // Identifier quoted for SQL: between double quotes, embedded quotes doubled
        std::string quoteIdentifier(const std::string &name)
        {
            std::string quoted = "\"";
            for (const char c : name)
            {
                if (c == '"')
                    quoted.push_back('"');
                quoted.push_back(c);
            }
            return quoted + "\"";
        }

        std::string formatNumber(double value)
        {
            return CPLSPrintf("%.17g", value);
        }
    } // namespace

    std::string queryVector(const std::string &vectorPath, const VectorQuery &query)
    {
        if (vectorPath.empty())
            throw InvalidArgsException("queryVector: vectorPath empty");
        if (query.maxFeatures < 0)
            throw InvalidArgsException("queryVector: maxFeatures < 0");
        if (query.startIndex < 0)
            throw InvalidArgsException("queryVector: startIndex < 0");

        const FormatSpec fmt = resolveOutputFormat(query.outputFormat);
        LayerScope layer(vectorPath, query.layerName);
        const std::string layerName = layer->GetName();

        // Build GDALVectorTranslate options for selected layer + paging.
        const VsiMemFile out = VsiMemFile::unique("ddb-vquery-", std::string(".") + fmt.vsiExt);

        // A layer without an SRS (a local CAD drawing, DDB-12) cannot be
        // reprojected: return its own coordinates, as plain GeoJSON (RFC 7946
        // mandates WGS84, so its layer option is not applied either).
        const bool layerHasSrs = layer->GetSpatialRef() != nullptr;

        std::vector<std::string> argStore;
        argStore.emplace_back("-f"); argStore.emplace_back(fmt.driver);
        if (layerHasSrs) {
            argStore.emplace_back("-t_srs"); argStore.emplace_back("EPSG:4326");
        }
        if (fmt.layerCreationOpts && layerHasSrs) {
            argStore.emplace_back("-lco");
            argStore.emplace_back(fmt.layerCreationOpts);
        }
        // Feature ids are the source FIDs, not the position in this page
        argStore.emplace_back("-preserve_fid");
        // -spat also filters the -sql result below (a filter set on the layer would not)
        if (const auto rect = layerFilterRect(layer.get(), query)) {
            argStore.emplace_back("-spat");
            for (const double v : {rect->MinX, rect->MinY, rect->MaxX, rect->MaxY})
                argStore.emplace_back(formatNumber(v));
        }
        // Pagination: when startIndex > 0 we cannot rely on -limit alone, so
        // we route the request through SQL with LIMIT/OFFSET using the
        // SQLite dialect (works against any OGR data source). Otherwise we
        // keep the layer-restricted invocation with -limit.
        if (query.startIndex > 0) {
            const char *fidColumn = layer->GetFIDColumn();
            const std::string where =
                attributeFilter(query, fidColumn != nullptr && *fidColumn != '\0' ? quoteIdentifier(fidColumn) : "rowid");
            std::string sql = "SELECT * FROM " + quoteIdentifier(layerName);
            if (!where.empty())
                sql += " WHERE " + where;
            sql += " LIMIT " + std::to_string(query.maxFeatures > 0 ? query.maxFeatures : -1);
            sql += " OFFSET " + std::to_string(query.startIndex);
            argStore.emplace_back("-sql"); argStore.emplace_back(sql);
            argStore.emplace_back("-dialect"); argStore.emplace_back("SQLITE");
            // The result layer keeps the layer's name (and the ids their prefix)
            argStore.emplace_back("-nln"); argStore.emplace_back(layerName);
        } else {
            const std::string where = attributeFilter(query, "FID");
            if (!where.empty()) {
                argStore.emplace_back("-where"); argStore.emplace_back(where);
            }
            if (query.maxFeatures > 0) {
                argStore.emplace_back("-limit");
                argStore.emplace_back(std::to_string(query.maxFeatures));
            }
            // Restrict to a single layer (the one we want).
            argStore.emplace_back(layerName);
        }

        std::vector<char *> argv;
        argv.reserve(argStore.size() + 1);
        for (auto &arg : argStore) argv.push_back(const_cast<char *>(arg.c_str()));
        argv.push_back(nullptr);

        const GdalOptionsPtr<GDALVectorTranslateOptions> opts(
            GDALVectorTranslateOptionsNew(argv.data(), nullptr));
        if (!opts)
            throw GDALException("Cannot build GDALVectorTranslate options");

        GDALDatasetH srcArr[1] = {layer.dataset()};
        int usageErr = 0;
        // Closed at the end of the statement, before its bytes are taken: closing writes them
        const bool written = adoptDataset(GDALVectorTranslate(out.c_str(), nullptr, 1, srcArr,
                                                              opts.get(), &usageErr)) != nullptr;
        if (!written || usageErr)
            throw GDALException("Vector translate failed for " + vectorPath);

        vsi_l_offset size = 0;
        const auto buf = stealMemFileBuffer(out.path(), size);
        if (!buf) {
            throw GDALException("Empty vector query result");
        }
        return std::string(reinterpret_cast<char *>(buf.get()), static_cast<size_t>(size));
    }

    std::string queryVector(const std::string &vectorPath,
                            const std::string &layerName,
                            const double *bbox,
                            const std::string &bboxSrs,
                            int maxFeatures,
                            int startIndex,
                            const std::string &outputFormat)
    {
        VectorQuery query;
        query.layerName = layerName;
        if (bbox != nullptr)
            query.bbox = std::array<double, 4>{bbox[0], bbox[1], bbox[2], bbox[3]};
        query.bboxSrs = bboxSrs;
        query.maxFeatures = maxFeatures;
        query.startIndex = startIndex;
        query.outputFormat = outputFormat;
        return queryVector(vectorPath, query);
    }

    int64_t countVectorFeatures(const std::string &vectorPath, const VectorQuery &query)
    {
        if (vectorPath.empty())
            throw InvalidArgsException("countVectorFeatures: vectorPath empty");

        LayerScope layer(vectorPath, query.layerName);
        if (const auto rect = layerFilterRect(layer.get(), query))
            layer->SetSpatialFilterRect(rect->MinX, rect->MinY, rect->MaxX, rect->MaxY);
        const std::string where = attributeFilter(query, "FID");
        if (!where.empty() && layer->SetAttributeFilter(where.c_str()) != OGRERR_NONE)
            throw GDALException("Cannot filter " + vectorPath + " with " + where);

        // GeoPackage counts with its R-tree and primary key, without reading features.
        // A filter that SQLite rejects only shows as an error raised by the count.
        CPLErrorReset();
        const auto count = static_cast<int64_t>(layer->GetFeatureCount(TRUE));
        if (count < 0 || CPLGetLastErrorType() == CE_Failure)
            throw GDALException("Cannot count the features of " + vectorPath +
                                (where.empty() ? std::string() : " with " + where) + ": " + CPLGetLastErrorMsg());
        return count;
    }

    // ---------------------------------------------------------------------

    std::string describeVector(const std::string &vectorPath,
                               const std::string &layerName)
    {
        if (vectorPath.empty())
            throw InvalidArgsException("describeVector: vectorPath empty");

        const auto dataset = RasterCache::instance().acquireVectorDataset(vectorPath);
        auto *hSrc = GDALDataset::FromHandle(dataset->handle());

        json out;
        const char *drvName = hSrc->GetDriverName();
        out["driver"] = drvName ? drvName : "";

        json layers = json::array();
        if (!layerName.empty()) {
            OGRLayer *l = hSrc->GetLayerByName(layerName.c_str());
            if (!l)
                throw InvalidArgsException("Layer not found: " + layerName);
            layers.push_back(layerToJson(l, true));
        } else {
            const int n = hSrc->GetLayerCount();
            for (int i = 0; i < n; i++) {
                OGRLayer *l = hSrc->GetLayer(i);
                if (l) layers.push_back(layerToJson(l, true));
            }
        }
        out["layers"] = layers;

        return out.dump();
    }

} // namespace ddb
