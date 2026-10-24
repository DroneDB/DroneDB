/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "cancellation.h"
#include "stringutil.h"
#include "coordstransformer.h"
#include "gdal_raii.h"
#include "raster_utils.h"
#include "gdal_priv.h"
#include "gdal_utils.h"
#include "cpl_conv.h"
#include "cpl_vsi.h"
#include "ogr_spatialref.h"

#include "raster_region.h"
#include "raster_cache.h"
#include "vegetation.h"
#include "vis_render.h"
#include "tile_encoder.h"
#include "exceptions.h"
#include "logger.h"
#include "parallel.h"
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

        const SrsPtr hDsSrs = srsFromWkt(wkt);
        if (!hDsSrs) {
            throw GDALException("Cannot create coord transformation");
        }
        const SrsPtr hReqSrs = srsFromUserInput(requestedSrs.c_str());
        if (!hReqSrs) {
            throw InvalidArgsException("Invalid SRS: " + requestedSrs);
        }

        // Transform (x,y) in request SRS to dataset SRS
        double dsX = x;
        double dsY = y;
        if (!OSRIsSame(hReqSrs.get(), hDsSrs.get())) {
            const auto toDataset = CoordsTransformer::tryCreate(srsOf(hReqSrs), srsOf(hDsSrs));
            if (!toDataset) {
                throw GDALException("Cannot create coord transformation");
            }
            if (!toDataset->tryTransform(1, &dsX, &dsY)) {
                throw GDALException("Point transformation failed");
            }
        }

        // Also compute lon/lat (WGS84) for the response
        double lon = x, lat = y;
        {
            const SrsPtr hWgs84 = srsFromEpsg(4326);
            if (!OSRIsSame(hReqSrs.get(), hWgs84.get())) {
                if (const auto toWgs84 = CoordsTransformer::tryCreate(srsOf(hReqSrs), srsOf(hWgs84))) {
                    toWgs84->tryTransform(1, &lon, &lat);
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
            const BandNodata nodata = readBandNodata(hBand);
            if (nodata.has) bj["nodata"] = nodata.value;

            if (!inBounds) {
                bj["value"] = nullptr;
            } else {
                double val = 0.0;
                CPLErr err = GDALRasterIO(hBand, GF_Read, px, py, 1, 1,
                                          &val, 1, 1, GDT_Float64, 0, 0);
                if (err != CE_None) {
                    bj["value"] = nullptr;
                } else if (isNodata(val, nodata)) {
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
    //
    // The arithmetic, the nodata handling and the colormap lookup are those of the shared
    // formula engine (VegetationEngine, vis_render.h): the thumbnails, the tiles and this layer
    // render a vegetation index through one implementation. What is the layer's own is the
    // choice of the formulas, the fixed band mapping and the colormap.
    // ---------------------------------------------------------------------
    namespace
    {
        // The formulas of the engine the layer renders (case-insensitive names)
        constexpr std::array<const char *, 5> kIndexFormulas = {"NDVI", "NDRE", "NDWI", "EVI", "SAVI"};

        // The bands of a typical 5-band aerial multispectral raster: R=1, G=2, B=3, RedEdge=4,
        // NIR=5. Every index above reads the NIR band, so a raster needs all five.
        constexpr const char *kIndexBandMapping = "RGBReN";
        constexpr int kIndexBands = 5;

        // The ramp the layer has always used (red, yellow, green over [-1, 1]), in the engine's table
        constexpr const char *kIndexColormap = "ryg";

        const char *resolveIndexFormula(const std::string &n)
        {
            std::string u = n;
            str::toUpper(u);
            for (const char *formula : kIndexFormulas)
                if (u == formula) return formula;
            throw InvalidArgsException("Unknown spectral index: " + n);
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

        const char *formulaId = resolveIndexFormula(indexName);
        const FormatInfo fi = resolveFormat(format);
        const std::string srs = bboxSrs.empty() ? std::string("EPSG:4326") : bboxSrs;

        // Step 1: warp to /vsimem GTiff at target window & resolution (keep all bands).
        const auto source = RasterCache::instance().acquireDataset(inputPath);
        GDALDatasetH hSrc = source->handle();

        if (GDALGetRasterCount(hSrc) < kIndexBands) {
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

        // Step 2: the shared engine reads the bands of the warped window as Float32, leaves the
        // nodata and transparent pixels out of the formula (they are rendered with alpha = 0
        // instead of being colorized as if they were valid samples) and colorizes the values
        // over the range of the formula.
        const size_t npx = static_cast<size_t>(width) * height;
        const FormulaPlan plan = planFormula(hWarp, formulaId,
                                             VegetationEngine::parseFilter(kIndexBandMapping, kIndexBands),
                                             kIndexColormap, true);
        const RasterWindow window{0, 0, width, height, width, height};
        std::vector<uint8_t> rgba = renderPlannedFormulaRgba(hWarp, plan, window, "");
        warp.reset();
        vsiTif.reset();

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
