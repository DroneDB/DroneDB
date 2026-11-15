/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "cancellation.h"
#include "stringutil.h"
#include "coordstransformer.h"
#include "gdal_raii.h"
#include "raster_utils.h"
#include "gdal_priv.h"
#include "cpl_conv.h"
#include "cpl_vsi.h"
#include "ogr_spatialref.h"

#include "raster_region.h"
#include "vegetation.h"
#include "vis_render.h"
#include "tile_encoder.h"
#include "tilers_raster.h"
#include "exceptions.h"
#include "logger.h"
#include "json.h"
#include "mio.h"
#include "raster_source.h"

namespace ddb
{

    namespace
    {

        // What a request asks for: a GeoTIFF (written by gdalwarp), the RGBA pixels themselves
        // (kRgbaPixelsFormat) or an image encoded as the tiles are
        struct FormatInfo
        {
            enum class Kind
            {
                GeoTiff,
                Pixels,
                Image
            };
            Kind kind;
            ImageFormat image;     // the format of Kind::Image
            bool wantsAlpha;       // append -dstalpha
            bool jpegCompositing;  // if true, composite on white before write

            bool pixels() const { return kind == Kind::Pixels; }
            bool geoTiff() const { return kind == Kind::GeoTiff; }
        };

        FormatInfo resolveFormat(const std::string &mime)
        {
            const std::string m = mime.empty() ? "image/png" : mime;
            if (m == "image/tiff" || m == "image/geotiff" || m == "tiff" || m == "tif")
                return {FormatInfo::Kind::GeoTiff, ImageFormat::Png, true, false};
            if (m == kRgbaPixelsFormat)
                return {FormatInfo::Kind::Pixels, ImageFormat::Png, true, false};

            // Renders are map previews (WMS GetMap): the same encoding as tiles
            ImageFormat format;
            try
            {
                format = parseImageFormat(m);
            }
            catch (const InvalidArgsException &)
            {
                throw InvalidArgsException("Unsupported format: " + m);
            }
            return {FormatInfo::Kind::Image, format, format != ImageFormat::Jpeg, format == ImageFormat::Jpeg};
        }

        // The format of a request, with its background: a GeoTIFF is data and has none
        FormatInfo requestFormat(const RegionRequest &request)
        {
            FormatInfo fi = resolveFormat(request.format);
            if (request.background && fi.geoTiff())
                throw InvalidArgsException("A background is for an image (PNG, JPEG, WebP or rgba), not for a GeoTIFF");
            return fi;
        }

        // Hands the bytes of a written render file to the caller (VSIFree)
        void takeFileBytes(const fs::path &file, uint8_t **outBytes, int *outSize, const std::string &label)
        {
            std::error_code ec;
            const auto size = fs::file_size(file, ec);
            if (ec || size == 0 || size > static_cast<uintmax_t>(INT32_MAX))
                throw GDALException("Empty render output for " + label);
            VsiFreePtr<GByte> bytes(static_cast<GByte *>(VSIMalloc(static_cast<size_t>(size))));
            if (!bytes)
                throw std::bad_alloc();
            std::ifstream in(file, std::ios::binary);
            if (!in.read(reinterpret_cast<char *>(bytes.get()), static_cast<std::streamsize>(size)))
                throw GDALException("Cannot read the render of " + label);
            *outBytes = reinterpret_cast<uint8_t *>(bytes.release());
            *outSize = static_cast<int>(size);
        }

        // The colour bands (1: grey, 3: RGB) and the alpha band (the last one; 0 when there is none) of the
        // 8-bit image of a render
        struct ImageBands
        {
            int color = 0;
            int alpha = 0;
        };

        ImageBands imageBands(const tilers::WarpedImage &image)
        {
            const int count = image.bandCount();
            ImageBands bands;
            if (count > 1 && image.colorInterp.back() == "Alpha")
                bands.alpha = count;
            bands.color = count - (bands.alpha > 0 ? 1 : 0);
            return bands;
        }

        // Fails unless the image of a render can be encoded: 8-bit, 1 (grey) or 3 (RGB) colour bands
        void requireImage(const tilers::WarpedImage &image, const ImageBands &bands, const std::string &label)
        {
            if (bands.color != 1 && bands.color != 3)
                throw InvalidArgsException("The render of " + label + " has " + std::to_string(bands.color) +
                                           " colour bands: an image has 1 (grey) or 3 (RGB)");
            if (image.dataType != "Byte")
                throw InvalidArgsException("The render of " + label + " is not 8-bit: an image holds bytes");
        }

        // The colours of the colour table of a single colour band (RGBA, at most 256), empty when the
        // image has none
        std::vector<std::array<uint8_t, 4>> colorTable(const tilers::WarpedImage &image, const ImageBands &bands)
        {
            if (bands.color != 1)
                return {};
            std::vector<std::array<uint8_t, 4>> colors = image.colorTable;
            if (colors.size() > 256)
                colors.resize(256);
            return colors;
        }

        // The bands `map` (1-based) of the 8-bit image interleaved into a new buffer of GDAL (VSIFree)
        VsiFreePtr<GByte> readInterleaved(const tilers::WarpedImage &image, const std::vector<int> &map)
        {
            const size_t pixels = static_cast<size_t>(image.width) * image.height;
            const size_t channels = map.size();
            VsiFreePtr<GByte> out(static_cast<GByte *>(VSIMalloc2(pixels, channels)));
            if (!out)
                throw std::bad_alloc();
            for (size_t c = 0; c < channels; c++)
            {
                const uint8_t *in = image.band(map[c]);
                GByte *o = out.get() + c;
                for (size_t i = 0; i < pixels; i++, o += channels)
                    *o = in[i];
            }
            return out;
        }

        // The pixels of the 8-bit image of a render as kRgbaPixelsFormat, in a buffer of GDAL (VSIFree):
        // what a PNG of the image decodes to (the grey in red, green and blue, alpha 255 without an alpha
        // band, the colours of a single band with a colour table, their alpha times that of the alpha
        // band when there is one; an index past the table is transparent black)
        VsiFreePtr<GByte> readRgba(const tilers::WarpedImage &image, const ImageBands &bands, const std::string &label)
        {
            requireImage(image, bands, label);
            std::vector<int> map = bands.color == 3 ? std::vector<int>{1, 2, 3} : std::vector<int>{1, 1, 1};
            map.push_back(bands.alpha > 0 ? bands.alpha : 1);
            VsiFreePtr<GByte> rgba = readInterleaved(image, map);

            const size_t pixels = static_cast<size_t>(image.width) * image.height;
            GByte *p = rgba.get();
            const auto colors = colorTable(image, bands);
            if (!colors.empty())
            {
                std::array<std::array<GByte, 4>, 256> lookup{};
                std::copy(colors.begin(), colors.end(), lookup.begin());
                for (size_t i = 0; i < pixels; i++, p += 4)
                {
                    const unsigned alpha = p[3];
                    std::memcpy(p, lookup[p[0]].data(), 4);
                    if (bands.alpha > 0)
                        p[3] = static_cast<GByte>((p[3] * alpha + 127) / 255);
                }
            }
            else if (bands.alpha == 0)
                for (size_t i = 0; i < pixels; i++)
                    p[i * 4 + 3] = 255;
            return rgba;
        }

        // RGBA pixels composited over an opaque colour, in place: alpha 255 after it, with the rounding of
        // RegionRequest::background (premultiplied with an exact rounding, the background added with
        // (v + 255) >> 8), which never exceeds 255
        void compositeOver(GByte *rgba, size_t pixels, const RgbColor &background)
        {
            const std::array<unsigned, 3> under = {background.r, background.g, background.b};
            for (size_t i = 0; i < pixels; i++, rgba += 4)
            {
                const unsigned a = rgba[3];
                if (a == 255)
                    continue;
                for (int c = 0; c < 3; c++)
                    rgba[c] = static_cast<GByte>((rgba[c] * a + 127) / 255 + ((under[c] * (255 - a) + 255) >> 8));
                rgba[3] = 255;
            }
        }

        // The colour a render with transparency is composited over: the background of the request, else
        // white for a format without alpha (JPEG), else none
        std::optional<RgbColor> imageBackground(const FormatInfo &fi, const RegionRequest &request, bool hasAlpha)
        {
            if (request.background)
                return request.background;
            if (!fi.wantsAlpha && hasAlpha)
                return RgbColor{255, 255, 255};
            return std::nullopt;
        }

        // Writes the RGBA pixels of a render in the format of the request: composited over `background`
        // when it is set, then given as they are for kRgbaPixelsFormat, else encoded (an alpha that is
        // 255 everywhere is not written)
        void writeRgba(VsiFreePtr<GByte> rgba, int width, int height, const std::optional<RgbColor> &background,
                       const FormatInfo &fi, uint8_t **outBytes, int *outSize)
        {
            const size_t pixels = static_cast<size_t>(width) * height;
            if (background)
                compositeOver(rgba.get(), pixels, *background);
            if (fi.pixels())
            {
                *outBytes = rgba.release();
                *outSize = static_cast<int>(pixels * 4);
                return;
            }
            TileEncoder::encode(InterleavedImage{rgba.get(), width, height, 4}, {fi.image}, outBytes, outSize);
        }

        // Writes the image of a render (8-bit: grey or RGB, then its alpha band if it has one) in the
        // format of the request: encoded as it is (grey stays grey, a colour table is the palette of a
        // PNG), or as its RGBA pixels when the request wants them, a background is composited under it
        // or a colour table cannot be a palette (JPEG, WebP, an alpha band). JPEG has no alpha: an image with alpha is
        // composited over white when the request has no background. The image is released before the
        // encode.
        void writeImage(tilers::WarpedImage image, const FormatInfo &fi, const RegionRequest &request,
                        uint8_t **outBytes, int *outSize)
        {
            const ImageBands bands = imageBands(image);
            requireImage(image, bands, request.inputPath);
            const std::optional<RgbColor> background = imageBackground(fi, request, bands.alpha > 0);
            const int width = image.width, height = image.height;
            auto palette = colorTable(image, bands);

            // A colour table is kept as the palette of a PNG without an alpha band; elsewhere its colours
            // are written (JPEG and WebP have no palette, and a palette has no alpha band beside it)
            if (!background && !fi.pixels() && (palette.empty() || (fi.image == ImageFormat::Png && bands.alpha == 0)))
            {
                std::vector<int> map;
                for (int b = 1; b <= bands.color; b++)
                    map.push_back(b);
                if (bands.alpha > 0)
                    map.push_back(bands.alpha);
                const VsiFreePtr<GByte> pixels = readInterleaved(image, map);
                image = {};
                // An index past the table is transparent black, as in the RGBA pixels (readRgba)
                if (!palette.empty())
                {
                    const size_t count = static_cast<size_t>(width) * height;
                    const GByte top = *std::max_element(pixels.get(), pixels.get() + count);
                    if (top >= palette.size())
                        palette.resize(static_cast<size_t>(top) + 1, {0, 0, 0, 0});
                }
                TileEncoder::encode(InterleavedImage{pixels.get(), width, height, static_cast<int>(map.size()),
                                                     palette.empty() ? nullptr : &palette},
                                    {fi.image}, outBytes, outSize);
                return;
            }

            VsiFreePtr<GByte> rgba = readRgba(image, bands, request.inputPath);
            image = {};
            writeRgba(std::move(rgba), width, height, background, fi, outBytes, outSize);
        }

        // Fails unless every requested band exists in the source (none requested: all do)
        void requireBandsInRange(const tilers::Raster &raster, const std::vector<int> &bands)
        {
            const int srcBands = raster.bandCount();
            for (int b : bands)
            {
                if (b < 1 || b > srcBands) {
                    throw InvalidArgsException(
                        "renderRasterRegion: band " + std::to_string(b) +
                        " out of range [1," + std::to_string(srcBands) + "]");
                }
            }
        }

        // What gdalwarp writes where the raster has no data: nothing (the bands as they are), an
        // alpha band, or white in every band (JPEG has no alpha)
        enum class WarpBackground
        {
            None,
            Alpha,
            White
        };

        // The warp of a region render: the window (bbox in srs, size in pixels) in the CRS tSrs, the
        // bands (none: all of them) and the background, as gdalwarp makes it
        tilers::WarpRequest regionWarp(const RegionRequest &request, const tilers::Raster &raster,
                                       const std::vector<int> &bands, WarpBackground background,
                                       const std::string &srs, const std::string &tSrs)
        {
            tilers::WarpRequest warp;
            warp.targetCrs = tSrs;
            warp.extent = request.bbox;
            warp.extentCrs = srs;
            warp.width = request.width;
            warp.height = request.height;
            // The indices of a colour table are classes, not values: they are not interpolated
            const int firstBand = bands.empty() ? 1 : bands.front();
            warp.nearest = firstBand == 1 && !raster.band(1).colorTable.empty();
            // The warp still reads the alpha band of the raster to know where it has no data
            warp.bands = bands;
            warp.dstAlpha = background == WarpBackground::Alpha;
            if (background == WarpBackground::White)
                warp.dstNodata = 255.0;
            return warp;
        }

        // The 8-bit image of a warped render whose bands are not 8-bit: every band but the last
        // (the alpha band of the warp) stretched from its range to 0-255, and the alpha band, or for an
        // image without alpha (JPEG), white where the alpha is 0
        tilers::WarpedImage stretchedImage(const tilers::WarpedImage &warped,
                                           const std::vector<std::pair<double, double>> &ranges, bool white)
        {
            const int colorBands = warped.bandCount() - 1;
            const size_t pixels = static_cast<size_t>(warped.width) * warped.height;
            tilers::WarpedImage image;
            image.width = warped.width;
            image.height = warped.height;
            image.dataType = "Byte";
            image.geoTransform = warped.geoTransform;
            image.pixels.resize(pixels * static_cast<size_t>(colorBands + (white ? 0 : 1)));
            std::vector<float> alpha, values;
            warped.readBand(colorBands + 1, alpha);
            for (int b = 1; b <= colorBands; b++)
            {
                warped.readBand(b, values);
                uint8_t *bytes = image.pixels.data() + static_cast<size_t>(b - 1) * pixels;
                const auto &range = ranges[std::min(ranges.size() - 1, static_cast<size_t>(b - 1))];
                for (size_t i = 0; i < pixels; i++)
                    bytes[i] = white && alpha[i] <= 0.f ? 255 : stretchToByte(values[i], range.first, range.second);
                static const char *const rgb[3] = {"Red", "Green", "Blue"};
                image.colorInterp.emplace_back(colorBands < 3 ? "Gray" : rgb[b - 1]);
            }
            if (!white)
            {
                uint8_t *bytes = image.pixels.data() + static_cast<size_t>(colorBands) * pixels;
                for (size_t i = 0; i < pixels; i++)
                    bytes[i] = static_cast<uint8_t>(std::clamp(alpha[i], 0.f, 255.f));
                image.colorInterp.emplace_back("Alpha");
                image.alphaBand = colorBands + 1;
            }
            return image;
        }

        // The value of the pixel in every band, with the band's nodata value when it has one: null
        // for a pixel outside the raster, one that cannot be read and one that is nodata
        // The value of every band at a pixel: null outside the raster, for NoData and for a read
        // that fails
        json pixelBandValues(const tilers::Raster &raster, int px, int py)
        {
            const bool inBounds = (px >= 0 && py >= 0 && px < raster.width() && py < raster.height());

            json bands = json::array();
            for (int b = 1; b <= raster.bandCount(); b++) {
                json bj;
                BandNodata nodata;
                if (const auto value = raster.band(b).nodata)
                    nodata = {true, *value};
                if (nodata.has) bj["nodata"] = nodata.value;

                if (!inBounds) {
                    bj["value"] = nullptr;
                } else {
                    double val = 0.0;
                    try {
                        raster.read(px, py, 1, 1, 1, 1, std::span<const int>(&b, 1), &val);
                        if (isNodata(val, nodata))
                            bj["value"] = nullptr;
                        else
                            bj["value"] = val;
                    } catch (const AppException &e) {
                        LOGD << "Cannot read band " << b << " at " << px << "," << py << ": " << e.what();
                        bj["value"] = nullptr;
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
            // gdalwarp reads it (-t_srs, -te_srs)
            if (!request.bboxSrs.empty())
                requireSrsUserInput(request.bboxSrs, "SRS");
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
        // gdalwarp reads it (-t_srs)
        if (!options.outputCrs.empty())
            requireSrsUserInput(options.outputCrs, "output CRS");

        const std::string &inputPath = request.inputPath;

        const FormatInfo fi = requestFormat(request);
        const std::string srs = request.bboxSrs.empty() ? std::string("EPSG:4326") : request.bboxSrs;
        // outputCrs empty → keep current behaviour (target SRS = bbox SRS).
        const std::string tSrs = options.outputCrs.empty() ? srs : options.outputCrs;

        // The file libddb reads for the raster (the COG of a build, else its normalized copy)
        const auto raster = tilers::Raster::openInput(inputPath);
        if (!raster)
            throw GDALException("Cannot open raster: " + inputPath);
        requireBandsInRange(*raster, options.bands);

        const bool directOutput = fi.geoTiff();

        // An image (PNG, JPEG, WebP) holds 8-bit grey or RGB: unless the caller pinned the bands
        // (WCS RangeSubset semantics: exactly those, with no alpha), it shows the raster as the
        // map tiles do, its first three data bands (or its first) stretched to 8 bits when they
        // are not. A multispectral or a DEM raster as it is (6 bands, Float32) cannot be encoded.
        std::vector<int> bands = options.bands;
        std::vector<std::pair<double, double>> stretch;
        if (!directOutput && bands.empty())
        {
            const auto source = rasterSourceOf(*raster);
            bands = defaultDisplayBands(*source);
            if (raster->dataType() != "Byte")
                stretch = defaultStretch(inputPath, *source, bands);
        }
        WarpBackground background = WarpBackground::None;
        if (!stretch.empty() || request.background)
            background = WarpBackground::Alpha; // the stretch makes JPEG's white from it, a background its image
        else if (fi.jpegCompositing)
            background = WarpBackground::White;
        else if (fi.wantsAlpha && options.bands.empty())
            background = WarpBackground::Alpha;

        cancel::throwIfCanceled();
        tilers::WarpedImage warped =
            tilers::warp(raster->file(), regionWarp(request, *raster, bands, background, srs, tSrs));

        if (!directOutput) {
            if (!stretch.empty())
                warped = stretchedImage(warped, stretch, fi.jpegCompositing && !request.background);
            writeImage(std::move(warped), fi, request, outBytes, outSize);
            return;
        }
        // gdalwarp's GeoTIFF names each warped band as its source band, and the alpha band alpha
        for (int b = 1; b <= warped.bandCount(); b++)
            if (b != warped.alphaBand)
                warped.colorInterp[static_cast<size_t>(b - 1)] =
                    raster->band(bands.empty() ? b : bands[static_cast<size_t>(b - 1)]).colorInterp;
        // The GeoTIFF of the warp, written whole in a private folder, then handed over
        const io::ScopedTempDir folder = io::ScopedTempDir::create(fs::temp_directory_path(), "ddb-render-");
        const fs::path out = folder.path() / "render.tif";
        tilers::writeGeoTiff(warped, tSrs, out);
        warped = {};
        takeFileBytes(out, outBytes, outSize, inputPath);
    }

    // ---------------------------------------------------------------------

    std::string queryRasterPoint(const std::string &inputPath,
                                 double x, double y,
                                 const std::string &srs)
    {
        if (inputPath.empty())
            throw InvalidArgsException("queryRasterPoint: input path is empty");

        const std::string requestedSrs = srs.empty() ? std::string("EPSG:4326") : srs;

        const auto raster = tilers::Raster::openInput(inputPath);
        if (!raster)
            throw GDALException("Cannot open raster: " + inputPath);

        if (!raster->geoTransform()) {
            throw GDALException("Raster has no geotransform: " + inputPath);
        }
        const std::array<double, 6> gt = *raster->geoTransform();

        // Source SRS
        if (raster->wkt().empty()) {
            throw GDALException("Raster has no projection: " + inputPath);
        }

        const SrsPtr hDsSrs = srsFromWkt(raster->wkt().c_str());
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

        // The one geo-to-pixel conversion (raster_utils.h): floor, so that points slightly outside
        // the raster on the negative side map to negative pixel coordinates and get reported as
        // out-of-bounds, rather than being silently clamped to the edge pixel.
        int px = 0, py = 0;
        GeoPixelMap(gt.data()).toPixelIndex(dsX, dsY, px, py);

        json out;
        out["lon"] = lon;
        out["lat"] = lat;
        out["pixel"] = {px, py};
        out["bands"] = pixelBandValues(*raster, px, py);
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
            IndexLayerPlanner(const std::string &rasterPath, const RasterSource &source)
                : detected_(VegetationEngine::instance().autoDetectFilter(rasterPath)), data_(rasterDataKind(source)) {}

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
        void restoreDilutedBands(tilers::WarpedImage &warped, const std::vector<int> &bands,
                                 const RasterBandNodata &sourceNodata, const std::vector<float> &coverage)
        {
            for (const int band : bands)
            {
                if (sourceNodata.bands[band].has)
                    continue;
                // The warp is Float32: its samples in place
                float *plane = reinterpret_cast<float *>(const_cast<uint8_t *>(warped.band(band + 1)));
                for (size_t i = 0; i < coverage.size(); ++i)
                    if (coverage[i] > 0.0f && coverage[i] < 1.0f && std::isfinite(plane[i]))
                        plane[i] /= coverage[i];
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

        SourceTransparency sourceTransparency(const RasterSource &source)
        {
            SourceTransparency transparency;
            transparency.nodata = detectBandNodata(source, source.bandCount());
            transparency.hasAlpha = transparency.nodata.alphaBandIdx >= 0;
            const bool hasNodata = std::any_of(transparency.nodata.bands.begin(), transparency.nodata.bands.end(),
                                               [](const BandNodata &nodata) { return nodata.has; });
            transparency.warperAlpha = !transparency.hasAlpha && !hasNodata;
            return transparency;
        }

        // The bands of the source that the index layer warps: those its formula reads, then the alpha band
        // if the raster has one (0-based, in the order of the warp). The others are not read.
        class IndexWarpBands
        {
            std::vector<int> source_;

            // The warped band of a band of the source (0-based), -1 when it is not warped
            int warped(int sourceBand) const
            {
                const auto it = std::find(source_.begin(), source_.end(), sourceBand);
                return it == source_.end() ? -1 : static_cast<int>(it - source_.begin());
            }

        public:
            IndexWarpBands(const IndexChoice &choice, const SourceTransparency &transparency)
                : source_(VegetationEngine::bandsRead(*choice.formula, choice.filter))
            {
                if (transparency.hasAlpha)
                    source_.push_back(transparency.nodata.alphaBandIdx);
            }

            const std::vector<int> &source() const { return source_; }

            // The band filter with the bands of the warp
            BandFilter filter(const BandFilter &sourceFilter) const
            {
                BandFilter filter = sourceFilter;
                for (int *band : {&filter.R, &filter.G, &filter.B, &filter.N, &filter.Re, &filter.T})
                    *band = *band < 0 ? -1 : warped(*band);
                return filter;
            }

            // The nodata of the bands of the warp, and its alpha band
            RasterBandNodata nodata(const RasterBandNodata &sourceNodata) const
            {
                RasterBandNodata nodata = sourceNodata;
                nodata.bands.clear();
                for (const int band : source_)
                    nodata.bands.push_back(sourceNodata.bands[band]);
                nodata.alphaBandIdx = sourceNodata.alphaBandIdx < 0 ? -1 : warped(sourceNodata.alphaBandIdx);
                return nodata;
            }
        };

        // The warp of the window of the index layer: the bands of `bands` as Float32, into memory, as gdalwarp
        // -ot Float32 -nosrcalpha -setci makes it. The alpha band, if there is one, is warped like the others, so
        // the engine reads the source's own 0 (no data) and not what the warper makes of it: with a source alpha
        // the warper rescales it by the maximum of the band's type (65535 for UInt16, where ODM writes 255), which
        // turned a fully opaque raster into alpha 0, and it turns the pixels that are nodata in a band into 0
        // instead of leaving them nodata. (And it takes the alpha for a validity of 0 or 1: the edge of the alpha
        // would not be anti-aliased.) The warped alpha is then the coverage of the pixel, which is what the output
        // alpha is made from below. The bands keep their names (-setci), so the warp keeps naming its alpha band;
        // the warper's own alpha (-dstalpha, DST_ALPHA_MAX 255: the default of a Float32 output) when the raster
        // says nothing of where it has no pixels.
        tilers::WarpRequest indexWarp(const RegionRequest &request, const std::string &srs, const IndexWarpBands &bands,
                                      bool warperAlpha)
        {
            tilers::WarpRequest warp;
            warp.targetCrs = srs;
            warp.extent = request.bbox;
            warp.extentCrs = srs;
            warp.width = request.width;
            warp.height = request.height;
            warp.outputType = "Float32";
            warp.noSrcAlpha = true;
            warp.setColorInterp = true;
            for (const int band : bands.source())
                warp.bands.push_back(band + 1);
            warp.dstAlpha = warperAlpha;
            return warp;
        }

        // The coverage of every pixel of the warped window, from its alpha band: 1 inside the
        // source's pixels, 0 outside, between them along an edge where the warp blended the two.
        // The alpha band of a raster is 255 in a Byte band and also in the UInt16 bands ODM writes:
        // the value that is opaque is derived from the band's data (kept with the dataset). Empty
        // when the raster says nothing of where it has no pixels (a nodata value alone leaves them
        // out of the formula).
        std::vector<float> warpedCoverage(const tilers::WarpedImage &warped, const RasterSource &source,
                                          const SourceTransparency &transparency, const RasterBandNodata &warpedNodata)
        {
            std::vector<float> coverage;
            if (!transparency.hasAlpha && !transparency.warperAlpha)
                return coverage;
            const int alphaBand = transparency.hasAlpha ? warpedNodata.alphaBandIdx + 1 : warped.bandCount();
            const double opaque = transparency.hasAlpha
                                      ? alphaOpaqueValue(source, transparency.nodata.alphaBandIdx + 1)
                                      : kWarperAlphaMax;
            warped.readBand(alphaBand, coverage);
            for (float &c : coverage)
                c = std::isfinite(c) ? static_cast<float>(std::clamp(c / opaque, 0.0, 1.0)) : 0.0f;
            return coverage;
        }
    } // anon

    std::vector<std::string> rasterIndexLayerIndices(const std::string &rasterPath)
    {
        const auto raster = tilers::Raster::openInput(rasterPath);
        if (!raster)
            throw GDALException("Cannot open raster: " + rasterPath);
        const IndexLayerPlanner planner(rasterPath, *rasterSourceOf(*raster));
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
        const FormatInfo fi = requestFormat(request);
        const std::string srs = request.bboxSrs.empty() ? std::string("EPSG:4326") : request.bboxSrs;

        // Step 1: which bands the index reads, over the source (the warp makes every band Float32,
        // so the type of the data is the source's), then warp them and the alpha band into memory at
        // the target window and resolution.
        const auto raster = tilers::Raster::openInput(inputPath);
        if (!raster)
            throw GDALException("Cannot open raster: " + inputPath);
        const auto source = rasterSourceOf(*raster);

        const IndexChoice choice = IndexLayerPlanner(inputPath, *source).choose(formulaId);
        if (!choice.refusal.empty())
            throw InvalidArgsException(choice.refusal);

        const SourceTransparency transparency = sourceTransparency(*source);
        const IndexWarpBands warpBands(choice, transparency);

        cancel::throwIfCanceled();
        tilers::WarpedImage warped =
            tilers::warp(raster->file(), indexWarp(request, srs, warpBands, transparency.warperAlpha));
        const size_t npx = static_cast<size_t>(width) * height;

        // Step 2: the coverage of every pixel, from the alpha band of the warp
        const RasterBandNodata warpedNodata = warpBands.nodata(transparency.nodata);
        const BandFilter filter = warpBands.filter(choice.filter);
        const std::vector<float> coverage = warpedCoverage(warped, *source, transparency, warpedNodata);
        if (transparency.hasAlpha)
            restoreDilutedBands(warped, VegetationEngine::bandsRead(*choice.formula, filter), warpedNodata, coverage);

        // Step 3: the shared engine reads the bands of the warped window as Float32, leaves the
        // nodata and transparent pixels out of the formula (they are rendered with alpha = 0
        // instead of being colorized as if they were valid samples) and colorizes the values
        // over the range of the formula; the alpha of the output is then the coverage.
        const auto warpedSource = rasterSourceOf(warped);
        FormulaPlan plan = planFormula(*warpedSource, formulaId, filter, "", false);
        plan.colormap = VegetationEngine::instance().getColormap(kIndexColormap, true);
        const RasterWindow window{0, 0, width, height, width, height};
        std::vector<uint8_t> rgba = renderPlannedFormulaRgba(*warpedSource, plan, window, "", NodataMask::BandsRead);
        if (!coverage.empty())
            for (size_t i = 0; i < npx; ++i)
                rgba[i * 4 + 3] = static_cast<uint8_t>(std::lround(rgba[i * 4 + 3] * coverage[i]));
        warped = {};

        // Step 4: the image in the requested format
        VsiFreePtr<GByte> pixels(static_cast<GByte *>(VSIMalloc(rgba.size())));
        if (!pixels)
            throw std::bad_alloc();
        std::memcpy(pixels.get(), rgba.data(), rgba.size());
        writeRgba(std::move(pixels), width, height, imageBackground(fi, request, true), fi, outBytes, outSize);
    }

} // namespace ddb
