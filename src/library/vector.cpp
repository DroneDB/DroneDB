/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <algorithm>
#include <cctype>
#include <limits>
#include <set>
#include <vector>

#include "gdal_inc.h"
#include "vector.h"
#include "mvt.h"
#include "logger.h"
#include "mio.h"
#include "exceptions.h"
#include "utils.h"

namespace ddb
{

    namespace
    {

        // ---- helpers ---------------------------------------------------------

        std::string toLowerExt(const std::string &input)
        {
            auto ext = fs::path(input).extension().string();
            ddb::utils::toLower(ext);
            return ext;
        }

        // Compute total feature count and union envelope (in WGS84) for MAXZOOM.
        struct VectorStats
        {
            long long featureCount = 0;
            // WGS84 envelope (degrees)
            double minX =  std::numeric_limits<double>::max();
            double minY =  std::numeric_limits<double>::max();
            double maxX = -std::numeric_limits<double>::max();
            double maxY = -std::numeric_limits<double>::max();
            bool haveEnv = false;
        };

        VectorStats computeStats(GDALDatasetH hSrcDS)
        {
            VectorStats s;

            OGRSpatialReferenceH hWgs84 = OSRNewSpatialReference(nullptr);
            if (OSRImportFromEPSG(hWgs84, 4326) != OGRERR_NONE)
            {
                OSRDestroySpatialReference(hWgs84);
                hWgs84 = nullptr;
            }
            else
            {
                OSRSetAxisMappingStrategy(hWgs84, OAMS_TRADITIONAL_GIS_ORDER);
            }

            const int layerCount = GDALDatasetGetLayerCount(hSrcDS);
            for (int i = 0; i < layerCount; i++)
            {
                OGRLayerH hLayer = GDALDatasetGetLayer(hSrcDS, i);
                if (!hLayer) continue;

                s.featureCount += static_cast<long long>(OGR_L_GetFeatureCount(hLayer, FALSE));

                OGREnvelope env;
                if (OGR_L_GetExtent(hLayer, &env, TRUE) != OGRERR_NONE) continue;

                OGRSpatialReferenceH hSrs = OGR_L_GetSpatialRef(hLayer);
                double xs[4] = {env.MinX, env.MaxX, env.MaxX, env.MinX};
                double ys[4] = {env.MinY, env.MinY, env.MaxY, env.MaxY};

                if (hSrs && hWgs84)
                {
                    OSRSetAxisMappingStrategy(hSrs, OAMS_TRADITIONAL_GIS_ORDER);
                    const char *authCode = OSRGetAuthorityCode(hSrs, nullptr);
                    const char *authName = OSRGetAuthorityName(hSrs, nullptr);
                    bool isWgs84 = (authName && authCode &&
                                    std::string(authName) == "EPSG" &&
                                    std::string(authCode) == "4326");
                    if (!isWgs84)
                    {
                        OGRCoordinateTransformationH hT =
                            OCTNewCoordinateTransformation(hSrs, hWgs84);
                        if (hT)
                        {
                            if (!OCTTransform(hT, 4, xs, ys, nullptr))
                            {
                                LOGD << "computeStats: layer " << i << " transform failed";
                                OCTDestroyCoordinateTransformation(hT);
                                continue;
                            }
                            OCTDestroyCoordinateTransformation(hT);
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

            if (hWgs84) OSRDestroySpatialReference(hWgs84);
            return s;
        }

        // Returns true if all source layers lack an SRS (typical for DXF).
        // In that case, callers must inject -s_srs EPSG:4326 (we already
        // computed the envelope assuming WGS84 in computeStats).
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
            const fs::path p(input);
            for (const auto &d : deps)
            {
                if (!fs::exists(p.parent_path() / d))
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

        // Convert input to GPKG with SPATIAL_INDEX=YES, reprojected to EPSG:4326.
        // Multi-layer sources are preserved verbatim.
        void convertToGpkg(GDALDatasetH hSrcDS, const std::string &output)
        {
            const bool needsSrs = allLayersMissSrs(hSrcDS);

            std::vector<const char *> args;
            args.insert(args.end(), {
                "-f",     "GPKG",
                "-t_srs", "EPSG:4326",
                "-lco",   "SPATIAL_INDEX=YES",
                "-lco",   "GEOMETRY_NAME=geom",
                "-lco",   "FID=fid",
                "-nlt",   "PROMOTE_TO_MULTI",
                // KML/KMZ have DateTime fields that several drivers warn on;
                // map to String for maximum compatibility.
                "-mapFieldType", "DateTime=String",
                "-skipfailures"});
            if (needsSrs) {
                args.push_back("-s_srs");
                args.push_back("EPSG:4326");
            }
            args.push_back(nullptr);

            char **argv = const_cast<char **>(args.data());
            GDALVectorTranslateOptions *opts =
                GDALVectorTranslateOptionsNew(argv, nullptr);
            if (!opts)
                throw AppException("Cannot create GDAL VectorTranslate options for GPKG");

            CPLErrorReset();
            int usageError = 0;
            GDALDatasetH hOut = GDALVectorTranslate(
                output.c_str(), nullptr, 1, &hSrcDS, opts, &usageError);

            GDALVectorTranslateOptionsFree(opts);

            if (!hOut || usageError)
            {
                const char *err = CPLGetLastErrorMsg();
                const std::string gdalErr = (err && *err) ? std::string(err) : "<no GDAL error>";
                if (hOut) GDALClose(hOut);
                throw AppException("GDAL VectorTranslate (GPKG) failed for " +
                                   output + ": " + gdalErr);
            }
            GDALClose(hOut);
        }

        // ---- MVT layer-name sanitization ------------------------------------
        //
        // GDAL's MVT writer requires layer names to be plain identifiers:
        // letters, digits and underscore, not starting with a digit. Sources
        // such as KMZ commonly carry sublayers with spaces ("Western Theater")
        // which the writer skips with: "ERROR 1: The layer name may not
        // contain special characters or spaces". To preserve every layer we
        // expose a renamed, read-only view via an in-memory OGR VRT.

        bool isValidMvtLayerName(const std::string &name)
        {
            if (name.empty()) return false;
            if (std::isdigit(static_cast<unsigned char>(name[0]))) return false;
            for (char c : name)
            {
                const unsigned char uc = static_cast<unsigned char>(c);
                if (!(std::isalnum(uc) || c == '_')) return false;
            }
            return true;
        }

        std::string sanitizeLayerName(const std::string &name)
        {
            std::string out;
            out.reserve(name.size());
            for (char c : name)
            {
                const unsigned char uc = static_cast<unsigned char>(c);
                out.push_back((std::isalnum(uc) || c == '_') ? c : '_');
            }
            if (out.empty()) out = "layer";
            if (std::isdigit(static_cast<unsigned char>(out[0])))
                out.insert(out.begin(), '_');
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

        // Build a GPKG in /vsimem/ that copies each source layer with a
        // sanitized, unique name. Uses GDALDatasetCopyLayer (per-index lookup)
        // so it works even when the source has duplicate layer names
        // (e.g. KMZ folders sharing a <name>).
        //
        // Returns the /vsimem/ path of the GPKG (caller owns: must VSIUnlink).
        std::string buildSanitizedGpkg(GDALDatasetH hSrcDS)
        {
            GDALDriverH hGpkg = GDALGetDriverByName("GPKG");
            if (!hGpkg)
                throw AppException(
                    "GPKG driver not available; cannot sanitize MVT layer names");

            const std::string vsiPath =
                "/vsimem/ddb_mvt_" + utils::generateRandomString(16) + ".gpkg";

            GDALDatasetH hDst = GDALCreate(hGpkg, vsiPath.c_str(),
                                            0, 0, 0, GDT_Unknown, nullptr);
            if (!hDst)
                throw AppException(
                    "Cannot create in-memory GPKG for MVT layer sanitization");

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

                OGRLayerH hCopied = GDALDatasetCopyLayer(
                    hDst, hLayer, candidate.c_str(), nullptr);
                if (hCopied) copied++;
            }

            GDALClose(hDst);
            LOGD << "MVT sanitization: copied " << copied << "/" << n
                 << " layers into " << vsiPath;
            return vsiPath;
        }

        // Convert input to MVT directory, reprojected to EPSG:3857.
        // Returns the dynamic MAXZOOM used.
        int convertToMvt(GDALDatasetH hSrcDS, const std::string &srcPath,
                         const std::string &outputDir, const VectorStats &stats)
        {
            // Sanity check driver availability up-front.
            if (GDALGetDriverByName("MVT") == nullptr)
            {
                std::vector<std::string> missing{"MVT driver"};
                throw BuildDepMissingException(
                    "MVT driver not available in this GDAL build", missing);
            }

            double areaDeg2 = 0.0;
            if (stats.haveEnv)
            {
                areaDeg2 = std::max(0.0, stats.maxX - stats.minX) *
                           std::max(0.0, stats.maxY - stats.minY);
            }
            const int maxZoom = computeMvtMaxZoom(stats.featureCount, areaDeg2);
            const std::string maxZoomArg = "MAXZOOM=" + std::to_string(maxZoom);
            LOGD << "MVT MAXZOOM=" << maxZoom
                 << " (areaDeg2=" << areaDeg2
                 << ", features=" << stats.featureCount << ")";

            // If any source layer name is rejected by the MVT writer, copy
            // the layers into an in-memory GPKG with sanitized, unique names
            // and translate from that instead. CopyLayer is index-based so
            // it survives duplicate source layer names (e.g. KMZ folders).
            GDALDatasetH hTranslateSrc = hSrcDS;
            std::string sanitizedPath;
            GDALDatasetH hSanitizedDS = nullptr;
            if (anyLayerNeedsSanitization(hSrcDS))
            {
                sanitizedPath = buildSanitizedGpkg(hSrcDS);
                hSanitizedDS = GDALOpenEx(sanitizedPath.c_str(),
                                          GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                          nullptr, nullptr, nullptr);
                if (!hSanitizedDS)
                {
                    VSIUnlink(sanitizedPath.c_str());
                    throw AppException(
                        "Cannot open sanitized GPKG for MVT translation: " +
                        sanitizedPath);
                }
                hTranslateSrc = hSanitizedDS;
                LOGD << "MVT: using sanitized GPKG (" << sanitizedPath
                     << ") for " << srcPath;
            }

            const bool needsSrs = allLayersMissSrs(hTranslateSrc);

            // NOTE: -dsco takes a single KEY=VALUE token.
            // Valid MVT dsco keys: FORMAT, MINZOOM, MAXZOOM, EXTENT, BUFFER,
            // COMPRESS, MAX_SIZE, MAX_FEATURES, TILE_EXTENSION, CONF, etc.
            std::vector<const char *> args;
            args.insert(args.end(), {
                "-f",      "MVT",
                "-t_srs",  "EPSG:3857",
                "-dsco",   "FORMAT=DIRECTORY",
                "-dsco",   "MINZOOM=0",
                "-dsco",   maxZoomArg.c_str(),
                "-dsco",   "EXTENT=4096",
                "-dsco",   "BUFFER=80",
                "-dsco",   "MAX_SIZE=500000",
                "-dsco",   "MAX_FEATURES=200000",
                "-dsco",   "COMPRESS=YES",
                "-mapFieldType", "DateTime=String",
                "-skipfailures"});
            if (needsSrs) {
                args.push_back("-s_srs");
                args.push_back("EPSG:4326");
            }
            args.push_back(nullptr);

            char **argv = const_cast<char **>(args.data());
            GDALVectorTranslateOptions *opts =
                GDALVectorTranslateOptionsNew(argv, nullptr);
            if (!opts)
            {
                if (hSanitizedDS) GDALClose(hSanitizedDS);
                if (!sanitizedPath.empty()) VSIUnlink(sanitizedPath.c_str());
                throw AppException("Cannot create GDAL VectorTranslate options for MVT");
            }

            // Reset GDAL error state so CPLGetLastErrorMsg() reflects this op.
            CPLErrorReset();

            int usageError = 0;
            GDALDatasetH hOut = GDALVectorTranslate(
                outputDir.c_str(), nullptr, 1, &hTranslateSrc, opts, &usageError);

            GDALVectorTranslateOptionsFree(opts);

            // Release sanitization resources whether or not translate succeeded.
            if (hSanitizedDS) GDALClose(hSanitizedDS);
            if (!sanitizedPath.empty()) VSIUnlink(sanitizedPath.c_str());

            if (!hOut || usageError)
            {
                const char *err = CPLGetLastErrorMsg();
                const std::string gdalErr = (err && *err) ? std::string(err) : "<no GDAL error>";
                if (hOut) GDALClose(hOut);
                throw AppException("GDAL VectorTranslate (MVT) failed for " +
                                   outputDir + ": " + gdalErr);
            }
            GDALClose(hOut);
            return maxZoom;
        }

        // Generate a sibling temp folder for staged atomic writes.
        fs::path makeTempSibling(const fs::path &finalDir)
        {
            return finalDir.string() + "-temp-" + utils::generateRandomString(16);
        }

    } // anonymous namespace

    // ---- public API ----------------------------------------------------------

    std::vector<std::string> getVectorDependencies(const std::string &input)
    {
        std::vector<std::string> deps;

        if (!fs::exists(input))
            throw FSException(input + " does not exist");

        const auto ext = toLowerExt(input);
        if (ext == ".shp")
        {
            deps.push_back(fs::path(input).replace_extension(".shx").filename().string());
        }
        else if (ext == ".shx")
        {
            deps.push_back(fs::path(input).replace_extension(".shp").filename().string());
        }
        return deps;
    }

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
        if (!fs::exists(input))
            throw FSException(input + " does not exist");

        // Step 1: verify sidecar deps (.shx for .shp)
        checkDependencies(input);

        // Step 2: open source once, reuse for both branches
        GDALDatasetH hSrcDS = GDALOpenEx(input.c_str(),
                                         GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                         nullptr, nullptr, nullptr);
        if (!hSrcDS)
            throw AppException("Cannot open vector source: " + input);

        try
        {
            const VectorStats stats = computeStats(hSrcDS);
            LOGD << "Vector stats: features=" << stats.featureCount
                 << " envelope=[" << stats.minX << "," << stats.minY << ","
                 << stats.maxX << "," << stats.maxY << "]";

            const fs::path basePath(baseOutputPath);
            io::assureFolderExists(basePath);

            const fs::path vecDir = basePath / "vec";
            const fs::path mvtDir = basePath / "mvt";

            const bool vecExists = fs::exists(vecDir);
            const bool mvtExists = fs::exists(mvtDir);

            // Skip if both already published and not overwriting.
            if (vecExists && mvtExists && !overwrite)
            {
                LOGD << "Vector artifacts already exist, skipping (vec/ and mvt/)";
                return;
            }

            const fs::path vecTemp = makeTempSibling(vecDir);
            const fs::path mvtTemp = makeTempSibling(mvtDir);
            io::assureFolderExists(vecTemp);
            // Note: MVT driver creates its own output directory; ensure parent.
            io::assureFolderExists(mvtTemp.parent_path());
            // Pre-remove mvtTemp if it leaked from a previous crash run.
            io::assureIsRemoved(mvtTemp);

            try
            {
                // Branch A: MVT first (cheaper to roll back).
                convertToMvt(hSrcDS, input, mvtTemp.string(), stats);
                // Branch B: GPKG sidecar.
                const auto gpkgOut = (vecTemp / "source.gpkg").string();
                convertToGpkg(hSrcDS, gpkgOut);
            }
            catch (...)
            {
                io::assureIsRemoved(vecTemp);
                io::assureIsRemoved(mvtTemp);
                throw;
            }

            // Both temps ready - publish atomically.
            if (vecExists) io::assureIsRemoved(vecDir);
            if (mvtExists) io::assureIsRemoved(mvtDir);
            io::rename(vecTemp.string(), vecDir.string());
            io::rename(mvtTemp.string(), mvtDir.string());
        }
        catch (...)
        {
            GDALClose(hSrcDS);
            throw;
        }

        GDALClose(hSrcDS);
    }

} // namespace ddb
