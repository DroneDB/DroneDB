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
#include "json.h"
#include "logger.h"
#include "mio.h"
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

// Fills a tile of an export: its window (x, y, width, height) in the output, its samples written to `out`, rows
// `stride` bytes apart
using TileFiller = std::function<void(int x, int y, int width, int height, uint8_t *out, size_t stride)>;

// The tiles of an export as ddbtWriteRaster asks for them: the filler, with the progress and the cancellation of
// the export; an exception of the filler stops the write and is thrown again after it
struct TileFill
{
    const TileFiller &filler;
    ExportSession &session;
    size_t total;
    size_t done = 0;
    std::exception_ptr error;

    TileFill(const TileFiller &tileFiller, ExportSession &exportSession, size_t tiles)
        : filler(tileFiller), session(exportSession), total(tiles)
    {
    }

    static int fill(void *userData, int x, int y, int width, int height, uint8_t *out, size_t stride)
    {
        auto &f = *static_cast<TileFill *>(userData);
        try
        {
            f.session.checkpoint(static_cast<double>(f.done) / static_cast<double>(f.total), "rendering");
            f.filler(x, y, width, height, out, stride);
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

// What an export writes besides its pixels
struct ExportLayout
{
    std::vector<std::string> colorInterp;  // one per band
    std::vector<std::string> descriptions; // one per band ("" for none), or none
    std::optional<double> nodata;
    bool mask = false;                     // an internal mask, filled after the bands of each pixel
    int predictor = 2;                     // the horizontal differencing of DEFLATE (1: none)
};

// Writes the export of the part of the source: the tiles the filler computes, a tiled DEFLATE GeoTIFF with the
// georeferencing of the source (the corner of the part) and its average overviews (ddb-tilers: ddbtWriteRaster).
// The output appears whole or not at all.
void writeExport(const RasterExport &request, const ExportSource &src, const SourceWindow &part, int tileSize,
                 const ExportLayout &layout, const TileFiller &filler, ExportSession &session)
{
    json options = {{"width", part.width},
                    {"height", part.height},
                    {"colorInterp", layout.colorInterp},
                    {"blockSize", tileSize},
                    {"codec", "deflate"},
                    {"predictor", layout.predictor},
                    {"overviewFactors", overviewFactorsOf(part.width, part.height)}};
    if (layout.nodata)
        options["nodata"] = std::isnan(*layout.nodata) ? json("nan") : json(*layout.nodata);
    if (!layout.descriptions.empty())
        options["descriptions"] = layout.descriptions;
    if (layout.mask)
        options["mask"] = true;
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

    TileFill fill(filler, session,
                  static_cast<size_t>((part.width + tileSize - 1) / tileSize) *
                      static_cast<size_t>((part.height + tileSize - 1) / tileSize));
    session.checkpoint(0.0, "rendering");
    const ddbtErr rc = ddbtWriteRaster(request.output.c_str(), options.dump().c_str(), &TileFill::fill, &fill,
                                       nullptr, nullptr);
    if (fill.error)
        std::rethrow_exception(fill.error);
    tilers::check(rc);
    session.report(1.0, "rendering");
}

// Writes the RGBA export of the part of the source: the values of each tile coloured by the colormap over the range
void writeRgba(const RasterExport &request, const ExportSource &src, const SourceWindow &part, int tileSize,
               const WindowValues &values, const Colormap &colormap, const std::pair<float, float> &range,
               ExportSession &session)
{
    std::vector<uint8_t> rgba;
    const TileFiller filler = [&](int x, int y, int width, int height, uint8_t *out, size_t stride) {
        const RasterWindow window{part.x + x, part.y + y, width, height, width, height};
        const std::vector<float> &windowValues = values(window);
        const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
        rgba.resize(pixels * 4);
        VegetationEngine::instance().applyColormap(windowValues.data(), rgba.data(), pixels, colormap,
                                                   {range.first, range.second, NODATA_SENTINEL});
        const size_t rowBytes = static_cast<size_t>(width) * 4;
        for (int row = 0; row < height; row++)
            std::memcpy(out + static_cast<size_t>(row) * stride, rgba.data() + static_cast<size_t>(row) * rowBytes,
                        rowBytes);
    };
    writeExport(request, src, part, tileSize, ExportLayout{{"Red", "Green", "Blue", "Alpha"}, {}, std::nullopt, false, 2},
                filler, session);
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

// How one band of a bands export becomes 8-bit, as gdal_translate -ot Byte -scale makes it (a VRTComplexSource with
// linear scaling and the NoData value of the band): its range (the rescale, else approximateMinMax), the working type
// of its samples (double for 32 and 64-bit integers, else float), and the value of the pixels it leaves out
struct ByteBand
{
    int band = 0;
    bool scaled = false;  // a source that is not Byte; a Byte one is copied as it is
    bool linear = false;  // the scaling changes the samples
    double ratio = 1.0, offset = 0.0;
    bool doubleWork = false;
    bool nodataNan = false, nodataValue = false;
    double nodata = 0;
    uint8_t fill = 0;     // the pixels left out: the NoData value of the output as Byte (GDALCopyWords)

    // ARE_REAL_EQUAL of GDAL, in the working type
    template <typename W> static bool equal(W a, W b)
    {
        return a == b || std::abs(a - b) < static_cast<W>(std::numeric_limits<float>::epsilon()) * std::abs(a + b) * 2;
    }

    template <typename W> uint8_t toByte(W v) const
    {
        if (nodataNan && std::isnan(v))
            return fill;
        if (nodataValue && equal(v, static_cast<W>(nodata)))
            return fill;
        if (linear)
            v = static_cast<W>(v * ratio + offset);
        return static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, static_cast<float>(v) + 0.5f)));
    }
};

// The range of a "min,max" rescale (gdal_translate -scale min max 0 255)
std::pair<double, double> rescaleRangeOf(const std::string &rescale)
{
    const size_t comma = rescale.find(',');
    if (comma == std::string::npos)
        return {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN()};
    return {CPLAtof(rescale.substr(0, comma).c_str()), CPLAtof(rescale.substr(comma + 1).c_str())};
}

// Bands mode (no formula, and not one band with a colormap): the selected bands as 8-bit, as gdal_translate -ot Byte
// [-srcwin] -b ... [-scale] writes them: a Byte source copied; any other scaled from its range (the rescale, else the
// approximate minimum and maximum of each band over the whole raster) to 0..255, its NoData pixels left at the NoData
// value of the source as Byte (which the output keeps, even one Byte cannot hold); the internal mask of the source kept
void exportBands(const RasterExport &request, ExportSession &session, const ExportSource &src)
{
    const RasterSource &source = src.source();
    const SourceWindow part = sourceWindowOf(request, source.width(), source.height());
    const std::vector<int> selected = selectBands(request, source.bandCount());
    const bool scale = source.dataType(1) != GDT_Byte;
    const std::pair<double, double> rescale = rescaleRangeOf(request.rescale);

    ExportLayout layout;
    layout.predictor = 1;  // gdal_translate's GTiff options: DEFLATE without a predictor
    std::vector<ByteBand> plans;
    for (const int b : selected)
    {
        ByteBand plan;
        plan.band = b;
        plan.scaled = scale;
        const GDALDataType type = source.dataType(b);
        plan.doubleWork = type == GDT_Int32 || type == GDT_UInt32 || type == GDT_Int64 || type == GDT_UInt64;
        // The NoData value of the source is the output's (GTiff keeps one Byte cannot hold); the pixels
        // left out take it as Byte (GDALCopyWords: rounded, clamped, NaN 0)
        const BandNodata nd = source.nodata(b);
        if (nd.has && !std::isnan(nd.value))
            plan.fill = static_cast<uint8_t>(std::clamp(std::floor(nd.value + 0.5), 0.0, 255.0));
        if (b == selected.front() && nd.has)
            layout.nodata = nd.value;
        if (scale)
        {
            plan.nodataNan = nd.has && std::isnan(nd.value);
            plan.nodataValue = nd.has && !std::isnan(nd.value) &&
                               (plan.doubleWork || std::fabs(nd.value) <= std::numeric_limits<float>::max());
            plan.nodata = nd.value;
            auto [lo, hi] = std::isnan(rescale.first) ? approximateMinMax(source, b) : rescale;
            // To avoid a divide by zero
            if (hi == lo)
                hi += 0.1;
            if (hi == lo)
                throw GDALException("-scale cannot be applied due to source minimum and maximum being equal");
            plan.ratio = 255.0 / (hi - lo);
            plan.offset = -1 * lo * plan.ratio + 0.0;
            plan.linear = plan.offset != 0.0 || plan.ratio != 1.0;
        }
        // gdal_translate copies the colour interpretation and the description of each band
        layout.colorInterp.push_back(source.colorInterp(b));
        layout.descriptions.push_back(source.description(b));
        plans.push_back(plan);
    }
    // gdal_translate's MASK_AUTO: a source whose first band has a dataset mask (not alpha, not NoData) keeps it
    layout.mask = detectBandNodata(source, source.bandCount()).datasetMask;

    const int bandCount = static_cast<int>(plans.size());
    const int samples = bandCount + (layout.mask ? 1 : 0);
    std::vector<float> floats;
    std::vector<double> doubles;
    std::vector<uint8_t> mask;
    const TileFiller filler = [&](int x, int y, int width, int height, uint8_t *out, size_t stride) {
        const RasterWindow window{part.x + x, part.y + y, width, height, width, height};
        const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
        for (int i = 0; i < bandCount; i++)
        {
            const ByteBand &plan = plans[static_cast<size_t>(i)];
            const bool withMask = layout.mask && i == 0;
            if (withMask)
                mask.resize(pixels);
            if (plan.doubleWork)
            {
                doubles.resize(pixels);
                source.read(0, window, {plan.band}, doubles.data(), withMask ? mask.data() : nullptr);
            }
            else
            {
                floats.resize(pixels);
                source.read(window, {plan.band}, floats.data(), withMask ? mask.data() : nullptr);
            }
            for (int row = 0; row < height; row++)
            {
                uint8_t *line = out + static_cast<size_t>(row) * stride;
                for (int col = 0; col < width; col++)
                {
                    const size_t k = static_cast<size_t>(row) * static_cast<size_t>(width) + static_cast<size_t>(col);
                    uint8_t v;
                    if (!plan.scaled)
                        v = static_cast<uint8_t>(floats[k]);  // a Byte sample
                    else if (plan.doubleWork)
                        v = plan.toByte(doubles[k]);
                    else
                        v = plan.toByte(floats[k]);
                    line[static_cast<size_t>(col) * samples + i] = v;
                }
            }
        }
        if (layout.mask)
            for (int row = 0; row < height; row++)
                for (int col = 0; col < width; col++)
                    out[static_cast<size_t>(row) * stride + static_cast<size_t>(col) * samples + bandCount] =
                        mask[static_cast<size_t>(row) * static_cast<size_t>(width) + static_cast<size_t>(col)];
    };
    // gdal_translate's GTiff tiles: 256, whatever tile the request asks for
    writeExport(request, src, part, 256, layout, filler, session);
}

} // namespace

void exportRaster(const RasterExport &request)
{
    // Creating the output truncates an existing file, so it must not be the input
    io::assureDistinctOutput(io::fromUtf8(request.input), io::fromUtf8(request.output));

    ExportSession session(request.progress);
    const ExportSource src(request);
    if (!request.formula.empty())
        exportFormula(request, session, src);
    else if (const int band = styledBandOf(request, src.source()); band > 0)
        exportStyledBand(request, band, session, src);
    else
        exportBands(request, session, src);
}

} // namespace ddb
