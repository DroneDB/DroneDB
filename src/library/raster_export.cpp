/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "raster_export.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <functional>
#include <numeric>
#include <random>
#include <sstream>
#include <vector>

#include "cancellation.h"
#include "dem.h"
#include "exceptions.h"
#include "artefact_cache.h"
#include "gdal_raii.h"
#include "gtiff_options.h"
#include "json.h"
#include "logger.h"
#include "mio.h"
#include "raster_artefacts.h"
#include "raster_source.h"
#include "raster_stats.h"
#include "parallel.h"
#include "raster_utils.h"
#include "sensorprofile.h"
#include "vegetation.h"
#include "ddbt/ddbt.h"
#include "tilers_call.h"
#include "tilers_raster.h"
#include "vis_render.h"

namespace ddb
{

namespace
{

// The factors of the overviews of an export: 2, 4, 8... while the larger side of the level keeps 256 pixels at
// least, 8 at most
std::vector<int> overviewFactorsOf(int width, int height)
{
    std::vector<int> levels;
    int factor = 2;
    while (std::max(width, height) / factor >= 256 && levels.size() < 8)
    {
        levels.push_back(factor);
        factor *= 2;
    }
    return levels;
}

// Builds the internal overviews of a freshly written GeoTIFF (overviewFactorsOf), DEFLATE + PREDICTOR as the
// full resolution, so the output supports efficient zoomed/partial reads. Best-effort: a failure to build
// overviews is non-fatal (the full-resolution data is intact).
void buildInternalOverviews(GDALDatasetH hDs, int width, int height)
{
    std::vector<int> levels = overviewFactorsOf(width, height);
    if (levels.empty()) return;
    // Thread-local config so concurrent exports do not clobber global GDAL state.
    const ScopedThreadConfigOption compress("COMPRESS_OVERVIEW", "DEFLATE");
    const ScopedThreadConfigOption predictor("PREDICTOR_OVERVIEW", "2");
    if (GDALBuildOverviews(hDs, "AVERAGE", static_cast<int>(levels.size()),
                           levels.data(), 0, nullptr, nullptr, nullptr) != CE_None)
    {
        LOGD << "Cannot build the internal overviews of the export (the full-resolution data is intact)";
    }
}

// Auto-compute a windowed tile size for the formula export branch.
// - Explicit tileSize > 0: clamped to [64, 8192], then rounded up to the
//   nearest multiple of 16 (GeoTIFF tiling requirement).
// - Auto (tileSize == 0): starts from max(blockSize, 512) clamped to
//   [256, 1024], then halved until the per-tile working set
//   (bandCount input floats + formula result float + RGBA bytes,
//   x2 headroom) fits within ~64 MB, with a floor of 256; the
//   result is then rounded up to the nearest multiple of 16.
int computeExportTileSize(const RasterSource &source, int bandCount, int tileSize)
{
    int result;
    if (tileSize > 0)
    {
        result = std::clamp(tileSize, 64, 8192);
    }
    else
    {
        int bx = 0, by = 0;
        source.blockSize(bx, by);
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

// One cancellation for the whole call: the token of this call is canceled by the
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

// The part of the source that is exported: the whole raster, or the window of the request (in source pixels)
struct SourceWindow
{
    int x = 0, y = 0;
    int width = 0, height = 0;
};

// The pixels of the raster under the WGS84 window of the request (those whose centre is inside it, by the one
// geo-to-pixel conversion of the raster values), cut at the edges of the raster; the whole raster without a window
SourceWindow sourceWindowOf(const RasterExport &request, int width, int height)
{
    if (!request.bbox)
        return SourceWindow{0, 0, width, height};

    // The same file as the export reads: its geotransform and CRS
    const DemContext dem = openDem(request.input, 1, BandValueOptions(), false);
    if (!dem.hasGeoTransform)
        throw InvalidArgsException("The raster has no geotransform: no window can be placed on it");

    const auto &box = *request.bbox;
    int x0, y0, x1, y1;
    dem.wgs84BoxToPixelBounds(box[0], box[1], box[2], box[3], x0, y0, x1, y1);
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, width - 1);
    y1 = std::min(y1, height - 1);
    if (x1 < x0 || y1 < y0)
        throw InvalidArgsException("The window does not overlap the raster: nothing to export");
    return SourceWindow{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
}

// The raster an export reads: the file ddb-tilers reads for the input (the COG of a build as it is, any other
// raster as its normalized copy), seen with the NoData override of the request
class ExportSource
{
public:
    explicit ExportSource(const RasterExport &request)
    {
        raster_ = tilers::Raster::openInput(request.input);
        if (!raster_)
            throw GDALException("Cannot open " + request.input);
        file_ = rasterSourceOf(*raster_);
        if (request.nodata)
            view_ = withNodataOverride(*file_, *request.nodata);
    }

    const tilers::Raster &raster() const { return *raster_; }
    // The bands as the export reads them (with the override)
    const RasterSource &source() const { return view_ ? *view_ : *file_; }

private:
    std::unique_ptr<tilers::Raster> raster_;
    std::unique_ptr<RasterSource> file_, view_;
};

// The display range of the formula over the exported part: the explicit rescale, else the range of the formula,
// else the p2-p98 of its values over a bounded sample of the part (planBoundedSample with the budget of the stretch
// of a thumbnail, kStretchStatisticsMaxPixels, the percentiles of a formula thumbnail, percentileDisplayRange):
// every pixel of a part that has no more than that, else its pixels in the largest overview within that budget,
// else the part decimated to that budget (an even grid of its pixels, as a formula thumbnail reads it), so a large
// export does not compute the formula over the whole raster twice. A sample in which no pixel has a value is read
// once more with the larger budget of a sparse raster (sparseSampleBudget). The sample is read in windows over at
// most the automatic tile of the export, whatever tile the request asks for, so the range does not depend on it,
// and its values are kept in a reservoir of at most kMaxSamples values (a uniform subsample when there are more),
// so the memory stays bounded.
std::pair<float, float> displayRange(const RasterSource &source, const FormulaPlan &plan, const SourceWindow &part,
                                     const std::string &rescale)
{
    if (const auto fixedRange = fixedDisplayRange(rescale, *plan.formula))
        return *fixedRange;

    constexpr size_t kMaxSamples = 4ull * 1024 * 1024;  // 16 MB of floats
    FormulaBuffers buffers;
    std::vector<float> valid;
    valid.reserve(std::min<size_t>(kMaxSamples, static_cast<size_t>(part.width) * static_cast<size_t>(part.height)));
    // Deterministic, thread-local RNG (fixed seed) for reproducible
    // reservoir sampling, independent of the global std::rand() state.
    std::mt19937 reservoirRng(1337u);
    std::uniform_real_distribution<double> reservoirUni(0.0, 1.0);
    size_t seen = 0;
    const auto collect = [&](const RasterWindow &window) {
        cancel::throwIfCanceled();
        computeFormulaValues(source, plan, window, buffers);
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
                const size_t j = static_cast<size_t>(reservoirUni(reservoirRng) * static_cast<double>(seen));
                if (j < kMaxSamples) valid[j] = v;
            }
        }
    };

    std::vector<int> bands(static_cast<size_t>(plan.bandCount));
    std::iota(bands.begin(), bands.end(), 1);
    const RasterWindow window{part.x, part.y, part.width, part.height, part.width, part.height};
    const int side = computeExportTileSize(source, plan.bandCount, 0);
    const BoundedSample sample =
        planBoundedSample(source, bands, window, kStretchStatisticsMaxPixels, BoundedSample::Fallback::Decimated);
    sample.forEachWindow(side, collect);
    if (valid.empty() && sample.sampled())
    {
        LOGD << "No pixel of the sample of the formula has a value: a larger sample";
        planBoundedSample(source, bands, window, sparseSampleBudget(kStretchStatisticsMaxPixels),
                          BoundedSample::Fallback::Decimated)
            .forEachWindow(side, collect);
    }
    return percentileDisplayRange(valid);
}

// The values of a window of the source: NODATA_SENTINEL where a pixel has none, valid until the next call
using WindowValues = std::function<const std::vector<float> &(const RasterWindow &)>;

// What the windows of an RGBA export are filled with: the values of the window coloured by the colormap
struct RgbaFill
{
    RgbaFill(const WindowValues &windowValues, const Colormap &windowColormap, const std::pair<float, float> &valueRange,
             const SourceWindow &exported, ExportSession &exportSession, size_t tiles)
        : values(windowValues), colormap(windowColormap), range(valueRange), part(exported), session(exportSession),
          total(tiles)
    {
    }

    const WindowValues &values;
    const Colormap &colormap;
    std::pair<float, float> range;
    SourceWindow part;
    ExportSession &session;
    size_t total = 0;
    size_t done = 0;
    std::vector<uint8_t> rgba;
    std::exception_ptr error;

    static int fill(void *userData, int x, int y, int width, int height, uint8_t *out, size_t stride)
    {
        auto &f = *static_cast<RgbaFill *>(userData);
        try
        {
            f.session.checkpoint(static_cast<double>(f.done) / static_cast<double>(f.total), "rendering");
            const RasterWindow window{f.part.x + x, f.part.y + y, width, height, width, height};
            const std::vector<float> &values = f.values(window);
            const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
            f.rgba.resize(pixels * 4);
            VegetationEngine::instance().applyColormap(values.data(), f.rgba.data(), pixels, f.colormap,
                                                       {f.range.first, f.range.second, NODATA_SENTINEL});
            const size_t rowBytes = static_cast<size_t>(width) * 4;
            for (int row = 0; row < height; row++)
                std::memcpy(out + static_cast<size_t>(row) * stride, f.rgba.data() + static_cast<size_t>(row) * rowBytes,
                            rowBytes);
            f.done++;
            return 0;
        }
        catch (...)
        {
            f.error = std::current_exception();
            return 1;
        }
    }
};

// Writes the RGBA export of the part of the source: the values of each tile coloured by the colormap over the
// range, a tiled DEFLATE GeoTIFF with the georeferencing of the source (the corner of the part) and its average
// overviews (ddb-tilers: ddbtWriteRaster). The output appears whole or not at all.
void writeRgba(const RasterExport &request, const ExportSource &src, const SourceWindow &part, int tileSize,
               const WindowValues &values, const Colormap &colormap, const std::pair<float, float> &range,
               ExportSession &session)
{
    json options = {{"width", part.width},
                    {"height", part.height},
                    {"colorInterp", {"Red", "Green", "Blue", "Alpha"}},
                    {"blockSize", tileSize},
                    {"codec", "deflate"},
                    {"predictor", 2},
                    {"overviewFactors", overviewFactorsOf(part.width, part.height)}};
    const tilers::Raster &raster = src.raster();
    if (const auto &gt = raster.geoTransform())
    {
        // The corner of the exported part of the source
        std::array<double, 6> corner = *gt;
        corner[0] += part.x * corner[1] + part.y * corner[2];
        corner[3] += part.x * corner[4] + part.y * corner[5];
        options["geoTransform"] = corner;
        if (!raster.wkt().empty())
            options["geoKeysFrom"] = io::toUtf8(raster.file());
    }

    RgbaFill fill(values, colormap, range, part, session,
                  static_cast<size_t>((part.width + tileSize - 1) / tileSize) *
                      static_cast<size_t>((part.height + tileSize - 1) / tileSize));
    session.checkpoint(0.0, "rendering");
    const ddbtErr rc = ddbtWriteRaster(request.output.c_str(), options.dump().c_str(), &RgbaFill::fill, &fill,
                                       nullptr, nullptr);
    if (fill.error)
        std::rethrow_exception(fill.error);
    tilers::check(rc);
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

// Formula mode: apply formula + colormap, export as RGBA GeoTIFF.
// Block-windowed so peak memory is O(tileW * tileH * bandCount) and
// independent of the full raster size.
void exportFormula(const RasterExport &request, ExportSession &session, const ExportSource &src)
{
    const RasterSource &source = src.source();
    const SourceWindow part = sourceWindowOf(request, source.width(), source.height());
    const FormulaPlan plan =
        planFormula(source, request.input, request.formula, request.bandFilter, request.colormap, true);
    const std::pair<float, float> range = displayRange(source, plan, part, request.rescale);

    FormulaBuffers buffers;
    writeRgba(
        request, src, part, computeExportTileSize(source, source.bandCount(), request.tileSize),
        [&](const RasterWindow &window) -> const std::vector<float> & {
            computeFormulaValues(source, plan, window, buffers);
            return buffers.result;
        },
        *plan.colormap, range, session);
}

// The one band that a styled export of a colormap without a formula draws, or 0 when the export is not one: the
// rule of the map tiles and the thumbnails (styledBand), with the bands that the bands or the preset choose. A
// raster of several bands with no choice is the first three bands, scaled.
int styledBandOf(const RasterExport &request, const RasterSource &source)
{
    if (request.colormap.empty())
        return 0;
    const std::vector<int> chosen = request.bands.empty() && request.preset.empty()
                                        ? std::vector<int>{}
                                        : selectBands(request, source.bandCount());
    return styledBand(source, request.colormap, chosen);
}

// One band with a colormap and no formula: the RGBA picture the colormap makes of the band, with the colours of
// the formula export and of the map tiles (planBandStyle: over the rescale, else over the default stretch of the
// band from the statistics of the whole raster, not of the window, in the physical values of the band). Pixels
// with no data are transparent. Block-windowed as the formula export.
void exportStyledBand(const RasterExport &request, int band, ExportSession &session, const ExportSource &src)
{
    const RasterSource &source = src.source();
    const SourceWindow part = sourceWindowOf(request, source.width(), source.height());
    const BandStylePlan style =
        planBandStyle(request.input, source, band, request.colormap, request.rescale, request.nodata);
    const RasterBandNodata nodata = detectBandNodata(source, source.bandCount());

    FormulaBuffers buffers;
    writeRgba(
        request, src, part, computeExportTileSize(source, 2, request.tileSize),
        [&](const RasterWindow &window) -> const std::vector<float> & {
            computeBandValues(source, nodata, style.band, window, buffers);
            return buffers.result;
        },
        *style.colormap, style.range, session);
}

// Bands mode (no formula, and not one band with a colormap): band selection + rescale, export as GeoTIFF
void exportBands(const RasterExport &request, ExportSession &session, GDALDatasetUniquePtr &srcGuard)
{
    GDALDatasetH hSrcDs = datasetHandle(srcGuard);
    const char *outputPath = request.output.c_str();
    const std::string &rescaleStr = request.rescale;
    const SourceWindow part = sourceWindowOf(request, GDALGetRasterXSize(hSrcDs), GDALGetRasterYSize(hSrcDs));

    const std::vector<int> selectedBands = selectBands(request, GDALGetRasterCount(hSrcDs));

    // Use GDALTranslate for band selection + rescale. Its progress goes to the
    // callback, and it stops when the export is canceled.
    session.checkpoint(0.0, "converting");
    CPLStringList targs;
    targs.AddString("-ot");
    targs.AddString("Byte");

    if (request.bbox)
    {
        targs.AddString("-srcwin");
        for (const int v : {part.x, part.y, part.width, part.height})
            targs.AddString(std::to_string(v).c_str());
    }

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

    closeOutputOrThrow(std::move(out), outputPath);
    // Reopen in update mode to add internal overviews (efficient zoomed
    // reads). Best-effort: a failure here is non-fatal.
    if (const auto ovr = raster::reopenWrittenRaster(io::fromUtf8(outputPath), raster::ArtefactAccess::Update))
    {
        buildInternalOverviews(datasetHandle(ovr), part.width, part.height);
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

    const ExportSource src(request);
    if (!request.formula.empty())
    {
        exportFormula(request, session, src);
        return;
    }
    if (const int band = styledBandOf(request, src.source()); band > 0)
    {
        exportStyledBand(request, band, session, src);
        return;
    }

    // Closed on any return or throw
    auto source = raster::openInputRaster(request.input);
    if (!source)
        throw GDALException("Cannot open " + request.input);

    // With a NoData override the export reads a view of the raster that has it (it is closed before the
    // raster, which it reads)
    GDALDatasetUniquePtr view;
    if (request.nodata)
        view = withNodataOverride(datasetHandle(source), *request.nodata);
    exportBands(request, session, view ? view : source);
}

} // namespace ddb
