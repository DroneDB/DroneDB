/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "raster_export.h"

#include <algorithm>
#include <random>
#include <sstream>
#include <vector>

#include "cancellation.h"
#include "exceptions.h"
#include "gdal_raii.h"
#include "gtiff_options.h"
#include "logger.h"
#include "mio.h"
#include "parallel.h"
#include "raster_utils.h"
#include "sensorprofile.h"
#include "vegetation.h"
#include "vis_render.h"

namespace ddb
{

namespace
{

// Build internal, power-of-two overviews on a freshly written GeoTIFF so the
// output supports efficient zoomed/partial reads (closer to a cloud-optimized
// layout). Levels are added while the largest dimension stays >= 256 px, using
// the same DEFLATE+PREDICTOR compression for the pyramids. Best-effort: a
// failure to build overviews is non-fatal (the full-resolution data is intact).
void buildInternalOverviews(GDALDatasetH hDs, int width, int height)
{
    std::vector<int> levels;
    int factor = 2;
    while (std::max(width, height) / factor >= 256 && levels.size() < 8)
    {
        levels.push_back(factor);
        factor *= 2;
    }
    if (levels.empty()) return;
    // Thread-local config so concurrent exports do not clobber global GDAL state.
    const ScopedThreadConfigOption compress("COMPRESS_OVERVIEW", "DEFLATE");
    const ScopedThreadConfigOption predictor("PREDICTOR_OVERVIEW", "2");
    GDALBuildOverviews(hDs, "AVERAGE", static_cast<int>(levels.size()),
                       levels.data(), 0, nullptr, nullptr, nullptr);
}

// Auto-compute a windowed tile size for the formula export branch.
// - Explicit tileSize > 0: clamped to [64, 8192], then rounded up to the
//   nearest multiple of 16 (GeoTIFF tiling requirement).
// - Auto (tileSize == 0): starts from max(blockSize, 512) clamped to
//   [256, 1024], then halved until the per-tile working set
//   (bandCount input floats + formula result float + RGBA bytes,
//   x2 headroom) fits within ~64 MB, with a floor of 256; the
//   result is then rounded up to the nearest multiple of 16.
int computeExportTileSize(GDALDatasetH hSrcDs, int bandCount, int tileSize)
{
    int result;
    if (tileSize > 0)
    {
        result = std::clamp(tileSize, 64, 8192);
    }
    else
    {
        int bx = 0, by = 0;
        GDALGetBlockSize(GDALGetRasterBand(hSrcDs, 1), &bx, &by);
        int t = std::clamp(std::max(bx, 512), 256, 1024);

        const size_t budgetBytes = 64ull * 1024 * 1024;
        // Working set per tile: bandCount input floats + formula result float +
        // RGBA bytes, with a x2 headroom factor for GDAL internal buffers.
        auto tileBytes = [&](int s) {
            const size_t pix = static_cast<size_t>(s) * s;
            return pix * (static_cast<size_t>(bandCount) * sizeof(float) + sizeof(float) + 4) * 2;
        };
        while (t > 256 && tileBytes(t) > budgetBytes) t /= 2;
        result = std::max(t, 256);
    }

    // GeoTIFF requires tile dimensions to be a multiple of 16. Round up and
    // re-clamp so the value is valid both as a processing window and as the
    // output BLOCKXSIZE/BLOCKYSIZE.
    result = ((result + 15) / 16) * 16;
    return std::clamp(result, 64, 8192);
}

// The output file of an export, deleted unless the export completed (commit): a failed or
// canceled export leaves no partial file
class PartialOutput
{
public:
    explicit PartialOutput(const char *path) : path_(path) {}
    ~PartialOutput()
    {
        if (!committed_)
            VSIUnlink(path_);
    }
    PartialOutput(const PartialOutput &) = delete;
    PartialOutput &operator=(const PartialOutput &) = delete;
    void commit() noexcept { committed_ = true; }

private:
    const char *path_;
    bool committed_ = false;
};

// One cancellation for the whole call (DQ-15): the token of this call is canceled by the
// caller's token, by the shutdown, and by the progress callback when it asks to stop,
// so every checkpoint of the export, GDAL's included, honours all three.
class ExportSession
{
public:
    explicit ExportSession(const std::function<bool(double, const char *)> &progress) : progress_(progress) {}

    // Reports progress; never throws (GDAL calls it through its progress callback)
    void report(double fraction, const char *phase)
    {
        if (progress_ && progress_(fraction, phase))
            callToken_.cancel("Export canceled by the progress callback");
    }

    // Reports progress, then stops the export when it was canceled
    void checkpoint(double fraction, const char *phase)
    {
        report(fraction, phase);
        cancel::throwIfCanceled();
    }

private:
    const std::function<bool(double, const char *)> &progress_;
    cancel::CallToken callToken_;
};

// The grid of windows a formula export reads and writes
struct WindowGrid
{
    int width = 0, height = 0;
    int tw = 0, th = 0;     // window size
    int ntx = 0, nty = 0;   // windows across and down

    WindowGrid(int width, int height, int tileSize)
        : width(width), height(height), tw(tileSize), th(tileSize),
          ntx((width + tw - 1) / tw), nty((height + th - 1) / th) {}

    size_t total() const { return static_cast<size_t>(ntx) * nty; }

    // The window (tx, ty): its origin and size (smaller at the right and bottom edges)
    RasterWindow at(int tx, int ty) const
    {
        const int x0 = tx * tw, y0 = ty * th;
        const int winW = std::min(tw, width - x0);
        const int winH = std::min(th, height - y0);
        return RasterWindow{x0, y0, winW, winH, winW, winH};
    }
};

// Determine rescale range. Explicit rescale and formula-defined ranges
// keep the export fully memory-bounded. The auto-percentile fallback
// performs an extra windowed pass that collects valid values (only hit
// when neither an explicit range nor a formula range is available).
std::pair<float, float> displayRange(GDALDatasetH hSrcDs, const FormulaPlan &plan, const WindowGrid &grid,
                                     const std::string &rescale)
{
    if (const auto fixedRange = fixedDisplayRange(rescale, *plan.formula))
        return *fixedRange;

    // Auto-percentile fallback: collect valid values with a windowed
    // pass. To keep memory bounded regardless of raster size, values
    // are uniformly subsampled into a fixed-capacity reservoir instead
    // of storing every pixel.
    constexpr size_t kMaxSamples = 4ull * 1024 * 1024;  // 16 MB of floats
    FormulaBuffers buffers;
    std::vector<float> valid;
    valid.reserve(std::min<size_t>(kMaxSamples, static_cast<size_t>(grid.width) * grid.height));
    // Deterministic, thread-local RNG (fixed seed) for reproducible
    // reservoir sampling, independent of the global std::rand() state.
    std::mt19937 reservoirRng(1337u);
    std::uniform_real_distribution<double> reservoirUni(0.0, 1.0);
    size_t seen = 0;
    for (int ty = 0; ty < grid.nty; ty++)
    {
        for (int tx = 0; tx < grid.ntx; tx++)
        {
            cancel::throwIfCanceled();
            computeFormulaValues(hSrcDs, plan, grid.at(tx, ty), buffers);
            for (float v : buffers.result)
            {
                if (!isFormulaValue(v)) continue;
                seen++;  // count this element before drawing (Algorithm R)
                if (valid.size() < kMaxSamples)
                {
                    valid.push_back(v);
                }
                else
                {
                    // Reservoir sampling: draw j from [0, seen) where seen
                    // already includes this element → probability k/seen.
                    const size_t j = static_cast<size_t>(reservoirUni(reservoirRng) * seen);
                    if (j < kMaxSamples) valid[j] = v;
                }
            }
        }
    }
    return percentileDisplayRange(valid);
}

// Creates the tiled RGBA GeoTIFF of a formula export, with the georeferencing of the source
GDALDatasetUniquePtr createRgbaOutput(const char *outputPath, GDALDatasetH hSrcDs, const WindowGrid &grid)
{
    // Get source geotransform and projection
    double geoTransform[6];
    bool hasGeoTransform = (GDALGetGeoTransform(hSrcDs, geoTransform) == CE_None);
    const char *projRef = GDALGetProjectionRef(hSrcDs);
    std::string projection = projRef ? std::string(projRef) : "";

    // Create tiled output GeoTIFF with 4 bands (RGBA)
    GDALDriverH gtiffDrv = GDALGetDriverByName("GTiff");
    if (!gtiffDrv)
        throw GDALException("GTiff driver not available");

    GtiffOptions createOpts;
    createOpts.tiled(grid.tw).compress(GtiffOptions::Codec::Deflate).predictorFor(GDT_Byte).bigTiffIfSafer();
    // Compression threads: this one and those the CPU pool allows (DQ-15), held until
    // the output is closed
    const auto cpu = parallel::tryAcquireThreads(parallel::hardwareThreads());
    createOpts.add(parallel::gdalNumThreads(cpu));

    // Closed on any throw. On success the caller closes it explicitly after the finalization.
    auto outGuard = adoptDataset(
        GDALCreate(gtiffDrv, outputPath, grid.width, grid.height, 4, GDT_Byte, createOpts.list().List()));
    if (!outGuard)
        throw GDALException("Cannot create output GeoTIFF");
    GDALDatasetH hOut = datasetHandle(outGuard);

    if (hasGeoTransform) GDALSetGeoTransform(hOut, geoTransform);
    if (!projection.empty()) GDALSetProjection(hOut, projection.c_str());
    GDALSetRasterColorInterpretation(GDALGetRasterBand(hOut, 1), GCI_RedBand);
    GDALSetRasterColorInterpretation(GDALGetRasterBand(hOut, 2), GCI_GreenBand);
    GDALSetRasterColorInterpretation(GDALGetRasterBand(hOut, 3), GCI_BlueBand);
    GDALSetRasterColorInterpretation(GDALGetRasterBand(hOut, 4), GCI_AlphaBand);
    return outGuard;
}

// Computes the formula window by window, colors it and writes the four channels
void renderWindows(GDALDatasetH hSrcDs, const FormulaPlan &plan, const WindowGrid &grid,
                   const std::pair<float, float> &range, GDALDatasetH hOut, ExportSession &session)
{
    FormulaBuffers buffers;
    std::vector<uint8_t> rgba;
    std::vector<uint8_t> chanData;
    size_t doneTiles = 0;
    const size_t totalTiles = grid.total();

    session.checkpoint(0.0, "rendering");
    for (int ty = 0; ty < grid.nty; ty++)
    {
        for (int tx = 0; tx < grid.ntx; tx++)
        {
            session.checkpoint(static_cast<double>(doneTiles) / totalTiles, "rendering");

            const RasterWindow window = grid.at(tx, ty);
            const int x0 = window.x, y0 = window.y;
            const int winW = window.xsize, winH = window.ysize;
            const size_t winPix = static_cast<size_t>(winW) * winH;

            computeFormulaValues(hSrcDs, plan, window, buffers);

            rgba.resize(winPix * 4);
            VegetationEngine::instance().applyColormap(buffers.result.data(), rgba.data(), winPix,
                                                       *plan.colormap,
                                                       {range.first, range.second, NODATA_SENTINEL});

            chanData.resize(winPix);
            for (int ch = 0; ch < 4; ch++)
            {
                for (size_t i = 0; i < winPix; i++) chanData[i] = rgba[i * 4 + ch];
                GDALRasterBandH hBand = GDALGetRasterBand(hOut, ch + 1);
                if (GDALRasterIO(hBand, GF_Write, x0, y0, winW, winH,
                                 chanData.data(), winW, winH, GDT_Byte, 0, 0) != CE_None)
                    throw GDALException("Cannot write output tile");
            }
            doneTiles++;
        }
    }
}

// Formula mode: apply formula + colormap, export as RGBA GeoTIFF.
// Block-windowed so peak memory is O(tileW * tileH * bandCount) and
// independent of the full raster size.
void exportFormula(const RasterExport &request, ExportSession &session, GDALDatasetUniquePtr &srcGuard)
{
    GDALDatasetH hSrcDs = datasetHandle(srcGuard);
    const int width = GDALGetRasterXSize(hSrcDs);
    const int height = GDALGetRasterYSize(hSrcDs);
    const int bandCount = GDALGetRasterCount(hSrcDs);
    const char *outputPath = request.output.c_str();

    const FormulaPlan plan =
        planFormula(hSrcDs, request.input, request.formula, request.bandFilter, request.colormap, true);

    const WindowGrid grid(width, height, computeExportTileSize(hSrcDs, bandCount, request.tileSize));
    const std::pair<float, float> range = displayRange(hSrcDs, plan, grid, request.rescale);

    // Deleted unless the export completes; declared first, so the dataset is closed
    // before the file is deleted
    PartialOutput partialOutput(outputPath);
    auto outGuard = createRgbaOutput(outputPath, hSrcDs, grid);
    GDALDatasetH hOut = datasetHandle(outGuard);

    renderWindows(hSrcDs, plan, grid, range, hOut, session);

    GDALFlushCache(hOut);
    // Add internal overviews for efficient zoomed/partial reads.
    buildInternalOverviews(hOut, width, height);
    outGuard.reset();  // closes hOut
    srcGuard.reset();  // closes hSrcDs
    partialOutput.commit();
    session.report(1.0, "rendering");
}

// The bands to export: the explicit list, the preset of the sensor, or the first three
std::vector<int> selectBands(const RasterExport &request, int bandCount)
{
    std::vector<int> selectedBands;
    try
    {
        if (!request.bands.empty())
        {
            std::istringstream ss(request.bands);
            std::string token;
            while (std::getline(ss, token, ','))
            {
                int b = std::stoi(token);
                if (b < 1 || b > bandCount)
                    throw InvalidArgsException("Band index out of range: " + token);
                selectedBands.push_back(b);
            }
        }
        else if (!request.preset.empty())
        {
            auto &spm = SensorProfileManager::instance();
            auto mapping = spm.getBandMappingForPreset(request.input, request.preset);
            selectedBands = {mapping.r, mapping.g, mapping.b};
        }
        else
        {
            for (int i = 1; i <= std::min(3, bandCount); i++) selectedBands.push_back(i);
        }
    }
    catch (const std::exception &e)
    {
        LOGW << "Error parsing bands/preset: " << e.what();
        throw;
    }
    return selectedBands;
}

// Non-formula mode: band selection + rescale, export as GeoTIFF
void exportBands(const RasterExport &request, ExportSession &session, GDALDatasetUniquePtr &srcGuard)
{
    GDALDatasetH hSrcDs = datasetHandle(srcGuard);
    const int width = GDALGetRasterXSize(hSrcDs);
    const int height = GDALGetRasterYSize(hSrcDs);
    const char *outputPath = request.output.c_str();
    const std::string &rescaleStr = request.rescale;

    const std::vector<int> selectedBands = selectBands(request, GDALGetRasterCount(hSrcDs));

    // Use GDALTranslate for band selection + rescale. Its progress goes to the
    // callback, and it stops when the export is canceled.
    session.checkpoint(0.0, "converting");
    CPLStringList targs;
    targs.AddString("-ot");
    targs.AddString("Byte");

    for (int b : selectedBands)
    {
        targs.AddString("-b");
        targs.AddString(std::to_string(b).c_str());
    }

    GDALDataType srcType = GDALGetRasterDataType(GDALGetRasterBand(hSrcDs, 1));
    if (srcType != GDT_Byte)
    {
        if (!rescaleStr.empty())
        {
            auto commaPos = rescaleStr.find(',');
            if (commaPos != std::string::npos)
            {
                targs.AddString("-scale");
                targs.AddString(rescaleStr.substr(0, commaPos).c_str());
                targs.AddString(rescaleStr.substr(commaPos + 1).c_str());
                targs.AddString("0");
                targs.AddString("255");
            }
        }
        else
        {
            targs.AddString("-scale");
        }
    }

    targs.AddString("-of");
    targs.AddString("GTiff");
    GtiffOptions().tiled().compress(GtiffOptions::Codec::Deflate).appendTo(targs);

    const GdalOptionsPtr<GDALTranslateOptions> psOptions(
        GDALTranslateOptionsNew(targs.List(), nullptr));
    if (!psOptions)
        throw GDALException("Cannot create GDALTranslate options");
    cancel::GdalProgress translateProgress([&](double fraction) { session.report(fraction, "converting"); });
    translateProgress.attach(psOptions.get());

    PartialOutput partialOutput(outputPath);
    int bUsageError = FALSE;
    auto out = adoptDataset(GDALTranslate(outputPath, hSrcDs, psOptions.get(), &bUsageError));
    srcGuard.reset();  // closes hSrcDs

    if (!out || bUsageError)
    {
        out.reset();
        cancel::throwIfCanceled();
        throw GDALException("Cannot create output GeoTIFF");
    }

    GDALFlushCache(datasetHandle(out));
    out.reset();
    // Reopen in update mode to add internal overviews (efficient zoomed
    // reads). Best-effort: a failure here is non-fatal.
    if (const auto ovr = adoptDataset(GDALOpen(outputPath, GA_Update)))
    {
        buildInternalOverviews(datasetHandle(ovr), width, height);
    }
    partialOutput.commit();
    session.report(1.0, "converting");
}

} // namespace

void exportRaster(const RasterExport &request)
{
    // Creating the output truncates an existing file, so it must not be the input
    io::assureDistinctOutput(io::fromUtf8(request.input), io::fromUtf8(request.output));

    ExportSession session(request.progress);

    // Closed on any return or throw
    auto srcGuard = adoptDataset(GDALOpen(request.input.c_str(), GA_ReadOnly));
    if (!srcGuard)
        throw GDALException("Cannot open " + request.input);

    if (!request.formula.empty())
        exportFormula(request, session, srcGuard);
    else
        exportBands(request, session, srcGuard);
}

} // namespace ddb
