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
            auto encoded = adoptDataset(GDALCreateCopy(driver, out.c_str(), ds, FALSE, createOpts.List(),
                                                       progress.function(), progress.data()));
            if (!encoded)
            {
                cancel::throwIfCanceled();
                throw GDALException("Cannot encode the render of " + label);
            }
            closeOutputOrThrow(std::move(encoded), "the render of " + label);

            takeRenderBytes(out, outBytes, outSize, label);
        }

        std::string fmtDouble(double v) {
            std::ostringstream os;
            os << std::setprecision(17) << v;
            return os.str();
        }

        // Fails unless every requested band exists in the source (none requested: all do)
        void requireBandsInRange(GDALDatasetH hSrc, const std::vector<int> &bands)
        {
            if (bands.empty())
                return;
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

        // The gdalwarp arguments of a region render, without the warper's threads: the window
        // (bbox in srs, size in pixels) in the CRS tSrs, with the format's creation options when
        // gdalwarp writes the file itself, the bands requested, and the transparency of the format
        std::vector<std::string> regionWarpArgs(const RegionRequest &request, const RegionOptions &options,
                                                const FormatInfo &fi, GDALDatasetH hSrc, const std::string &srs,
                                                const std::string &tSrs)
        {
            const auto &bbox = request.bbox;
            const std::vector<int> &bands = options.bands;
            const bool directOutput = std::string(fi.driver) == "GTiff";

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
            argStore.emplace_back(std::to_string(request.width));
            argStore.emplace_back(std::to_string(request.height));
            argStore.emplace_back("-r");        argStore.emplace_back("bilinear");
            argStore.emplace_back("-multi");
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
            return argStore;
        }

        // The value of the pixel in every band, with the band's nodata value when it has one: null
        // for a pixel outside the raster, one that cannot be read and one that is nodata
        json pixelBandValues(GDALDatasetH hDS, int px, int py)
        {
            const bool inBounds = (px >= 0 && py >= 0 && px < GDALGetRasterXSize(hDS) && py < GDALGetRasterYSize(hDS));
            const int bandCount = GDALGetRasterCount(hDS);

            json bands = json::array();
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
                bands.push_back(bj);
            }
            return bands;
        }

        void validateInputs(const RegionRequest &request)
        {
            if (request.inputPath.empty())
                throw InvalidArgsException("renderRasterRegion: input path is empty");
            if (request.width  <= 0 || request.width  > 4096)
                throw InvalidArgsException("renderRasterRegion: width out of range [1,4096]");
            if (request.height <= 0 || request.height > 4096)
                throw InvalidArgsException("renderRasterRegion: height out of range [1,4096]");
            if (request.bbox[0] >= request.bbox[2] || request.bbox[1] >= request.bbox[3])
                throw InvalidArgsException("renderRasterRegion: invalid bbox order");
        }

    } // anonymous namespace

    // ---------------------------------------------------------------------

    void renderRasterRegion(const RegionRequest &request,
                            const RegionOptions &options,
                            uint8_t **outBytes,
                            int *outSize)
    {
        validateInputs(request);
        if (!outBytes || !outSize)
            throw InvalidArgsException("renderRasterRegion: outBytes/outSize null");

        const std::string &inputPath = request.inputPath;

        const FormatInfo fi = resolveFormat(request.format);
        const std::string srs = request.bboxSrs.empty() ? std::string("EPSG:4326") : request.bboxSrs;
        // outputCrs empty → keep current behaviour (target SRS = bbox SRS).
        const std::string tSrs = options.outputCrs.empty() ? srs : options.outputCrs;

        // A handle kept from previous requests: no open, warm block cache
        const auto source = RasterCache::instance().acquireDataset(inputPath);
        GDALDatasetH hSrc = source->handle();

        requireBandsInRange(hSrc, options.bands);

        // PNG, JPEG and WebP can only be written by CreateCopy: gdalwarp would then pull
        // the warp block by block on one thread. Warp into memory with the warper's
        // threads and encode the result instead.
        const bool directOutput = std::string(fi.driver) == "GTiff";
        const VsiMemFile out = VsiMemFile::unique("ddb-render-", std::string(".") + fi.vsiExt);

        std::vector<std::string> argStore = regionWarpArgs(request, options, fi, hSrc, srs, tSrs);
        const RenderInFlight inFlight;
        parallel::CpuTokens warpTokens;
        for (const auto &arg : warpThreadArgs(inFlight, warpTokens))
            argStore.push_back(arg);

        CPLStringList argv = cslFromStrings(argStore);
        const GdalOptionsPtr<GDALWarpAppOptions> opts(GDALWarpAppOptionsNew(argv.List(), nullptr));
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
        closeOutputOrThrow(std::move(warped), "the render of " + inputPath);

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

        json out;
        out["lon"] = lon;
        out["lat"] = lat;
        out["pixel"] = {px, py};
        out["bands"] = pixelBandValues(hDS, px, py);
        return out.dump();
    }

    // ---------------------------------------------------------------------
    // Spectral index rendering (NDVI/NDRE/NDWI/EVI/SAVI)
    //
    // The arithmetic, the nodata handling and the colormap lookup are those of the shared
    // formula engine (VegetationEngine, vis_render.h): the thumbnails, the tiles and this layer
    // render a vegetation index through one implementation, and the bands are those the tiles
    // use: the sensor profile of the raster (VegetationEngine::autoDetectFilter) says which band
    // is red, NIR... What is the layer's own is the choice of the formulas, the order of the
    // bands of a raster the profiles do not name, the colormap and the nodata rule: a pixel is
    // left out when a band its index reads has no value (NodataMask::BandsRead), as the layer
    // has always done.
    // ---------------------------------------------------------------------
    namespace
    {
        // The formulas of the engine the layer renders (case-insensitive names), in the order the
        // layer lists them
        constexpr std::array<const char *, 5> kIndexFormulas = {"NDVI", "NDRE", "NDWI", "SAVI", "EVI"};

        // The bands of a raster the sensor profiles do not name, if it has five or more: the order
        // the layer had before it read the profiles, R=1, G=2, B=3, RedEdge=4, NIR=5
        constexpr const char *kLegacyBandMapping = "RGBReN";
        constexpr int kLegacyBands = 5;

        // The ramp the layer has always used (red, yellow, green over [-1, 1]), in the engine's
        // table. It is not a colormap clients pick: the engine does not list it.
        constexpr const char *kIndexColormap = "ryg";

        // The value that is opaque in the alpha band the warper itself writes (-dstalpha, for a
        // raster that has no alpha band and no nodata value): 255 where it has a pixel, 0 where the
        // window leaves the raster. A source's own alpha band has the opaque value of its data
        // (alphaOpaqueValue)
        constexpr double kWarperAlphaMax = 255.0;

        const char *resolveIndexFormula(const std::string &n)
        {
            std::string u = n;
            str::toUpper(u);
            for (const char *formula : kIndexFormulas)
                if (u == formula) return formula;
            throw InvalidArgsException("Unknown spectral index: " + n);
        }

        // What the layer does for one index over one raster: the bands it reads it from, or why it
        // cannot
        struct IndexChoice
        {
            const VegetationFormula *formula = nullptr;
            BandFilter filter;
            std::string refusal; // empty when the raster can render the index
        };

        // The one rule of which of its indices the layer can render from a raster, and with which
        // bands. The render and the raster info both ask it.
        //   - an index that needs reflectance (SAVI, EVI) needs a floating-point raster;
        //   - the bands are those the sensor profile (band names, descriptions, colour
        //     interpretation) maps, when it maps every band the index reads;
        //   - otherwise, a raster with five data bands or more is read in the fixed order R, G, B,
        //     Red edge, NIR (kLegacyBandMapping);
        //   - otherwise the index is refused.
        class IndexLayerPlanner
        {
            BandFilter detected_;
            RasterDataKind data_;

        public:
            IndexLayerPlanner(const std::string &rasterPath, GDALDatasetH ds)
                : detected_(VegetationEngine::instance().autoDetectFilter(rasterPath)), data_(rasterDataKind(ds)) {}

            IndexChoice choose(const char *formulaId) const
            {
                IndexChoice choice;
                choice.formula = VegetationEngine::instance().getFormula(formulaId);
                choice.refusal = VegetationEngine::dataRefusal(*choice.formula, data_);
                if (!choice.refusal.empty())
                    return choice;

                choice.filter = detected_;
                const auto missing = VegetationEngine::missingSymbols(*choice.formula, detected_);
                if (missing.empty())
                    return choice;
                if (data_.bandCount >= kLegacyBands)
                {
                    choice.filter = VegetationEngine::parseFilter(kLegacyBandMapping, kLegacyBands);
                    return choice;
                }

                std::string names;
                for (const char symbol : missing)
                    names += std::string(names.empty() ? "" : ", ") + VegetationEngine::symbolName(symbol);
                choice.refusal = std::string(formulaId) + " needs the " + names + " band" + (missing.size() > 1 ? "s" : "") +
                                 ", which the raster does not name, and it has " + std::to_string(data_.bandCount) +
                                 " data bands, fewer than the " + std::to_string(kLegacyBands) +
                                 " (Red, Green, Blue, Red edge, NIR) the layer reads by position";
                return choice;
            }
        };

        // Divides the bands of the warped window that the formula reads by the coverage (the
        // warped alpha over its maximum), where there is some. -nosrcalpha makes the warper blend
        // the bands with the 0 of the transparent pixels around an edge as it blends the alpha
        // band, so at the edge a band is its value times the coverage: dividing gives the value of
        // the pixels that are opaque, which SAVI and EVI (they add constants) need and the ratio
        // indices do not. A band that declares a nodata value is left alone: the warper left its
        // nodata pixels out of the blend, so it is not diluted.
        void restoreDilutedBands(GDALDatasetH warped, const std::vector<int> &bands, const RasterBandNodata &sourceNodata,
                                 const std::vector<float> &coverage, int width, int height)
        {
            std::vector<float> plane(coverage.size());
            for (const int band : bands)
            {
                if (sourceNodata.bands[band].has)
                    continue;
                GDALRasterBandH hBand = GDALGetRasterBand(warped, band + 1);
                if (GDALRasterIO(hBand, GF_Read, 0, 0, width, height, plane.data(), width, height, GDT_Float32, 0, 0) != CE_None)
                    throw GDALException("Cannot read band " + std::to_string(band + 1) + " of the index render");
                bool changed = false;
                for (size_t i = 0; i < plane.size(); ++i)
                {
                    if (coverage[i] > 0.0f && coverage[i] < 1.0f && std::isfinite(plane[i]))
                    {
                        plane[i] /= coverage[i];
                        changed = true;
                    }
                }
                if (changed &&
                    GDALRasterIO(hBand, GF_Write, 0, 0, width, height, plane.data(), width, height, GDT_Float32, 0, 0) != CE_None)
                    throw GDALException("Cannot write band " + std::to_string(band + 1) + " of the index render");
            }
        }

        // What the raster says about where it has no pixels: an alpha band, a nodata value in a
        // band, or nothing
        struct SourceTransparency
        {
            RasterBandNodata nodata;
            bool hasAlpha = false;
            // Nothing says where the raster has no pixels, so outside of it the warp would write 0
            // in every band, a value to SAVI and EVI: it writes the alpha of the pixels it has
            bool warperAlpha = false;
        };

        SourceTransparency sourceTransparency(GDALDatasetH hSrc)
        {
            SourceTransparency transparency;
            transparency.nodata = detectBandNodata(hSrc, GDALGetRasterCount(hSrc));
            transparency.hasAlpha = transparency.nodata.alphaBandIdx >= 0;
            const bool hasNodata = std::any_of(transparency.nodata.bands.begin(), transparency.nodata.bands.end(),
                                               [](const BandNodata &nodata) { return nodata.has; });
            transparency.warperAlpha = !transparency.hasAlpha && !hasNodata;
            return transparency;
        }

        // The gdalwarp arguments of the window of the index layer (every band of the source as
        // Float32), without the warper's threads
        std::vector<std::string> indexWarpArgs(const RegionRequest &request, const std::string &srs, bool warperAlpha)
        {
            const auto &bbox = request.bbox;
            std::vector<std::string> args = {
                "-of", "GTiff",
                "-t_srs", srs,
                "-te", fmtDouble(bbox[0]), fmtDouble(bbox[1]),
                       fmtDouble(bbox[2]), fmtDouble(bbox[3]),
                "-te_srs", srs,
                "-ts", std::to_string(request.width), std::to_string(request.height),
                "-r", "bilinear", "-multi",
                "-ot", "Float32",
                // The alpha band, if there is one, is warped like the others, so the engine reads the
                // source's own 0 (no data) and not what the warper makes of it: with a source alpha the
                // warper rescales it by the maximum of the band's type (65535 for UInt16, where ODM writes
                // 255), which turned a fully opaque raster into alpha 0, and it turns the pixels that are
                // nodata in a band into 0 instead of leaving them nodata. (And it takes the alpha for a
                // validity of 0 or 1: the edge of the alpha would not be anti-aliased.) The warped alpha
                // is then the coverage of the pixel, which is what the output alpha is made from below.
                "-nosrcalpha"
            };
            if (warperAlpha)
            {
                args.emplace_back("-dstalpha");
                args.emplace_back("-wo");
                args.emplace_back("DST_ALPHA_MAX=" + std::to_string(static_cast<int>(kWarperAlphaMax)));
            }
            return args;
        }

        // The coverage of every pixel of the warped window, from its alpha band: 1 inside the
        // source's pixels, 0 outside, between them along an edge where the warp blended the two.
        // The alpha band of a raster is 255 in a Byte band and also in the UInt16 bands ODM writes:
        // the value that is opaque is derived from the band's data (kept with the dataset). Empty
        // when the raster says nothing of where it has no pixels (a nodata value alone leaves them
        // out of the formula).
        std::vector<float> warpedCoverage(GDALDatasetH hWarp, const OpenedDataset &source,
                                          const SourceTransparency &transparency, const RegionRequest &request)
        {
            std::vector<float> coverage;
            if (!transparency.hasAlpha && !transparency.warperAlpha)
                return coverage;
            const int alphaBand = transparency.hasAlpha ? transparency.nodata.alphaBandIdx + 1 : GDALGetRasterCount(hWarp);
            const double opaque = transparency.hasAlpha ? source.alphaOpaque(alphaBand) : kWarperAlphaMax;
            coverage.resize(static_cast<size_t>(request.width) * request.height);
            if (GDALRasterIO(GDALGetRasterBand(hWarp, alphaBand), GF_Read, 0, 0, request.width, request.height,
                             coverage.data(), request.width, request.height, GDT_Float32, 0, 0) != CE_None)
                throw GDALException("Cannot read the alpha band of the index render of " + request.inputPath);
            for (float &c : coverage)
                c = std::isfinite(c) ? static_cast<float>(std::clamp(c / opaque, 0.0, 1.0)) : 0.0f;
            return coverage;
        }

        // The RGBA pixels (interleaved) composited over an opaque white background, as RGB: JPEG
        // cannot carry an alpha band, so nodata regions are visible but distinct from valid red
        // samples after the encode
        std::vector<uint8_t> compositeOverWhite(const std::vector<uint8_t> &rgba, size_t pixels)
        {
            std::vector<uint8_t> rgb(pixels * 3);
            for (size_t i = 0; i < pixels; ++i)
            {
                const size_t in = i * 4;
                const size_t on = i * 3;
                const double a = rgba[in + 3] / 255.0;
                for (int c = 0; c < 3; ++c)
                {
                    const double src = rgba[in + c];
                    const double bg = 255.0;
                    const double comp = a * src + (1.0 - a) * bg;
                    rgb[on + c] = static_cast<uint8_t>(std::clamp(comp, 0.0, 255.0));
                }
            }
            return rgb;
        }

        // The pixels of the render as a Byte MEM dataset, ready for CreateCopy: four bands with an
        // alpha band, or three (the pixels over white) for a format that has none
        GDALDatasetUniquePtr rgbaMemDataset(std::vector<uint8_t> &rgba, const RegionRequest &request, bool withAlpha)
        {
            const int width = request.width, height = request.height;
            const size_t pixels = static_cast<size_t>(width) * height;
            const int bandCount = withAlpha ? 4 : 3;

            GDALDriverH hMem = GDALGetDriverByName("MEM");
            if (!hMem)
                throw GDALException("GDALGetDriverByName(\"MEM\") returned null; MEM driver unavailable");
            auto dataset = adoptDataset(GDALCreate(hMem, "", width, height, bandCount, GDT_Byte, nullptr));
            if (!dataset)
                throw GDALException("GDALCreate failed for in-memory RGBA dataset");
            GDALDatasetH hRgba = datasetHandle(dataset);

            std::vector<uint8_t> rgb = withAlpha ? std::vector<uint8_t>() : compositeOverWhite(rgba, pixels);
            uint8_t *interleaved = withAlpha ? rgba.data() : rgb.data();
            for (int b = 0; b < bandCount; ++b)
            {
                if (GDALRasterIO(GDALGetRasterBand(hRgba, b + 1), GF_Write, 0, 0, width, height,
                                 interleaved + b, width, height, GDT_Byte, bandCount,
                                 bandCount * width) != CE_None)
                    throw GDALException("Cannot write the index render of " + request.inputPath);
            }
            if (withAlpha)
                setBandColors(hRgba, {GCI_RedBand, GCI_GreenBand, GCI_BlueBand, GCI_AlphaBand});
            else
                setBandColors(hRgba, {GCI_RedBand, GCI_GreenBand, GCI_BlueBand});
            return dataset;
        }
    } // anon

    std::vector<std::string> rasterIndexLayerIndices(const std::string &rasterPath)
    {
        const auto source = RasterCache::instance().acquireDataset(rasterPath);
        const IndexLayerPlanner planner(rasterPath, source->handle());
        std::vector<std::string> indices;
        for (const char *formula : kIndexFormulas)
            if (planner.choose(formula).refusal.empty())
                indices.emplace_back(formula);
        return indices;
    }

    void renderRasterIndex(const RegionRequest &request,
                           const std::string &indexName,
                           uint8_t **outBytes,
                           int *outSize)
    {
        validateInputs(request);
        if (!outBytes || !outSize)
            throw InvalidArgsException("renderRasterIndex: outBytes/outSize null");

        const std::string &inputPath = request.inputPath;
        const int width = request.width, height = request.height;

        const char *formulaId = resolveIndexFormula(indexName);
        const FormatInfo fi = resolveFormat(request.format);
        const std::string srs = request.bboxSrs.empty() ? std::string("EPSG:4326") : request.bboxSrs;

        // Step 1: which bands the index reads, over the source (the warp makes every band Float32,
        // so the type of the data is the source's), then warp to /vsimem GTiff at target window &
        // resolution (keep all bands).
        const auto source = RasterCache::instance().acquireDataset(inputPath);
        GDALDatasetH hSrc = source->handle();

        const IndexChoice choice = IndexLayerPlanner(inputPath, hSrc).choose(formulaId);
        if (!choice.refusal.empty())
            throw InvalidArgsException(choice.refusal);

        const SourceTransparency transparency = sourceTransparency(hSrc);

        VsiMemFile vsiTif = VsiMemFile::unique("ddb-idx-", ".tif");

        std::vector<std::string> argStore = indexWarpArgs(request, srs, transparency.warperAlpha);
        const RenderInFlight inFlight;
        parallel::CpuTokens warpTokens;
        for (const auto &arg : warpThreadArgs(inFlight, warpTokens))
            argStore.push_back(arg);
        CPLStringList argv = cslFromStrings(argStore);
        const GdalOptionsPtr<GDALWarpAppOptions> opts(GDALWarpAppOptionsNew(argv.List(), nullptr));
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
        const size_t npx = static_cast<size_t>(width) * height;

        // Step 2: the coverage of every pixel, from the alpha band of the warp
        const std::vector<float> coverage = warpedCoverage(hWarp, *source, transparency, request);
        if (transparency.hasAlpha)
            restoreDilutedBands(hWarp, VegetationEngine::bandsRead(*choice.formula, choice.filter), transparency.nodata, coverage, width, height);

        // Step 3: the shared engine reads the bands of the warped window as Float32, leaves the
        // nodata and transparent pixels out of the formula (they are rendered with alpha = 0
        // instead of being colorized as if they were valid samples) and colorizes the values
        // over the range of the formula; the alpha of the output is then the coverage.
        FormulaPlan plan = planFormula(hWarp, formulaId, choice.filter, "", false);
        plan.colormap = VegetationEngine::instance().getColormap(kIndexColormap, true);
        const RasterWindow window{0, 0, width, height, width, height};
        std::vector<uint8_t> rgba = renderPlannedFormulaRgba(hWarp, plan, window, "", NodataMask::BandsRead);
        if (!coverage.empty())
            for (size_t i = 0; i < npx; ++i)
                rgba[i * 4 + 3] = static_cast<uint8_t>(std::lround(rgba[i * 4 + 3] * coverage[i]));
        warp.reset();
        vsiTif.reset();

        // Step 4: write to a /vsimem dataset of the requested format
        const auto rgbaDs = rgbaMemDataset(rgba, request, !fi.jpegCompositing);
        encodeDataset(datasetHandle(rgbaDs), fi, outBytes, outSize, inputPath);
    }

} // namespace ddb
