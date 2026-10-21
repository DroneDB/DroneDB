/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "cancellation.h"
#include "gdal_raii.h"
#include "gdal_priv.h"
#include "gdal_utils.h"
#include "cpl_conv.h"
#include "cpl_vsi.h"
#include "ogr_spatialref.h"

#include "raster_region.h"
#include "raster_cache.h"
#include "tile_encoder.h"
#include "exceptions.h"
#include "logger.h"
#include "parallel.h"
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
            std::vector<std::string> creationOptions;
        };

        FormatInfo resolveFormat(const std::string &mime)
        {
            const std::string m = mime.empty() ? "image/png" : mime;
            if (m == "image/tiff" || m == "image/geotiff" || m == "tiff" || m == "tif")
                return {"GTiff", "tif", true, false, {}};

            // Renders are map previews (WMS GetMap): the same encoding as tiles
            EncodeOptions options;
            try
            {
                options.format = parseImageFormat(m);
            }
            catch (const InvalidArgsException &)
            {
                throw InvalidArgsException("Unsupported format: " + m);
            }
            const auto opts = imageCreationOptions(options);
            switch (options.format)
            {
            case ImageFormat::Jpeg:
                return {"JPEG", "jpg", false, true, opts};
            case ImageFormat::Webp:
                return {"WEBP", "webp", true, false, opts};
            default:
                return {"PNG", "png", true, false, opts};
            }
        }

        // Renders running in this process
        std::atomic<int> rendersInFlight{0};

        struct RenderInFlight
        {
            const int count;
            RenderInFlight() : count(++rendersInFlight) {}
            ~RenderInFlight() { --rendersInFlight; }
        };

        // Warper threads of a render (DDB_RENDER_THREADS, default 4; 0 = every core).
        // gdalwarp's -multi only overlaps reading with warping: the bilinear warp of a
        // 1024x768 map takes 150 ms on one thread and 60 ms on four. Only a render that
        // runs alone gets them: under load every render already has a core, and more
        // threads only contend (8 at once: 31 renders/s on one thread each, 22/s on four).
        // The threads beyond the calling one take CPU tokens into `tokens`: keep it until
        // the warp ends.
        std::vector<std::string> warpThreadArgs(const RenderInFlight &render, parallel::CpuTokens &tokens)
        {
            static const int configured = parallel::threadsOption("DDB_RENDER_THREADS", 4);
            tokens = parallel::tryAcquireThreads(render.count > 1 ? 1 : static_cast<size_t>(configured));
            return {"-wo", parallel::gdalNumThreads(tokens)};
        }

        // Hands the bytes of a written /vsimem/ render to the caller (VSIFree); the file is gone
        void takeRenderBytes(const VsiMemFile &file, uint8_t **outBytes, int *outSize,
                             const std::string &label)
        {
            int size = 0;
            auto buf = stealMemFileBuffer(file.path(), size);
            if (!buf || size == 0)
                throw GDALException("Empty render output for " + label);
            *outBytes = reinterpret_cast<uint8_t *>(buf.release());
            *outSize = size;
        }

        // Encodes ds in the output format into a buffer allocated by GDAL (VSIFree)
        void encodeDataset(GDALDatasetH ds, const FormatInfo &fi, uint8_t **outBytes, int *outSize,
                           const std::string &label)
        {
            GDALDriverH driver = GDALGetDriverByName(fi.driver);
            if (!driver)
                throw GDALException(std::string("Driver not available: ") + fi.driver);

            const VsiMemFile out = VsiMemFile::unique("ddb-render-", std::string(".") + fi.vsiExt);
            CPLStringList createOpts;
            for (const auto &co : fi.creationOptions)
                createOpts.AddString(co.c_str());
            // Closed before its bytes are taken: closing writes them
            cancel::GdalProgress progress;
            if (!adoptDataset(GDALCreateCopy(driver, out.c_str(), ds, FALSE, createOpts.List(),
                                             progress.function(), progress.data())))
            {
                cancel::throwIfCanceled();
                throw GDALException("Cannot encode the render of " + label);
            }

            takeRenderBytes(out, outBytes, outSize, label);
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
                            int *outSize,
                            const std::string &outputCrs,
                            const std::vector<int> &bands)
    {
        validateInputs(inputPath, bbox, width, height);
        if (!outBytes || !outSize)
            throw InvalidArgsException("renderRasterRegion: outBytes/outSize null");

        const FormatInfo fi = resolveFormat(format);
        const std::string srs = bboxSrs.empty() ? std::string("EPSG:4326") : bboxSrs;
        // outputCrs empty → keep current behaviour (target SRS = bbox SRS).
        const std::string tSrs = outputCrs.empty() ? srs : outputCrs;

        // A handle kept from previous requests: no open, warm block cache
        const auto source = RasterCache::instance().acquireDataset(inputPath);
        GDALDatasetH hSrc = source->handle();

        // Validate requested band indices against the source raster.
        if (!bands.empty())
        {
            const int srcBands = GDALGetRasterCount(hSrc);
            for (int b : bands)
            {
                if (b < 1 || b > srcBands) {
                    throw InvalidArgsException(
                        "renderRasterRegion: band " + std::to_string(b) +
                        " out of range [1," + std::to_string(srcBands) + "]");
                }
            }
        }

        // PNG, JPEG and WebP can only be written by CreateCopy: gdalwarp would then pull
        // the warp block by block on one thread. Warp into memory with the warper's
        // threads and encode the result instead.
        const bool directOutput = std::string(fi.driver) == "GTiff";
        const VsiMemFile out = VsiMemFile::unique("ddb-render-", std::string(".") + fi.vsiExt);

        // Build GDALWarp options
        std::vector<std::string> argStore;
        argStore.emplace_back("-of");       argStore.emplace_back(directOutput ? fi.driver : "MEM");
        argStore.emplace_back("-t_srs");    argStore.emplace_back(tSrs);
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
        const RenderInFlight inFlight;
        parallel::CpuTokens warpTokens;
        for (const auto &arg : warpThreadArgs(inFlight, warpTokens))
            argStore.push_back(arg);
        if (directOutput) {
            for (const auto &co : fi.creationOptions) {
                argStore.emplace_back("-co");
                argStore.emplace_back(co);
            }
        }
        for (int b : bands) {
            argStore.emplace_back("-b");
            argStore.emplace_back(std::to_string(b));
        }
        // Append an alpha mask only when the caller did not pin a specific band
        // layout. When @p bands is non-empty the output must match exactly the
        // requested bands (WCS RangeSubset semantics).
        if (fi.wantsAlpha && bands.empty())
            argStore.emplace_back("-dstalpha");
        if (fi.jpegCompositing) {
            // JPEG has no alpha: use white background for nodata regions.
            // Build one nodata value per output band to satisfy gdalwarp
            // (single-band grayscale outputs reject a 3-value "255 255 255").
            const int outBandCount = bands.empty()
                ? GDALGetRasterCount(hSrc)
                : static_cast<int>(bands.size());
            std::string nodataStr = "255";
            for (int i = 1; i < outBandCount; ++i) nodataStr += " 255";
            argStore.emplace_back("-dstnodata"); argStore.emplace_back(nodataStr);
        }

        std::vector<char *> argv;
        argv.reserve(argStore.size() + 1);
        for (auto &s : argStore) argv.push_back(const_cast<char *>(s.c_str()));
        argv.push_back(nullptr);

        const GdalOptionsPtr<GDALWarpAppOptions> opts(GDALWarpAppOptionsNew(argv.data(), nullptr));
        if (!opts)
            throw GDALException("Cannot build GDALWarp options");
        cancel::GdalProgress progress; // the warp stops when the call is canceled (DQ-15)
        progress.attach(opts.get());

        int usageErr = 0;
        auto warped = adoptDataset(
            GDALWarp(directOutput ? out.c_str() : "", nullptr, 1, &hSrc, opts.get(), &usageErr));
        if (!warped || usageErr)
        {
            cancel::throwIfCanceled();
            throw GDALException("GDALWarp failed for " + inputPath);
        }

        if (!directOutput) {
            encodeDataset(datasetHandle(warped), fi, outBytes, outSize, inputPath);
            return;
        }
        // Closed before its bytes are taken: closing writes them
        warped.reset();

        takeRenderBytes(out, outBytes, outSize, inputPath);
    }

    // ---------------------------------------------------------------------

    std::string queryRasterPoint(const std::string &inputPath,
                                 double x, double y,
                                 const std::string &srs)
    {
        if (inputPath.empty())
            throw InvalidArgsException("queryRasterPoint: input path is empty");

        const std::string requestedSrs = srs.empty() ? std::string("EPSG:4326") : srs;

        const auto source = RasterCache::instance().acquireDataset(inputPath);
        GDALDatasetH hDS = source->handle();

        double gt[6];
        if (GDALGetGeoTransform(hDS, gt) != CE_None) {
            throw GDALException("Raster has no geotransform: " + inputPath);
        }

        // Source SRS
        const char *wkt = GDALGetProjectionRef(hDS);
        if (!wkt || strlen(wkt) == 0) {
            throw GDALException("Raster has no projection: " + inputPath);
        }

        const SrsPtr hDsSrs(OSRNewSpatialReference(wkt));
        OSRSetAxisMappingStrategy(hDsSrs.get(), OAMS_TRADITIONAL_GIS_ORDER);
        const SrsPtr hReqSrs(OSRNewSpatialReference(nullptr));
        if (OSRSetFromUserInput(hReqSrs.get(), requestedSrs.c_str()) != OGRERR_NONE) {
            throw InvalidArgsException("Invalid SRS: " + requestedSrs);
        }
        OSRSetAxisMappingStrategy(hReqSrs.get(), OAMS_TRADITIONAL_GIS_ORDER);

        // Transform (x,y) in request SRS to dataset SRS
        double dsX = x;
        double dsY = y;
        if (!OSRIsSame(hReqSrs.get(), hDsSrs.get())) {
            const CoordTransformPtr hT(OCTNewCoordinateTransformation(hReqSrs.get(), hDsSrs.get()));
            if (!hT) {
                throw GDALException("Cannot create coord transformation");
            }
            if (!OCTTransform(hT.get(), 1, &dsX, &dsY, nullptr)) {
                throw GDALException("Point transformation failed");
            }
        }

        // Also compute lon/lat (WGS84) for the response
        double lon = x, lat = y;
        {
            const SrsPtr hWgs84 = srsFromEpsg(4326);
            if (!OSRIsSame(hReqSrs.get(), hWgs84.get())) {
                const CoordTransformPtr hT(
                    OCTNewCoordinateTransformation(hReqSrs.get(), hWgs84.get()));
                if (hT) {
                    OCTTransform(hT.get(), 1, &lon, &lat, nullptr);
                }
            }
        }

        // Invert geotransform: pixel <- world
        double invGt[6];
        if (!GDALInvGeoTransform(gt, invGt)) {
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
            std::string u = n;
            utils::toUpper(u);
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
        const auto source = RasterCache::instance().acquireDataset(inputPath);
        GDALDatasetH hSrc = source->handle();

        const int srcBands = GDALGetRasterCount(hSrc);
        if (srcBands < std::max({idx.b1, idx.b2, idx.b3})) {
            throw InvalidArgsException("Raster has insufficient bands for " + indexName);
        }

        VsiMemFile vsiTif = VsiMemFile::unique("ddb-idx-", ".tif");

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
        const RenderInFlight inFlight;
        parallel::CpuTokens warpTokens;
        for (const auto &arg : warpThreadArgs(inFlight, warpTokens))
            argStore.push_back(arg);
        std::vector<char *> argv;
        argv.reserve(argStore.size() + 1);
        for (auto &s : argStore) argv.push_back(const_cast<char *>(s.c_str()));
        argv.push_back(nullptr);

        const GdalOptionsPtr<GDALWarpAppOptions> opts(GDALWarpAppOptionsNew(argv.data(), nullptr));
        if (!opts) throw GDALException("Cannot build warp options");
        cancel::GdalProgress progress; // the warp stops when the call is canceled (DQ-15)
        progress.attach(opts.get());
        int usageErr = 0;
        auto warp = adoptDataset(
            GDALWarp(vsiTif.c_str(), nullptr, 1, &hSrc, opts.get(), &usageErr));
        if (!warp || usageErr) {
            cancel::throwIfCanceled();
            throw GDALException("Warp failed for index render: " + inputPath);
        }
        GDALDatasetH hWarp = datasetHandle(warp);

        // Step 2: read required bands as Float32 buffers and capture their
        // nodata values so that nodata pixels can be rendered as transparent
        // (alpha = 0) instead of being colorized as if they were valid
        // numeric samples.
        const size_t npx = static_cast<size_t>(width) * height;
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
        readBand(idx.b1, b1.data(), has1, nd1);
        readBand(idx.b2, b2.data(), has2, nd2);
        if (idx.b3 > 0) readBand(idx.b3, b3.data(), has3, nd3);
        warp.reset();
        vsiTif.reset();

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

        const bool emitAlpha = !fi.jpegCompositing;
        const int outBandCount = emitAlpha ? 4 : 3;

        GDALDriverH hMem = GDALGetDriverByName("MEM");
        if (!hMem)
            throw GDALException("GDALGetDriverByName(\"MEM\") returned null; MEM driver unavailable");
        const auto rgbaDs = adoptDataset(GDALCreate(hMem, "", width, height, outBandCount,
                                                    GDT_Byte, nullptr));
        if (!rgbaDs)
            throw GDALException("GDALCreate failed for in-memory RGBA dataset");
        GDALDatasetH hRgba = datasetHandle(rgbaDs);
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

        encodeDataset(hRgba, fi, outBytes, outSize, inputPath);
    }

} // namespace ddb
