/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "gdal_inc.h"
#include "gdal_priv.h"
#include "gdal_utils.h"
#include "cpl_conv.h"
#include "cpl_vsi.h"
#include "ogr_spatialref.h"

#include "raster_region.h"
#include "exceptions.h"
#include "logger.h"
#include "utils.h"
#include "json.h"

namespace ddb
{

    namespace
    {

        // Map MIME type to GDAL driver name + sensible defaults.
        struct FormatInfo
        {
            const char *driver;
            const char *vsiExt;
            bool wantsAlpha;       // append -dstalpha
            bool jpegCompositing;  // if true, composite on white before write
        };

        FormatInfo resolveFormat(const std::string &mime)
        {
            const std::string m = mime.empty() ? "image/png" : mime;
            if (m == "image/png"  || m == "png")         return {"PNG",  "png",  true,  false};
            if (m == "image/jpeg" || m == "jpeg" || m == "jpg" || m == "image/jpg")
                                                          return {"JPEG", "jpg",  false, true};
            if (m == "image/webp" || m == "webp")        return {"WEBP", "webp", true,  false};
            if (m == "image/tiff" || m == "image/geotiff" || m == "tiff" || m == "tif")
                                                          return {"GTiff", "tif",  true,  false};
            throw InvalidArgsException("Unsupported format: " + m);
        }

        std::string fmtDouble(double v) {
            std::ostringstream os;
            os << std::setprecision(17) << v;
            return os.str();
        }

        void validateInputs(const std::string &inputPath, const double bbox[4],
                            int width, int height)
        {
            if (inputPath.empty())
                throw InvalidArgsException("renderRasterRegion: input path is empty");
            if (bbox == nullptr)
                throw InvalidArgsException("renderRasterRegion: bbox is null");
            if (width  <= 0 || width  > 4096)
                throw InvalidArgsException("renderRasterRegion: width out of range [1,4096]");
            if (height <= 0 || height > 4096)
                throw InvalidArgsException("renderRasterRegion: height out of range [1,4096]");
            if (bbox[0] >= bbox[2] || bbox[1] >= bbox[3])
                throw InvalidArgsException("renderRasterRegion: invalid bbox order");
        }

    } // anonymous namespace

    // ---------------------------------------------------------------------

    void renderRasterRegion(const std::string &inputPath,
                            const double bbox[4],
                            const std::string &bboxSrs,
                            int width, int height,
                            const std::string &format,
                            uint8_t **outBytes,
                            int *outSize)
    {
        validateInputs(inputPath, bbox, width, height);
        if (!outBytes || !outSize)
            throw InvalidArgsException("renderRasterRegion: outBytes/outSize null");

        const FormatInfo fi = resolveFormat(format);
        const std::string srs = bboxSrs.empty() ? std::string("EPSG:4326") : bboxSrs;

        GDALDatasetH hSrc = GDALOpenEx(inputPath.c_str(),
                                      GDAL_OF_RASTER | GDAL_OF_READONLY,
                                      nullptr, nullptr, nullptr);
        if (!hSrc)
            throw GDALException("Cannot open raster: " + inputPath);

        const std::string vsiPath =
            "/vsimem/ddb-render-" + utils::generateRandomString(16) +
            "." + fi.vsiExt;

        // Build GDALWarp options
        std::vector<std::string> argStore;
        argStore.emplace_back("-of");       argStore.emplace_back(fi.driver);
        argStore.emplace_back("-t_srs");    argStore.emplace_back(srs);
        argStore.emplace_back("-te");
        argStore.emplace_back(fmtDouble(bbox[0]));
        argStore.emplace_back(fmtDouble(bbox[1]));
        argStore.emplace_back(fmtDouble(bbox[2]));
        argStore.emplace_back(fmtDouble(bbox[3]));
        argStore.emplace_back("-te_srs");   argStore.emplace_back(srs);
        argStore.emplace_back("-ts");
        argStore.emplace_back(std::to_string(width));
        argStore.emplace_back(std::to_string(height));
        argStore.emplace_back("-r");        argStore.emplace_back("bilinear");
        argStore.emplace_back("-multi");
        if (fi.wantsAlpha)
            argStore.emplace_back("-dstalpha");
        if (fi.jpegCompositing) {
            // JPEG has no alpha: use white background for nodata regions
            argStore.emplace_back("-dstnodata"); argStore.emplace_back("255 255 255");
        }

        std::vector<char *> argv;
        argv.reserve(argStore.size() + 1);
        for (auto &s : argStore) argv.push_back(const_cast<char *>(s.c_str()));
        argv.push_back(nullptr);

        GDALWarpAppOptions *opts =
            GDALWarpAppOptionsNew(argv.data(), nullptr);
        if (!opts) {
            GDALClose(hSrc);
            throw GDALException("Cannot build GDALWarp options");
        }

        int usageErr = 0;
        GDALDatasetH hOut = GDALWarp(vsiPath.c_str(), nullptr, 1, &hSrc, opts, &usageErr);
        GDALWarpAppOptionsFree(opts);
        GDALClose(hSrc);

        if (!hOut || usageErr) {
            if (hOut) GDALClose(hOut);
            VSIUnlink(vsiPath.c_str());
            throw GDALException("GDALWarp failed for " + inputPath);
        }
        GDALClose(hOut);

        vsi_l_offset size = 0;
        GByte *buf = VSIGetMemFileBuffer(vsiPath.c_str(), &size, TRUE /*unlink+steal*/);
        if (!buf || size == 0) {
            if (buf) VSIFree(buf);
            throw GDALException("Empty render output for " + inputPath);
        }

        *outBytes = reinterpret_cast<uint8_t *>(buf);
        *outSize = static_cast<int>(size);
    }

    // ---------------------------------------------------------------------

    std::string queryRasterPoint(const std::string &inputPath,
                                 double x, double y,
                                 const std::string &srs)
    {
        if (inputPath.empty())
            throw InvalidArgsException("queryRasterPoint: input path is empty");

        const std::string requestedSrs = srs.empty() ? std::string("EPSG:4326") : srs;

        GDALDatasetH hDS = GDALOpenEx(inputPath.c_str(),
                                     GDAL_OF_RASTER | GDAL_OF_READONLY,
                                     nullptr, nullptr, nullptr);
        if (!hDS)
            throw GDALException("Cannot open raster: " + inputPath);

        double gt[6];
        if (GDALGetGeoTransform(hDS, gt) != CE_None) {
            GDALClose(hDS);
            throw GDALException("Raster has no geotransform: " + inputPath);
        }

        // Source SRS
        const char *wkt = GDALGetProjectionRef(hDS);
        if (!wkt || strlen(wkt) == 0) {
            GDALClose(hDS);
            throw GDALException("Raster has no projection: " + inputPath);
        }

        OGRSpatialReferenceH hDsSrs = OSRNewSpatialReference(wkt);
        OSRSetAxisMappingStrategy(hDsSrs, OAMS_TRADITIONAL_GIS_ORDER);
        OGRSpatialReferenceH hReqSrs = OSRNewSpatialReference(nullptr);
        if (OSRSetFromUserInput(hReqSrs, requestedSrs.c_str()) != OGRERR_NONE) {
            OSRDestroySpatialReference(hDsSrs);
            OSRDestroySpatialReference(hReqSrs);
            GDALClose(hDS);
            throw InvalidArgsException("Invalid SRS: " + requestedSrs);
        }
        OSRSetAxisMappingStrategy(hReqSrs, OAMS_TRADITIONAL_GIS_ORDER);

        // Transform (x,y) in request SRS to dataset SRS
        double dsX = x;
        double dsY = y;
        if (!OSRIsSame(hReqSrs, hDsSrs)) {
            OGRCoordinateTransformationH hT =
                OCTNewCoordinateTransformation(hReqSrs, hDsSrs);
            if (!hT) {
                OSRDestroySpatialReference(hDsSrs);
                OSRDestroySpatialReference(hReqSrs);
                GDALClose(hDS);
                throw GDALException("Cannot create coord transformation");
            }
            if (!OCTTransform(hT, 1, &dsX, &dsY, nullptr)) {
                OCTDestroyCoordinateTransformation(hT);
                OSRDestroySpatialReference(hDsSrs);
                OSRDestroySpatialReference(hReqSrs);
                GDALClose(hDS);
                throw GDALException("Point transformation failed");
            }
            OCTDestroyCoordinateTransformation(hT);
        }

        // Also compute lon/lat (WGS84) for the response
        double lon = x, lat = y;
        {
            OGRSpatialReferenceH hWgs84 = OSRNewSpatialReference(nullptr);
            OSRImportFromEPSG(hWgs84, 4326);
            OSRSetAxisMappingStrategy(hWgs84, OAMS_TRADITIONAL_GIS_ORDER);
            if (!OSRIsSame(hReqSrs, hWgs84)) {
                OGRCoordinateTransformationH hT =
                    OCTNewCoordinateTransformation(hReqSrs, hWgs84);
                if (hT) {
                    OCTTransform(hT, 1, &lon, &lat, nullptr);
                    OCTDestroyCoordinateTransformation(hT);
                }
            }
            OSRDestroySpatialReference(hWgs84);
        }

        OSRDestroySpatialReference(hDsSrs);
        OSRDestroySpatialReference(hReqSrs);

        // Invert geotransform: pixel <- world
        double invGt[6];
        if (!GDALInvGeoTransform(gt, invGt)) {
            GDALClose(hDS);
            throw GDALException("Cannot invert geotransform");
        }
        // Use std::floor (not truncation toward zero) so that points slightly
        // outside the raster on the negative side map to negative pixel
        // coordinates and get reported as out-of-bounds, rather than being
        // silently clamped to the edge pixel.
        const double pxd = invGt[0] + invGt[1] * dsX + invGt[2] * dsY;
        const double pyd = invGt[3] + invGt[4] * dsX + invGt[5] * dsY;
        const int px = static_cast<int>(std::floor(pxd));
        const int py = static_cast<int>(std::floor(pyd));

        const int rxs = GDALGetRasterXSize(hDS);
        const int rys = GDALGetRasterYSize(hDS);

        json out;
        out["lon"] = lon;
        out["lat"] = lat;
        out["pixel"] = {px, py};
        out["bands"] = json::array();

        const bool inBounds = (px >= 0 && py >= 0 && px < rxs && py < rys);
        const int bandCount = GDALGetRasterCount(hDS);

        for (int b = 1; b <= bandCount; b++) {
            GDALRasterBandH hBand = GDALGetRasterBand(hDS, b);
            json bj;
            int hasNodata = 0;
            double nodata = GDALGetRasterNoDataValue(hBand, &hasNodata);
            if (hasNodata) bj["nodata"] = nodata;

            if (!inBounds) {
                bj["value"] = nullptr;
            } else {
                double val = 0.0;
                CPLErr err = GDALRasterIO(hBand, GF_Read, px, py, 1, 1,
                                          &val, 1, 1, GDT_Float64, 0, 0);
                if (err != CE_None) {
                    bj["value"] = nullptr;
                } else if (hasNodata && val == nodata) {
                    bj["value"] = nullptr;
                } else {
                    bj["value"] = val;
                }
            }
            out["bands"].push_back(bj);
        }

        GDALClose(hDS);
        return out.dump();
    }

    // ---------------------------------------------------------------------
    // Spectral index rendering (NDVI/NDRE/NDWI/EVI/SAVI)
    // ---------------------------------------------------------------------
    namespace
    {
        struct IndexFormula
        {
            // 1-based band indices for typical 5-band aerial multispectral
            // (R=1, G=2, B=3, RedEdge=4, NIR=5). Falls back gracefully when
            // fewer bands are present.
            int b1; // first operand band
            int b2; // second operand band
            int b3; // optional third (EVI); 0 = unused
            // type: 0=(b1-b2)/(b1+b2); 1=SAVI; 2=EVI
            int kind;
            double L; // SAVI/EVI tuning
        };

        IndexFormula resolveIndex(const std::string &n)
        {
            std::string u; u.reserve(n.size());
            for (char c : n) u.push_back(static_cast<char>(::toupper(c)));
            if (u == "NDVI") return {5, 1, 0, 0, 0.0};        // (NIR-R)/(NIR+R)
            if (u == "NDRE") return {5, 4, 0, 0, 0.0};        // (NIR-RE)/(NIR+RE)
            if (u == "NDWI") return {2, 5, 0, 0, 0.0};        // (G-NIR)/(G+NIR)
            if (u == "SAVI") return {5, 1, 0, 1, 0.5};        // ((NIR-R)/(NIR+R+L))*(1+L)
            if (u == "EVI")  return {5, 1, 3, 2, 0.0};        // 2.5*(NIR-R)/(NIR+6R-7.5B+1)
            throw InvalidArgsException("Unknown spectral index: " + n);
        }

        // Map index value in [-1,1] to RGB (red→yellow→green ramp).
        void rampNdvi(double v, uint8_t &r, uint8_t &g, uint8_t &b, uint8_t &a)
        {
            if (std::isnan(v)) { r = g = b = 0; a = 0; return; }
            double t = (v + 1.0) * 0.5;
            if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
            // 0=red, 0.5=yellow, 1=green
            if (t < 0.5) { r = 255; g = static_cast<uint8_t>(t * 2.0 * 255); b = 0; }
            else          { r = static_cast<uint8_t>((1.0 - t) * 2.0 * 255); g = 255; b = 0; }
            a = 255;
        }
    } // anon

    void renderRasterIndex(const std::string &inputPath,
                           const std::string &indexName,
                           const double bbox[4],
                           const std::string &bboxSrs,
                           int width, int height,
                           const std::string &format,
                           uint8_t **outBytes,
                           int *outSize)
    {
        validateInputs(inputPath, bbox, width, height);
        if (!outBytes || !outSize)
            throw InvalidArgsException("renderRasterIndex: outBytes/outSize null");

        const IndexFormula idx = resolveIndex(indexName);
        const FormatInfo fi = resolveFormat(format);
        const std::string srs = bboxSrs.empty() ? std::string("EPSG:4326") : bboxSrs;

        // Step 1: warp to /vsimem GTiff at target window & resolution (keep all bands).
        GDALDatasetH hSrc = GDALOpenEx(inputPath.c_str(),
                                      GDAL_OF_RASTER | GDAL_OF_READONLY,
                                      nullptr, nullptr, nullptr);
        if (!hSrc)
            throw GDALException("Cannot open raster: " + inputPath);

        const int srcBands = GDALGetRasterCount(hSrc);
        if (srcBands < std::max({idx.b1, idx.b2, idx.b3})) {
            GDALClose(hSrc);
            throw InvalidArgsException("Raster has insufficient bands for " + indexName);
        }

        const std::string vsiTif =
            "/vsimem/ddb-idx-" + utils::generateRandomString(16) + ".tif";

        std::vector<std::string> argStore = {
            "-of", "GTiff",
            "-t_srs", srs,
            "-te", fmtDouble(bbox[0]), fmtDouble(bbox[1]),
                   fmtDouble(bbox[2]), fmtDouble(bbox[3]),
            "-te_srs", srs,
            "-ts", std::to_string(width), std::to_string(height),
            "-r", "bilinear", "-multi",
            "-ot", "Float32"
        };
        std::vector<char *> argv;
        argv.reserve(argStore.size() + 1);
        for (auto &s : argStore) argv.push_back(const_cast<char *>(s.c_str()));
        argv.push_back(nullptr);

        GDALWarpAppOptions *opts = GDALWarpAppOptionsNew(argv.data(), nullptr);
        if (!opts) { GDALClose(hSrc); throw GDALException("Cannot build warp options"); }
        int usageErr = 0;
        GDALDatasetH hWarp = GDALWarp(vsiTif.c_str(), nullptr, 1, &hSrc, opts, &usageErr);
        GDALWarpAppOptionsFree(opts);
        GDALClose(hSrc);
        if (!hWarp || usageErr) {
            if (hWarp) GDALClose(hWarp);
            VSIUnlink(vsiTif.c_str());
            throw GDALException("Warp failed for index render: " + inputPath);
        }

        // Step 2: read required bands as Float32 buffers and capture their
        // nodata values so that nodata pixels can be rendered as transparent
        // (alpha = 0) instead of being colorized as if they were valid
        // numeric samples.
        const int npx = width * height;
        std::vector<float> b1(npx), b2(npx), b3;
        if (idx.b3 > 0) b3.resize(npx);
        bool has1 = false, has2 = false, has3 = false;
        double nd1 = 0.0, nd2 = 0.0, nd3 = 0.0;

        auto readBand = [&](int bi, float *dst, bool &hasND, double &ndVal) {
            GDALRasterBandH hB = GDALGetRasterBand(hWarp, bi);
            if (!hB) throw GDALException("Missing band " + std::to_string(bi));
            CPLErr e = GDALRasterIO(hB, GF_Read, 0, 0, width, height,
                                    dst, width, height, GDT_Float32, 0, 0);
            if (e != CE_None) throw GDALException("RasterIO failed for band " + std::to_string(bi));
            int hn = 0;
            const double v = GDALGetRasterNoDataValue(hB, &hn);
            hasND = (hn != 0);
            ndVal = v;
        };
        try {
            readBand(idx.b1, b1.data(), has1, nd1);
            readBand(idx.b2, b2.data(), has2, nd2);
            if (idx.b3 > 0) readBand(idx.b3, b3.data(), has3, nd3);
        } catch (...) {
            GDALClose(hWarp);
            VSIUnlink(vsiTif.c_str());
            throw;
        }
        GDALClose(hWarp);
        VSIUnlink(vsiTif.c_str());

        // Helper to detect nodata in a single sample (tolerant of float NaN).
        auto isND = [](double v, bool hasND, double ndVal) {
            if (std::isnan(v)) return true;
            if (!hasND) return false;
            if (std::isnan(ndVal)) return true;
            return v == ndVal;
        };

        // Step 3: compute the index per-pixel into RGBA byte buffer.
        std::vector<uint8_t> rgba(static_cast<size_t>(npx) * 4);
        for (int i = 0; i < npx; ++i) {
            double v;
            const double v1 = b1[i], v2 = b2[i];
            const bool nodataHere =
                isND(v1, has1, nd1) || isND(v2, has2, nd2) ||
                (idx.b3 > 0 && isND(b3[i], has3, nd3));
            if (nodataHere) {
                v = std::nan("");
            } else {
                switch (idx.kind) {
                    case 0: { // simple normalized difference
                        const double sum = v1 + v2;
                        v = (sum == 0.0) ? std::nan("") : (v1 - v2) / sum;
                        break;
                    }
                    case 1: { // SAVI
                        const double denom = v1 + v2 + idx.L;
                        v = (denom == 0.0) ? std::nan("") : ((v1 - v2) / denom) * (1.0 + idx.L);
                        break;
                    }
                    case 2: { // EVI
                        const double v3 = b3[i];
                        const double denom = v1 + 6.0 * v2 - 7.5 * v3 + 1.0;
                        v = (denom == 0.0) ? std::nan("") : 2.5 * (v1 - v2) / denom;
                        break;
                    }
                    default: v = std::nan("");
                }
            }
            uint8_t r, g, b, a;
            rampNdvi(v, r, g, b, a);
            const size_t off = static_cast<size_t>(i) * 4;
            rgba[off + 0] = r;
            rgba[off + 1] = g;
            rgba[off + 2] = b;
            rgba[off + 3] = a;
        }

        // Step 4: write to a /vsimem dataset of the requested format.
        // JPEG cannot carry an alpha band, so for that format we composite
        // the RGBA pixels over an opaque white background and emit a 3-band
        // RGB MEM dataset before handing off to CreateCopy.
        const std::string vsiOut =
            "/vsimem/ddb-idx-out-" + utils::generateRandomString(16) + "." + fi.vsiExt;

        const bool emitAlpha = !fi.jpegCompositing;
        const int outBandCount = emitAlpha ? 4 : 3;

        GDALDriverH hMem = GDALGetDriverByName("MEM");
        GDALDatasetH hRgba = GDALCreate(hMem, "", width, height, outBandCount,
                                        GDT_Byte, nullptr);
        if (emitAlpha) {
            for (int b = 0; b < 4; ++b) {
                GDALRasterBandH hB = GDALGetRasterBand(hRgba, b + 1);
                GDALRasterIO(hB, GF_Write, 0, 0, width, height,
                             rgba.data() + b, width, height, GDT_Byte, 4, 4 * width);
            }
            GDALColorInterp ci[4] = { GCI_RedBand, GCI_GreenBand, GCI_BlueBand, GCI_AlphaBand };
            for (int b = 0; b < 4; ++b)
                GDALSetRasterColorInterpretation(GDALGetRasterBand(hRgba, b + 1), ci[b]);
        } else {
            // Composite RGBA over white (255,255,255) so nodata regions are
            // visible but distinct from valid red samples after JPEG encode.
            std::vector<uint8_t> rgb(static_cast<size_t>(npx) * 3);
            for (int i = 0; i < npx; ++i) {
                const size_t in = static_cast<size_t>(i) * 4;
                const size_t on = static_cast<size_t>(i) * 3;
                const double a = rgba[in + 3] / 255.0;
                for (int c = 0; c < 3; ++c) {
                    const double src = rgba[in + c];
                    const double bg = 255.0;
                    const double comp = a * src + (1.0 - a) * bg;
                    rgb[on + c] = static_cast<uint8_t>(std::clamp(comp, 0.0, 255.0));
                }
            }
            for (int b = 0; b < 3; ++b) {
                GDALRasterBandH hB = GDALGetRasterBand(hRgba, b + 1);
                GDALRasterIO(hB, GF_Write, 0, 0, width, height,
                             rgb.data() + b, width, height, GDT_Byte, 3, 3 * width);
            }
            GDALColorInterp ci[3] = { GCI_RedBand, GCI_GreenBand, GCI_BlueBand };
            for (int b = 0; b < 3; ++b)
                GDALSetRasterColorInterpretation(GDALGetRasterBand(hRgba, b + 1), ci[b]);
        }

        GDALDriverH hOutDrv = GDALGetDriverByName(fi.driver);
        if (!hOutDrv) {
            GDALClose(hRgba);
            throw GDALException(std::string("Driver not available: ") + fi.driver);
        }
        GDALDatasetH hOut = GDALCreateCopy(hOutDrv, vsiOut.c_str(), hRgba,
                                           FALSE, nullptr, nullptr, nullptr);
        GDALClose(hRgba);
        if (!hOut) {
            VSIUnlink(vsiOut.c_str());
            throw GDALException("CreateCopy failed for index render");
        }
        GDALClose(hOut);

        vsi_l_offset size = 0;
        GByte *buf = VSIGetMemFileBuffer(vsiOut.c_str(), &size, TRUE);
        if (!buf || size == 0) {
            if (buf) VSIFree(buf);
            throw GDALException("Empty index render output");
        }
        *outBytes = reinterpret_cast<uint8_t *>(buf);
        *outSize = static_cast<int>(size);
    }

} // namespace ddb
