/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <fstream>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <vector>

#include "build_layout.h"
#include "randomutil.h"
#include "stringutil.h"
#include "cad.h"
#include "cancellation.h"
#include "coordstransformer.h"
#include "gdal_inc.h"
#include "json.h"
#include "vector.h"
#include "mvt.h"
#include "logger.h"
#include "mio.h"
#include "raster_cache.h"
#include "exceptions.h"
#include "gdal_raii.h"

namespace ddb
{

    namespace
    {

        // ---- helpers ---------------------------------------------------------

        [[noreturn]] void throwVectorWriteFailure(const std::string &operation,
                                                  const std::string &output,
                                                  int systemError,
                                                  const std::string &message)
        {
            const std::string description = operation + " failed for " + output + ": " + message;
            if (systemError == ENOSPC)
                throw RetryableBuildException(description, RetryableBuildReason::NoSpaceOnDevice);
            throw AppException(description);
        }

        // Compute total feature count and union envelope (in WGS84) for MAXZOOM.
        struct VectorStats
        {
            // -1 = unknown (no layer reported a cheap count; OGR convention).
            //  0 = confirmed empty (at least one layer reported 0, none reported >0).
            // >0 = sum of cheap counts from layers that supplied one.
            // Keeping these distinct is critical: computeMvtMaxZoom treats
            // featureCount==0 as "empty layer => MAXZOOM cap" short-circuit,
            // which on a real dataset with unknown count (e.g. DXF) would
            // produce hundreds of thousands of empty tiles.
            long long featureCount = -1;
            // WGS84 envelope (degrees)
            double minX =  std::numeric_limits<double>::max();
            double minY =  std::numeric_limits<double>::max();
            double maxX = -std::numeric_limits<double>::max();
            double maxY = -std::numeric_limits<double>::max();
            bool haveEnv = false;
        };

        // sourceSrs: SRS of layers that carry none (empty = assume WGS84).
        VectorStats computeStats(GDALDatasetH hSrcDS, const std::string &sourceSrs)
        {
            VectorStats s;

            const SrsPtr wgs84 = srsFromEpsg(4326);
            if (!wgs84)
                throw GDALException("computeStats: failed to import EPSG:4326 - PROJ database may be missing or corrupted");

            SrsPtr source;
            if (!sourceSrs.empty())
            {
                source = srsFromUserInput(sourceSrs.c_str());
                if (!source)
                    throw GDALException("computeStats: invalid source SRS " + sourceSrs);
            }
            OGRSpatialReferenceH hSource = source.get();

            const int layerCount = GDALDatasetGetLayerCount(hSrcDS);
            for (int i = 0; i < layerCount; i++)
            {
                OGRLayerH hLayer = GDALDatasetGetLayer(hSrcDS, i);
                if (!hLayer) continue;

                // OGR_L_GetFeatureCount(..., FALSE) returns -1 when the driver
                // cannot provide a cheap count (e.g. DXF, some KML setups).
                // Preserve the distinction between "unknown" (-1) and
                // "confirmed empty/non-negative" (>= 0): a layer that
                // reports 0 promotes the running total from -1 to 0, and
                // any layer that reports >0 contributes to the sum. If every
                // layer reports -1, featureCount stays -1 and the MVT
                // heuristic falls through to the area-only branch instead of
                // mistakenly short-circuiting to MAXZOOM=18.
                const GIntBig rawCount = OGR_L_GetFeatureCount(hLayer, FALSE);
                if (rawCount >= 0)
                {
                    if (s.featureCount < 0) s.featureCount = 0;
                    s.featureCount += static_cast<long long>(rawCount);
                }

                OGREnvelope env;
                if (OGR_L_GetExtent(hLayer, &env, TRUE) != OGRERR_NONE) continue;

                OGRSpatialReferenceH hSrs = OGR_L_GetSpatialRef(hLayer);
                if (!hSrs) hSrs = hSource;
                double xs[4] = {env.MinX, env.MaxX, env.MaxX, env.MinX};
                double ys[4] = {env.MinY, env.MinY, env.MaxY, env.MaxY};

                if (hSrs)
                {
                    const char *authCode = OSRGetAuthorityCode(hSrs, nullptr);
                    const char *authName = OSRGetAuthorityName(hSrs, nullptr);
                    bool isWgs84 = (authName && authCode &&
                                    std::string(authName) == "EPSG" &&
                                    std::string(authCode) == "4326");
                    if (!isWgs84)
                    {
                        const auto toWgs84 =
                            CoordsTransformer::tryCreate(*OGRSpatialReference::FromHandle(hSrs), srsOf(wgs84));
                        if (toWgs84)
                        {
                            if (!toWgs84->tryTransform(4, xs, ys))
                            {
                                LOGD << "computeStats: layer " << i << " transform failed";
                                continue;
                            }
                        }
                    }
                }

                const double mnX = std::min({xs[0], xs[1], xs[2], xs[3]});
                const double mxX = std::max({xs[0], xs[1], xs[2], xs[3]});
                const double mnY = std::min({ys[0], ys[1], ys[2], ys[3]});
                const double mxY = std::max({ys[0], ys[1], ys[2], ys[3]});

                if (!s.haveEnv)
                {
                    s.minX = mnX; s.maxX = mxX; s.minY = mnY; s.maxY = mxY;
                    s.haveEnv = true;
                }
                else
                {
                    s.minX = std::min(s.minX, mnX);
                    s.maxX = std::max(s.maxX, mxX);
                    s.minY = std::min(s.minY, mnY);
                    s.maxY = std::max(s.maxY, mxY);
                }
            }

            return s;
        }

        // Returns true if all source layers lack an SRS (typical for DXF).
        // In that case, callers must inject -s_srs (the CAD CRS, or EPSG:4326
        // for other inputs, matching the envelope computed in computeStats).
        bool allLayersMissSrs(GDALDatasetH hSrcDS)
        {
            const int n = GDALDatasetGetLayerCount(hSrcDS);
            if (n == 0) return false;
            for (int i = 0; i < n; i++)
            {
                OGRLayerH hLayer = GDALDatasetGetLayer(hSrcDS, i);
                if (hLayer && OGR_L_GetSpatialRef(hLayer) != nullptr) return false;
            }
            return true;
        }

        void checkDependencies(const std::string &input)
        {
            const auto deps = getVectorDependencies(input);
            std::vector<std::string> missing;
            const fs::path p = io::fromUtf8(input);
            for (const auto &d : deps)
            {
                if (!fs::exists(p.parent_path() / io::fromUtf8(d)))
                    missing.push_back(d);
            }
            if (!missing.empty())
            {
                std::string msg = "Dependencies missing for " + input + ": ";
                for (size_t i = 0; i < missing.size(); i++)
                {
                    if (i > 0) msg += ", ";
                    msg += missing[i];
                }
                throw BuildDepMissingException(msg, missing);
            }
        }

        // Convert input to GPKG with SPATIAL_INDEX=YES. Multi-layer sources are
        // preserved verbatim. reproject: to EPSG:4326, assigning sourceSrs to
        // layers without an SRS; otherwise (a local CAD drawing) the geometry
        // keeps its drawing coordinates and no SRS.
        void convertToGpkg(GDALDatasetH hSrcDS, const std::string &output,
                           const std::string &sourceSrs, bool reproject)
        {
            const bool needsSrs = reproject && allLayersMissSrs(hSrcDS);

            std::vector<std::string> argStore = {"-f", "GPKG"};
            if (reproject) {
                argStore.push_back("-t_srs");
                argStore.push_back("EPSG:4326");
            }
            for (const char *a : {"-lco", "SPATIAL_INDEX=YES",
                                  "-lco", "GEOMETRY_NAME=geom",
                                  "-lco", "FID=fid",
                                  "-nlt", "PROMOTE_TO_MULTI",
                                  // KML/KMZ have DateTime fields that several drivers warn on;
                                  // map to String for maximum compatibility.
                                  "-mapFieldType", "DateTime=String",
                                  "-skipfailures"})
                argStore.push_back(a);
            if (needsSrs) {
                argStore.push_back("-s_srs");
                argStore.push_back(sourceSrs);
            }
            CPLStringList argv = cslFromStrings(argStore);
            GdalOptionsPtr<GDALVectorTranslateOptions> opts(
                GDALVectorTranslateOptionsNew(argv.List(), nullptr));
            if (!opts)
                throw AppException("Cannot create GDAL VectorTranslate options for GPKG");
            cancel::GdalProgress progress; // stops when the build is canceled (DQ-15)
            progress.attach(opts.get());

            CPLErrorReset();
            errno = 0;
            int usageError = 0;
            auto out = adoptDataset(GDALVectorTranslate(
                output.c_str(), nullptr, 1, &hSrcDS, opts.get(), &usageError));
            const int translateSystemError = errno;
            const bool translateFailed = !out || usageError ||
                (translateSystemError == ENOSPC && CPLGetLastErrorType() >= CE_Failure);
            const char *error = CPLGetLastErrorMsg();
            const std::string translateMessage = error && *error ? error : "<no GDAL error>";

            opts.reset();
            const auto closeStatus = closeOutput(std::move(out));

            if (translateFailed)
                cancel::throwIfCanceled();
            if (translateFailed)
                throwVectorWriteFailure("GDAL VectorTranslate (GPKG)", output,
                    translateSystemError == ENOSPC ? translateSystemError :
                        closeStatus.failed ? closeStatus.systemError : translateSystemError,
                    translateMessage);
            if (closeStatus.failed)
                throwVectorWriteFailure("GDAL output close (GPKG)", output,
                                        closeStatus.systemError, closeStatus.message);
        }

        // ---- MVT source sanitization ----------------------------------------
        //
        // GDAL's MVT writer requires layer names to be plain identifiers:
        // letters, digits and underscore, not starting with a digit. Sources
        // such as KMZ commonly carry sublayers with spaces ("Western Theater")
        // which the writer skips with: "ERROR 1: The layer name may not
        // contain special characters or spaces".
        //
        // It also encodes only points, lines and polygons (it splits
        // GeometryCollections itself). Any other type fails tile by tile with
        // "Unsupported geometry type" and the feature is missing from the
        // tiles. GDAL linearizes curves before they reach a driver without
        // curve support, but not inside collections, and polyhedral types are
        // not curves: CAD drawings carry both (a DWG INSERT block holding a
        // hatch becomes a GEOMETRYCOLLECTION with a MULTISURFACE inside).
        //
        // When a source has either problem, its layers are copied into an
        // in-memory GPKG with sanitized names and encodable geometries, and
        // the MVT is translated from that copy.

        bool isValidMvtLayerName(const std::string &name)
        {
            if (name.empty()) return false;
            if (str::isDigitChar(name[0])) return false;
            for (char c : name)
            {
                if (!(str::isAlnumChar(c) || c == '_')) return false;
            }
            return true;
        }

        std::string sanitizeLayerName(const std::string &name)
        {
            std::string out;
            out.reserve(name.size());
            for (char c : name)
            {
                out.push_back((str::isAlnumChar(c) || c == '_') ? c : '_');
            }
            if (out.empty()) out = "layer";
            if (str::isDigitChar(out[0]))
                out.insert(out.begin(), '_');
            return out;
        }

        bool isMvtEncodableType(OGRwkbGeometryType type)
        {
            switch (wkbFlatten(type))
            {
            case wkbPoint:
            case wkbMultiPoint:
            case wkbLineString:
            case wkbMultiLineString:
            case wkbPolygon:
            case wkbMultiPolygon:
                return true;
            default:
                return false;
            }
        }

        // True if the MVT writer can encode the geometry, after splitting the
        // GeometryCollections it splits itself.
        bool isMvtEncodable(const OGRGeometry &geom)
        {
            if (wkbFlatten(geom.getGeometryType()) != wkbGeometryCollection)
                return isMvtEncodableType(geom.getGeometryType());
            const auto *collection = geom.toGeometryCollection();
            return std::all_of(collection->begin(), collection->end(),
                               [](const OGRGeometry *part) { return isMvtEncodable(*part); });
        }

        // Equivalent geometry made of types the MVT writer can encode: curves
        // are linearized and polyhedral surfaces, TINs and triangles become
        // multipolygons, at any depth inside collections.
        std::unique_ptr<OGRGeometry> toMvtEncodable(const OGRGeometry &geom)
        {
            const auto type = wkbFlatten(geom.getGeometryType());
            std::unique_ptr<OGRGeometry> out;
            if (type == wkbGeometryCollection)
            {
                auto collection = std::make_unique<OGRGeometryCollection>();
                for (const OGRGeometry *part : *geom.toGeometryCollection())
                    collection->addGeometryDirectly(toMvtEncodable(*part).release());
                out = std::move(collection);
            }
            else if (type == wkbTriangle || OGR_GT_IsSubClassOf(type, wkbPolyhedralSurface))
                out.reset(OGRGeometryFactory::forceToMultiPolygon(geom.clone()));
            else if (OGR_GT_IsNonLinear(type))
                out.reset(geom.getLinearGeometry());
            else
                out.reset(geom.clone());
            out->assignSpatialReference(geom.getSpatialReference());
            return out;
        }

        // Returns true if any source layer has a name not acceptable by the
        // MVT writer.
        bool anyLayerNeedsSanitization(GDALDatasetH hSrcDS)
        {
            const int n = GDALDatasetGetLayerCount(hSrcDS);
            for (int i = 0; i < n; i++)
            {
                OGRLayerH hLayer = GDALDatasetGetLayer(hSrcDS, i);
                if (!hLayer) continue;
                const char *raw = OGR_L_GetName(hLayer);
                if (!raw || !isValidMvtLayerName(raw)) return true;
            }
            return false;
        }

        // True if a feature of the layer holds a geometry the MVT writer
        // cannot encode. A layer that declares an encodable type is trusted;
        // the others (DXF, mixed GPKG or GeoJSON) are read up to the first hit.
        bool layerNeedsGeometryNormalization(OGRLayerH hLayer)
        {
            OGRLayer *layer = OGRLayer::FromHandle(hLayer);
            const auto declared = wkbFlatten(layer->GetGeomType());
            if (declared == wkbNone || isMvtEncodableType(declared)) return false;

            bool found = false;
            layer->ResetReading();
            while (!found)
            {
                const OGRFeatureUniquePtr feature(layer->GetNextFeature());
                if (!feature) break;
                const OGRGeometry *geom = feature->GetGeometryRef();
                found = geom && !isMvtEncodable(*geom);
            }
            layer->ResetReading();
            return found;
        }

        // Per source layer index: whether its geometries must be normalized.
        std::vector<bool> layersNeedingGeometryNormalization(GDALDatasetH hSrcDS)
        {
            const int n = GDALDatasetGetLayerCount(hSrcDS);
            std::vector<bool> result(static_cast<size_t>(std::max(n, 0)), false);
            for (int i = 0; i < n; i++)
                if (OGRLayerH hLayer = GDALDatasetGetLayer(hSrcDS, i))
                    result[static_cast<size_t>(i)] = layerNeedsGeometryNormalization(hLayer);
            return result;
        }

        // Copies a layer feature by feature into hDst, replacing the geometries
        // the MVT writer cannot encode. Returns the new layer, or nullptr.
        OGRLayerH copyLayerWithEncodableGeometries(GDALDatasetH hDst, OGRLayerH hSrc,
                                                   const std::string &name)
        {
            GDALDataset *dst = GDALDataset::FromHandle(hDst);
            OGRLayer *src = OGRLayer::FromHandle(hSrc);
            OGRFeatureDefn *srcDefn = src->GetLayerDefn();

            OGRLayer *out = dst->CreateLayer(name.c_str(), src->GetSpatialRef(), wkbUnknown, nullptr);
            if (!out) return nullptr;
            for (int i = 0; i < srcDefn->GetFieldCount(); i++)
                if (out->CreateField(srcDefn->GetFieldDefn(i)) != OGRERR_NONE) return nullptr;
            // The fields were created in order; map by index so names the
            // GPKG driver launders still receive their values.
            std::vector<int> fieldMap(static_cast<size_t>(srcDefn->GetFieldCount()));
            std::iota(fieldMap.begin(), fieldMap.end(), 0);

            const bool inTransaction = dst->StartTransaction() == OGRERR_NONE;
            long long skipped = 0;
            src->ResetReading();
            for (OGRFeatureUniquePtr feature(src->GetNextFeature()); feature;
                 feature.reset(src->GetNextFeature()))
            {
                OGRFeature copy(out->GetLayerDefn());
                bool copied = copy.SetFieldsFrom(feature.get(), fieldMap.data()) == OGRERR_NONE;
                if (const OGRGeometry *geom = feature->GetGeometryRef())
                    copied = copied && copy.SetGeometryDirectly(isMvtEncodable(*geom)
                                                                    ? geom->clone()
                                                                    : toMvtEncodable(*geom).release()) == OGRERR_NONE;
                if (!copied || out->CreateFeature(&copy) != OGRERR_NONE) skipped++;
            }
            src->ResetReading();
            // The features of the transaction are only in the layer once it is committed
            if (inTransaction && dst->CommitTransaction() != OGRERR_NONE)
                throw GDALException("Cannot commit the sanitized layer '" + name + "': " + CPLGetLastErrorMsg());

            if (skipped > 0)
                LOGD << "MVT sanitization: " << skipped << " features of layer '" << name
                     << "' could not be copied and will be missing from the MVT output";
            return OGRLayer::ToHandle(out);
        }

        // Build a GPKG in /vsimem/ that copies each source layer with a
        // sanitized, unique name, normalizing the geometries of the layers
        // flagged in normalizeGeometries. Layers that need no geometry work go
        // through GDALDatasetCopyLayer (per-index lookup), so it works even
        // when the source has duplicate layer names (e.g. KMZ folders sharing
        // a <name>).
        //
        // Returns the /vsimem/ file of the GPKG.
        VsiMemFile buildSanitizedGpkg(GDALDatasetH hSrcDS,
                                       const std::vector<bool> &normalizeGeometries)
        {
            GDALDriverH hGpkg = GDALGetDriverByName("GPKG");
            if (!hGpkg)
                throw AppException(
                    "GPKG driver not available; cannot sanitize the MVT source");

            VsiMemFile gpkg = VsiMemFile::unique("ddb_mvt_", ".gpkg");

            auto dst = adoptDataset(GDALCreate(hGpkg, gpkg.c_str(),
                                               0, 0, 0, GDT_Unknown, nullptr));
            if (!dst)
                throw AppException(
                    "Cannot create in-memory GPKG for MVT source sanitization");
            GDALDatasetH hDst = datasetHandle(dst);

            std::set<std::string> used;
            const int n = GDALDatasetGetLayerCount(hSrcDS);
            int copied = 0;
            for (int i = 0; i < n; i++)
            {
                OGRLayerH hLayer = GDALDatasetGetLayer(hSrcDS, i);
                if (!hLayer) continue;
                const char *raw = OGR_L_GetName(hLayer);
                const std::string srcLayer = raw ? std::string(raw) : "";

                std::string sane = sanitizeLayerName(srcLayer);
                std::string candidate = sane;
                int suffix = 2;
                while (used.count(candidate) > 0)
                    candidate = sane + "_" + std::to_string(suffix++);
                used.insert(candidate);

                const bool normalize = static_cast<size_t>(i) < normalizeGeometries.size() &&
                                       normalizeGeometries[static_cast<size_t>(i)];
                OGRLayerH hCopied = normalize
                    ? copyLayerWithEncodableGeometries(hDst, hLayer, candidate)
                    : GDALDatasetCopyLayer(hDst, hLayer, candidate.c_str(), nullptr);
                if (hCopied) {
                    copied++;
                } else {
                    // Don't fail the whole build for one bad layer, but make
                    // the omission auditable: the resulting MVT will not
                    // contain features from this source layer.
                    const char *err = CPLGetLastErrorMsg();
                    LOGD << "MVT sanitization: copying failed for "
                         << "source layer index " << i
                         << " (name='" << srcLayer
                         << "', sanitized='" << candidate << "'): "
                         << (err && *err ? err : "<no GDAL error>")
                         << " - this layer will be missing from the MVT output";
                }
            }

            dst.reset();
            LOGD << "MVT sanitization: copied " << copied << "/" << n
                 << " layers into " << gpkg.path();
            return gpkg;
        }

        // The dynamic MAXZOOM of the MVT: the zoom the feature count over the area of the source calls
        // for (logged)
        int logMvtMaxZoom(const VectorStats &stats)
        {
            double areaDeg2 = 0.0;
            if (stats.haveEnv)
            {
                areaDeg2 = std::max(0.0, stats.maxX - stats.minX) *
                           std::max(0.0, stats.maxY - stats.minY);
            }
            const int maxZoom = computeMvtMaxZoom(stats.featureCount, areaDeg2);
            LOGD << "MVT MAXZOOM=" << maxZoom
                 << " (areaDeg2=" << areaDeg2
                 << ", features=" << stats.featureCount << ")";
            return maxZoom;
        }

        // The ogr2ogr arguments of the MVT directory: Web Mercator, the zoom range and tile limits,
        // and the source SRS when no layer has one
        std::vector<std::string> mvtTranslateArgs(const std::string &maxZoomArg, bool needsSrs,
                                                  const std::string &sourceSrs)
        {
            // NOTE: -dsco takes a single KEY=VALUE token.
            // Valid MVT dsco keys: FORMAT, MINZOOM, MAXZOOM, EXTENT, BUFFER,
            // COMPRESS, MAX_SIZE, MAX_FEATURES, TILE_EXTENSION, CONF, etc.
            std::vector<std::string> argStore = {
                "-f",      "MVT",
                "-t_srs",  "EPSG:3857",
                "-dsco",   "FORMAT=DIRECTORY",
                "-dsco",   "MINZOOM=0",
                "-dsco",   maxZoomArg,
                "-dsco",   "EXTENT=4096",
                "-dsco",   "BUFFER=80",
                "-dsco",   "MAX_SIZE=500000",
                "-dsco",   "MAX_FEATURES=200000",
                "-dsco",   "COMPRESS=YES",
                "-mapFieldType", "DateTime=String",
                "-skipfailures"};
            if (needsSrs) {
                argStore.push_back("-s_srs");
                argStore.push_back(sourceSrs);
            }
            return argStore;
        }

        // Convert input to MVT directory, reprojected to EPSG:3857.
        // Returns the dynamic MAXZOOM used.
        int convertToMvt(GDALDatasetH hSrcDS, const std::string &srcPath,
                         const std::string &outputDir, const VectorStats &stats,
                         const std::string &sourceSrs)
        {
            // Sanity check driver availability up-front. A missing driver can never
            // be provided by adding files, so the build fails instead of deferring.
            if (GDALGetDriverByName("MVT") == nullptr)
                throw AppException("MVT driver not available in this GDAL build");

            const int maxZoom = logMvtMaxZoom(stats);
            const std::string maxZoomArg = "MAXZOOM=" + std::to_string(maxZoom);

            // If the MVT writer would reject a layer name or a geometry, copy
            // the layers into an in-memory GPKG with sanitized, unique names
            // and encodable geometries, and translate from that instead.
            GDALDatasetH hTranslateSrc = hSrcDS;
            // The file outlives the dataset that reads it
            VsiMemFile sanitized;
            GDALDatasetUniquePtr sanitizedDS;
            const auto normalizeGeometries = layersNeedingGeometryNormalization(hSrcDS);
            const bool anyGeometryToNormalize =
                std::find(normalizeGeometries.begin(), normalizeGeometries.end(), true) !=
                normalizeGeometries.end();
            if (anyGeometryToNormalize || anyLayerNeedsSanitization(hSrcDS))
            {
                sanitized = buildSanitizedGpkg(hSrcDS, normalizeGeometries);
                sanitizedDS = adoptDataset(GDALOpenEx(sanitized.c_str(),
                                                      GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                                      nullptr, nullptr, nullptr));
                if (!sanitizedDS)
                    throw AppException(
                        "Cannot open sanitized GPKG for MVT translation: " +
                        sanitized.path());
                hTranslateSrc = datasetHandle(sanitizedDS);
                LOGD << "MVT: using sanitized GPKG (" << sanitized.path()
                     << ") for " << srcPath;
            }

            const bool needsSrs = allLayersMissSrs(hTranslateSrc);

            const std::vector<std::string> argStore = mvtTranslateArgs(maxZoomArg, needsSrs, sourceSrs);
            CPLStringList argv = cslFromStrings(argStore);
            GdalOptionsPtr<GDALVectorTranslateOptions> opts(
                GDALVectorTranslateOptionsNew(argv.List(), nullptr));
            if (!opts)
                throw AppException("Cannot create GDAL VectorTranslate options for MVT");
            cancel::GdalProgress progress; // stops when the build is canceled (DQ-15)
            progress.attach(opts.get());

            // Reset GDAL error state so CPLGetLastErrorMsg() reflects this op.
            CPLErrorReset();

            int usageError = 0;
            errno = 0;
            auto out = adoptDataset(GDALVectorTranslate(
                outputDir.c_str(), nullptr, 1, &hTranslateSrc, opts.get(), &usageError));
            const int translateSystemError = errno;
            const bool translateFailed = !out || usageError ||
                (translateSystemError == ENOSPC && CPLGetLastErrorType() >= CE_Failure);
            const char *error = CPLGetLastErrorMsg();
            const std::string translateMessage = error && *error ? error : "<no GDAL error>";

            opts.reset();
            const auto closeStatus = closeOutput(std::move(out));

            // Release sanitization resources whether or not translate succeeded.
            sanitizedDS.reset();
            sanitized.reset();

            if (translateFailed)
                cancel::throwIfCanceled();
            if (translateFailed)
                throwVectorWriteFailure("GDAL VectorTranslate (MVT)", outputDir,
                    translateSystemError == ENOSPC ? translateSystemError :
                        closeStatus.failed ? closeStatus.systemError : translateSystemError,
                    translateMessage);
            if (closeStatus.failed)
                throwVectorWriteFailure("GDAL output close (MVT)", outputDir,
                                        closeStatus.systemError, closeStatus.message);
            return maxZoom;
        }

    } // anonymous namespace

    // ---- public API ----------------------------------------------------------

    std::vector<std::string> getVectorDependencies(const std::string &input)
    {
        std::vector<std::string> deps;

        if (!fs::exists(io::fromUtf8(input)))
            throw FSException(input + " does not exist");

        const auto ext = io::lowerExtension(io::fromUtf8(input));
        if (ext == "shp")
        {
            deps.push_back(io::toUtf8(io::fromUtf8(input).replace_extension(".shx").filename()));
        }
        else if (ext == "shx")
        {
            deps.push_back(io::toUtf8(io::fromUtf8(input).replace_extension(".shp").filename()));
        }
        return deps;
    }

    namespace
    {

        // The CRS of the source layers that have none, and whether the drawing is local
        struct VectorSourceCrs
        {
            std::string sourceSrs = "EPSG:4326"; // layers without an SRS: EPSG:4326 for non-CAD inputs (historical behaviour)
            bool local = false;                  // a CAD drawing without a CRS: nothing to place on a map
            cad::CadCrs crs;
            std::string units;
        };

        // The drawing's CRS for CAD (nothing for a local drawing), EPSG:4326 for the rest
        VectorSourceCrs resolveSourceCrs(const std::string &input, const std::string &openPath, bool isCad,
                                         GDALDatasetH hSrcDS)
        {
            VectorSourceCrs out;
            if (isCad)
            {
                const auto info = cad::readDxfInfo(io::fromUtf8(openPath));
                out.crs = cad::resolveCadCrs(io::fromUtf8(input), hSrcDS, info);
                out.units = cad::insUnitsName(info.insUnits);
                out.local = !out.crs.georeferenced;
                if (!out.crs.srs.empty()) out.sourceSrs = out.crs.srs;
                LOGD << "CAD CRS for " << input << ": source=" << out.crs.source
                     << " label=" << out.crs.label << (out.local ? " (local)" : "");
            }
            return out;
        }

        // Writes the outputs into the staging folders: a GeoPackage, and the MVT pyramid (or, for a
        // local drawing, the local.json that says it has none)
        void stageOutputs(GDALDatasetH hSrcDS, const std::string &input, const VectorSourceCrs &source,
                          const fs::path &stagingVec, const fs::path &stagingMvt)
        {
            const auto gpkgOut = io::toUtf8(stagingVec / VectorGpkgFileName);
            if (source.local)
            {
                // Drawing coordinates, no SRS and no MVT pyramid: there is
                // nothing to place on a map (the Hub shows it without a globe).
                convertToGpkg(hSrcDS, gpkgOut, "", false);
                json localInfo = {{"georeferenced", false}, {"units", source.units}};
                if (source.crs.hasExtent)
                    localInfo["extent"] = {source.crs.extent.MinX, source.crs.extent.MinY,
                                           source.crs.extent.MaxX, source.crs.extent.MaxY};
                std::ofstream out(stagingVec / VectorLocalFileName, std::ios::binary);
                out << localInfo.dump();
                if (!out)
                    throw FSException("Cannot write " + io::toUtf8(stagingVec / VectorLocalFileName));
            }
            else
            {
                const VectorStats stats = computeStats(hSrcDS, source.sourceSrs);
                LOGD << "Vector stats: features=" << stats.featureCount
                     << " envelope=[" << stats.minX << "," << stats.minY << ","
                     << stats.maxX << "," << stats.maxY << "]";
                // Branch A: MVT first (cheaper to roll back).
                convertToMvt(hSrcDS, input, io::toUtf8(stagingMvt), stats, source.sourceSrs);
                // Branch B: GPKG sidecar.
                convertToGpkg(hSrcDS, gpkgOut, source.sourceSrs, true);
            }
        }

        // Publish: move existing artifacts (if any) to a sibling backup
        // dir, promote the staged outputs, and on failure restore from
        // the backup so users never observe a half-published state. A
        // local build publishes no mvt/, so an old pyramid is retired.
        void publishStaged(const fs::path &basePath, const fs::path &stagingDir, bool local)
        {
            const fs::path vecDir = basePath / VectorBuildSubfolder;
            const fs::path mvtDir = basePath / MvtBuildSubfolder;
            const fs::path stagingVec = stagingDir / VectorBuildSubfolder;
            const fs::path stagingMvt = stagingDir / MvtBuildSubfolder;
            const fs::path backupDir = basePath /
                (VectorBackupPrefix + randomString(16));
            const fs::path backupVec = backupDir / VectorBuildSubfolder;
            const fs::path backupMvt = backupDir / MvtBuildSubfolder;
            // Back up any existing directory at the publish target, even if
            // it lacks the canonical artifact (e.g. an empty leftover from a
            // previous aborted build): otherwise the promote-rename below
            // would fail because the destination already exists.
            // A GeoPackage of the previous build kept open for WFS/OGC queries would
            // block the renames on Windows
            RasterCache::instance().release(vecDir);
            RasterCache::instance().release(mvtDir);
            const bool hadVec = fs::exists(vecDir);
            const bool hadMvt = fs::exists(mvtDir);
            io::assureFolderExists(backupDir);
            try
            {
                if (hadVec) io::rename(vecDir, backupVec);
                if (hadMvt) io::rename(mvtDir, backupMvt);
                io::rename(stagingVec, vecDir);
                if (!local) io::rename(stagingMvt, mvtDir);
            }
            catch (...)
            {
                // Rollback: remove anything we already promoted, then
                // restore the originals from the backup.
                io::assureIsRemoved(vecDir);
                io::assureIsRemoved(mvtDir);
                if (hadVec && fs::exists(backupVec))
                    io::rename(backupVec, vecDir);
                if (hadMvt && fs::exists(backupMvt))
                    io::rename(backupMvt, mvtDir);
                io::assureIsRemoved(backupDir);
                throw;
            }

            io::assureIsRemoved(stagingDir);
            io::assureIsRemoved(backupDir);
        }

    } // anonymous namespace

    void buildVector(const std::string &input,
                     const std::string &baseOutputPath,
                     bool overwrite)
    {
        LOGD << "buildVector(" << input << " -> " << baseOutputPath
             << ", overwrite=" << (overwrite ? "true" : "false") << ")";

        if (input.empty())
            throw InvalidArgsException("buildVector: input is empty");
        if (baseOutputPath.empty())
            throw InvalidArgsException("buildVector: baseOutputPath is empty");
        if (!fs::exists(io::fromUtf8(input)))
            throw FSException(input + " does not exist");

        // Step 1: verify sidecar deps (.shx for .shp)
        checkDependencies(input);

        const fs::path basePath = io::fromUtf8(baseOutputPath);
        io::assureFolderExists(basePath);

        // Step 2: CAD drawings: a DWG is converted to a temporary DXF (staged
        // next to the outputs), and the CRS comes from the drawing (cad.h).
        const bool isCad = cad::isCadFile(io::fromUtf8(input));
        std::optional<cad::CadSource> cadSource;
        if (isCad)
            cadSource.emplace(io::fromUtf8(input), nullptr, basePath);
        const std::string openPath = isCad ? io::toUtf8(cadSource->dxfPath()) : input;

        // Step 3: open source once, reuse for both branches
        const auto srcDS = adoptDataset(GDALOpenEx(openPath.c_str(),
                                                   GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                                   nullptr, nullptr, nullptr));
        if (!srcDS)
            throw AppException("Cannot open vector source: " + input);
        GDALDatasetH hSrcDS = datasetHandle(srcDS);

        // Layers without an SRS: EPSG:4326 for non-CAD inputs (historical
        // behaviour), the drawing's CRS for CAD, nothing for a local drawing.
        const VectorSourceCrs source = resolveSourceCrs(input, openPath, isCad, hSrcDS);

        const fs::path vecDir = basePath / VectorBuildSubfolder;
        const fs::path mvtDir = basePath / MvtBuildSubfolder;

        // A previous aborted build can leave empty vec/ or mvt/ directories
        // behind. Check for the actual artifacts inside (source.gpkg and
        // the MVT tileset metadata.json, or local.json for a local drawing)
        // so we don't silently skip and produce a broken state.
        const bool vecExists = fs::exists(vecDir / VectorGpkgFileName);
        const bool upToDate = source.local ? fs::exists(vecDir / VectorLocalFileName)
                                           : fs::exists(mvtDir / MvtMetadataFileName) &&
                                                 !fs::exists(vecDir / VectorLocalFileName);

        if (vecExists && upToDate && !overwrite)
        {
            LOGD << "Vector artifacts already exist, skipping";
            return;
        }

        // Stage both outputs under a single sibling directory so that a
        // crash partway through leaves at most one orphan directory to
        // clean up. The publish step then promotes the children with a
        // best-effort rollback if a rename fails.
        // Removed on any throw from here on.
        const io::ScopedTempDir staging = io::ScopedTempDir::create(basePath, VectorStagingPrefix);
        const fs::path &stagingDir = staging.path();
        const fs::path stagingVec = stagingDir / VectorBuildSubfolder;
        const fs::path stagingMvt = stagingDir / MvtBuildSubfolder;
        io::assureFolderExists(stagingVec);
        // MVT driver creates its own output dir; ensure parent exists
        // and the target child does not (pre-empt leftovers).
        io::assureIsRemoved(stagingMvt);

        stageOutputs(hSrcDS, input, source, stagingVec, stagingMvt);
        publishStaged(basePath, stagingDir, source.local);
    }

} // namespace ddb
