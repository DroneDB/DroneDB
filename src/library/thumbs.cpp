/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "thumbs.h"

#include <cstdlib>
#include <iomanip>
#include <pdal/io/CopcReader.hpp>
#include <sstream>
#include <algorithm>
#include <cmath>

#include "coordstransformer.h"
#include "copc_utils.h"
#include "dbops.h"
#include "embedded_preview.h"
#include "point_rasterizer.h"
#include "exceptions.h"
#include "gdal_inc.h"
#include "gsplat.h"
#include "hash.h"
#include "mio.h"
#include "pointcloud.h"
#include "rad.h"
#include "randomutil.h"
#include "raster_stats.h"
#include "tile_encoder.h"
#include "tiler.h"
#include "userprofile.h"
#include "utils.h"
#include "vis_render.h"

// SPZ (vendored, MIT) - decode .spz to render an on-demand splat thumbnail.
#include "load-spz.h"
#include "splat-types.h"

namespace ddb {

fs::path getThumbFromUserCache(const fs::path& imagePath, int thumbSize, bool forceRecreate) {
    if (oneIn(1000))
        cleanupThumbsUserCache();
    if (!fs::exists(imagePath))
        throw FSException(imagePath.filename().string() + " does not exist");
    const fs::path outdir = UserProfile::get()->getThumbsDir(thumbSize);
    io::Path p = imagePath;
    const fs::path thumbPath = outdir / getThumbFilename(imagePath, p.getModifiedTime(), thumbSize);
    return generateThumb(imagePath, thumbSize, thumbPath, forceRecreate, nullptr, nullptr);
}

bool supportsThumbnails(EntryType type) {
    return type == Image || type == GeoImage || type == GeoRaster;
}

void generateThumbs(const std::vector<std::string>& input,
                    const fs::path& output,
                    int thumbSize,
                    bool useCrc) {
    if (input.size() > 1)
        io::assureFolderExists(output);
    const bool outputIsFile = input.size() == 1 && io::Path(output).checkExtension(
                                                       {"jpg", "jpeg", "png", "webp", "json"});

    const std::vector<fs::path> filePaths = std::vector<fs::path>(input.begin(), input.end());

    for (auto& fp : filePaths) {
        LOGD << "Parsing entry " << fp.string();

        const EntryType type = fingerprint(fp);
        io::Path p(fp);

        // Point clouds are recognized by the .copc.laz suffix produced by buildCopc().
        if (supportsThumbnails(type) || isCopcPath(fp.string())) {
            fs::path outImagePath;
            if (useCrc) {
                outImagePath = output / getThumbFilename(fp, p.getModifiedTime(), thumbSize);
            } else if (outputIsFile) {
                outImagePath = output;
            } else {
                outImagePath = output / fs::path(fp).replace_extension(".webp").filename();
            }
            std::cout << generateThumb(fp, thumbSize, outImagePath, true, nullptr, nullptr).string()
                      << std::endl;
        } else {
            LOGD << "Skipping " << fp;
        }
    }
}

fs::path getThumbFilename(const fs::path& imagePath, time_t modifiedTime, int thumbSize) {
    // Thumbnails are WEBP files idenfitied by:
    // CRC64(imagePath + "*" + modifiedTime + "*" + thumbSize).webp
    std::ostringstream os;
    os << imagePath.string() << "*" << modifiedTime << "*" << thumbSize;
    return fs::path(Hash::strCRC64(os.str()) + ".webp");
}

namespace {

// WebP quality of every thumbnail: at preview sizes q80 is visually indistinguishable
// from q95 at about half the bytes and encode time
constexpr int ThumbnailWebpQuality = 80;

// A point-cloud thumbnail reads about this many points per output pixel: enough for
// the 2-px discs to cover the footprint, a fraction of the cloud
constexpr uint64_t PointsPerThumbnailPixel = 2;

// RAII guard that closes a GDAL dataset on scope exit. Does nothing if released.
struct GdalDatasetGuard {
    GDALDatasetH handle;
    explicit GdalDatasetGuard(GDALDatasetH h = nullptr) : handle(h) {}
    ~GdalDatasetGuard() { if (handle) GDALClose(handle); }
    GdalDatasetGuard(const GdalDatasetGuard&) = delete;
    GdalDatasetGuard& operator=(const GdalDatasetGuard&) = delete;
};

// Range of source values mapped to 0-255 for one output band
struct ScaleRange {
    double min;
    double max;
};

// Writes a thumbnail as WEBP to outImagePath, or into *outBuffer when no path is given
void writeThumbnail(const RasterImage& image,
                    const fs::path& outImagePath,
                    uint8_t** outBuffer,
                    int* outBufferSize) {
    EncodeOptions options;
    options.format = ImageFormat::Webp;
    options.quality = ThumbnailWebpQuality;

    if (outImagePath.empty() && outBuffer != nullptr)
        TileEncoder::encode(image, options, outBuffer, outBufferSize);
    else
        TileEncoder::write(image, options, outImagePath.string());
}

// Open a source raster, transparently falling back between a /vsicurl/ URL
// and the raw path when the input looks like a remote TIFF.
GDALDatasetH openThumbSource(const fs::path& imagePath, std::string& resolvedPath) {
    resolvedPath = imagePath.string();
    bool tryReopen = false;
    if (utils::isNetworkPath(resolvedPath) &&
        io::Path(resolvedPath).checkExtension({"tif", "tiff"})) {
        resolvedPath = "/vsicurl/" + resolvedPath;
        tryReopen = true;
    }

    GDALDatasetH hSrc = GDALOpen(resolvedPath.c_str(), GA_ReadOnly);
    if (!hSrc && tryReopen) {
        resolvedPath = imagePath.string();
        hSrc = GDALOpen(resolvedPath.c_str(), GA_ReadOnly);
    }
    if (!hSrc)
        throw GDALException("Cannot open " + resolvedPath + " for reading");
    return hSrc;
}

// Opens the source of an image thumbnail. A local JPEG that carries a preview of the same
// framing at least thumbSize pixels long (MPF, e.g. the 960x720 copy in DJI photos) is
// decoded from the preview: the entropy decoding of the full image is most of the cost of
// a thumbnail, whatever the DCT scale. frameWidth/frameHeight receive the size of the image
// the thumbnail represents (the main image), so both sources give the same output size.
GDALDatasetH openImageThumbSource(const fs::path& imagePath, int thumbSize, std::string& openPath,
                                  int& frameWidth, int& frameHeight) {
    frameWidth = frameHeight = 0;
    if (!utils::isNetworkPath(imagePath.string()) && io::Path(imagePath).checkExtension({"jpg", "jpeg"})) {
        const JpegLayout layout = readJpegLayout(imagePath);
        if (const auto preview = pickJpegPreview(layout, thumbSize)) {
            openPath = jpegPreviewGdalPath(imagePath, *preview);
            if (GDALDatasetH hPreview = GDALOpen(openPath.c_str(), GA_ReadOnly)) {
                frameWidth = layout.width;
                frameHeight = layout.height;
                LOGD << "Thumbnail of " << imagePath.string() << " from its " << preview->width << "x"
                     << preview->height << " preview";
                return hPreview;
            }
        }
    }

    GDALDatasetH hSrc = openThumbSource(imagePath, openPath);
    frameWidth = GDALGetRasterXSize(hSrc);
    frameHeight = GDALGetRasterYSize(hSrc);
    return hSrc;
}

// Compute thumbnail target dimensions preserving aspect ratio.
// Guaranteed to return values >= 1 pixel.
void computeThumbTargetSize(int srcWidth, int srcHeight, int thumbSize,
                            int& outWidth, int& outHeight) {
    if (srcWidth <= 0 || srcHeight <= 0 || thumbSize <= 0)
        throw InvalidArgsException("Invalid thumbnail size parameters");

    if (srcWidth > srcHeight) {
        outWidth = thumbSize;
        outHeight = static_cast<int>((static_cast<float>(thumbSize) /
                                      static_cast<float>(srcWidth)) *
                                     static_cast<float>(srcHeight));
    } else {
        outHeight = thumbSize;
        outWidth = static_cast<int>((static_cast<float>(thumbSize) /
                                     static_cast<float>(srcHeight)) *
                                    static_cast<float>(srcWidth));
    }
    if (outWidth < 1) outWidth = 1;
    if (outHeight < 1) outHeight = 1;
}

// Decide which source band should provide the alpha channel for the thumbnail.
// Returns nullptr when the source is known to be fully opaque (no nodata, no
// alpha band, no per-dataset mask) so the caller can skip the alpha pass.
GDALRasterBandH resolveAlphaBand(GDALDatasetH hSrc, int bandCount) {
    // Generic 2-band rasters conventionally carry grayscale + alpha.
    if (bandCount == 2)
        return GDALGetRasterBand(hSrc, 2);

    // Prefer an explicit alpha band when present on 4+ band rasters.
    if (bandCount >= 4) {
        GDALRasterBandH band4 = GDALGetRasterBand(hSrc, 4);
        if (band4 && GDALGetRasterColorInterpretation(band4) == GCI_AlphaBand)
            return band4;
    }

    GDALRasterBandH band1 = GDALGetRasterBand(hSrc, 1);
    if (!band1)
        return nullptr;

    // GDALGetMaskBand always returns a valid handle, but for fully-valid
    // rasters its only flag is GMF_ALL_VALID. In that case skip the extra
    // mask read and output a plain 3-band WEBP.
    const int maskFlags = GDALGetMaskFlags(band1);
    if (maskFlags == GMF_ALL_VALID)
        return nullptr;

    return GDALGetMaskBand(band1);
}

// Read an alpha source band at target resolution into image.alpha. Leaves the image
// without alpha when every pixel is opaque.
void readThumbAlpha(GDALRasterBandH hAlphaBand, int srcWidth, int srcHeight, RasterImage& image) {
    std::vector<uint8_t> buffer(image.pixels());

    GDALRasterIOExtraArg extraArg;
    INIT_RASTERIO_EXTRA_ARG(extraArg);
    extraArg.eResampleAlg = GRIORA_Average;

    if (GDALRasterIOEx(hAlphaBand, GF_Read, 0, 0, srcWidth, srcHeight,
                       buffer.data(), image.width, image.height,
                       GDT_Byte, 0, 0, &extraArg) != CE_None) {
        throw GDALException(std::string("Failed to read alpha/mask band: ") +
                            CPLGetLastErrorMsg());
    }

    if (std::any_of(buffer.begin(), buffer.end(), [](uint8_t v) { return v < 255; }))
        image.alpha = std::move(buffer);
}

// Resamples the source to a 3-band Byte image of width x height. bands[i] is the source
// band of output band i (ignored when expandPalette); scales[i], when given, maps
// [min, max] of output band i to 0-255. Byte data without scales is copied as is.
RasterImage translateToImage(GDALDatasetH hSrc,
                             int width,
                             int height,
                             const std::vector<int>& bands,
                             bool expandPalette,
                             const std::vector<ScaleRange>& scales,
                             const std::string& sourceLabel) {
    const auto fmt = [](double v) {
        std::ostringstream os;
        os << std::setprecision(17) << v;
        return os.str();
    };

    std::vector<std::string> argStore = {
        "-outsize", std::to_string(width), std::to_string(height),
        "-ot", "Byte",
        "-r", expandPalette ? "nearest" : "average",
        "-of", "MEM",
        // Drop SRS: the thumbnail is meant as a display-only artifact.
        "-a_srs", ""};

    if (expandPalette) {
        // Palette transparency, if any, is recovered from the source mask band.
        argStore.insert(argStore.end(), {"-expand", "rgb"});
    } else {
        for (int b : bands)
            argStore.insert(argStore.end(), {"-b", std::to_string(b)});
    }

    for (size_t i = 0; i < scales.size(); i++) {
        argStore.insert(argStore.end(), {"-scale_" + std::to_string(i + 1),
                                         fmt(scales[i].min), fmt(scales[i].max), "0", "255"});
    }

    std::vector<char*> argv;
    argv.reserve(argStore.size() + 1);
    for (auto& s : argStore) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);

    GDALTranslateOptions* psOptions = GDALTranslateOptionsNew(argv.data(), nullptr);
    if (!psOptions)
        throw GDALException("Failed to create GDAL translate options for " + sourceLabel);

    GdalDatasetGuard translated(GDALTranslate("", hSrc, psOptions, nullptr));
    GDALTranslateOptionsFree(psOptions);
    if (!translated.handle)
        throw GDALException("Failed to normalize " + sourceLabel + " for thumbnail: " + CPLGetLastErrorMsg());

    RasterImage image(width, height, 3, false);
    if (GDALDatasetRasterIO(translated.handle, GF_Read, 0, 0, width, height, image.color.data(), width, height,
                            GDT_Byte, 3, nullptr, 0, 0, 0) != CE_None)
        throw GDALException(std::string("Failed to read thumbnail pixels: ") + CPLGetLastErrorMsg());
    return image;
}

// Display range of the source band (its minimum and maximum) for a stretch to 0-255
ScaleRange bandRange(const fs::path& imagePath, GDALDatasetH hSrc, int band) {
    const auto s = RasterStatsProvider::instance().get(imagePath.string(), hSrc, band);
    return {s.min, s.max};
}

}  // namespace

void generateImageThumb(const fs::path& imagePath,
                        int thumbSize,
                        const fs::path& outImagePath,
                        uint8_t** outBuffer,
                        int* outBufferSize) {
    std::string openPath;
    int frameWidth = 0, frameHeight = 0;
    GdalDatasetGuard srcGuard(openImageThumbSource(imagePath, thumbSize, openPath, frameWidth, frameHeight));
    GDALDatasetH hSrc = srcGuard.handle;

    const int srcWidth = GDALGetRasterXSize(hSrc);
    const int srcHeight = GDALGetRasterYSize(hSrc);
    const int bandCount = GDALGetRasterCount(hSrc);
    if (srcWidth <= 0 || srcHeight <= 0 || bandCount <= 0 || frameWidth <= 0 || frameHeight <= 0)
        throw GDALException("Invalid source raster dimensions in " + openPath);

    GDALRasterBandH hBand1 = GDALGetRasterBand(hSrc, 1);
    const bool hasPalette = hBand1 && GDALGetRasterColorTable(hBand1) != nullptr;
    const bool isByte = GDALGetRasterDataType(hBand1) == GDT_Byte;

    int targetWidth = 0, targetHeight = 0;
    computeThumbTargetSize(frameWidth, frameHeight, thumbSize, targetWidth, targetHeight);

    // Grey (and grey + alpha) is replicated to RGB; otherwise the first three bands.
    // Only non-Byte data (DEM, thermal...) is stretched, from the raster statistics:
    // 8-bit photos keep their colours.
    const std::vector<int> bands = bandCount <= 2 ? std::vector<int>{1, 1, 1} : std::vector<int>{1, 2, 3};
    std::vector<ScaleRange> scales;
    if (!hasPalette && !isByte) {
        for (int b : bands)
            scales.push_back(bandRange(imagePath, hSrc, b));
    }

    RasterImage image = translateToImage(hSrc, targetWidth, targetHeight, bands, hasPalette, scales, openPath);

    if (GDALRasterBandH hAlphaBand = resolveAlphaBand(hSrc, bandCount))
        readThumbAlpha(hAlphaBand, srcWidth, srcHeight, image);

    LOGD << "Writing " << (image.hasAlpha() ? "RGBA" : "RGB") << " WEBP thumbnail for " << openPath;
    writeThumbnail(image, outImagePath, outBuffer, outBufferSize);
}

void generateImageThumbEx(const fs::path& imagePath,
                          int thumbSize,
                          const fs::path& outImagePath,
                          uint8_t** outBuffer,
                          int* outBufferSize,
                          const ThumbVisParams& visParams) {
    GdalDatasetGuard srcGuard(GDALOpen(imagePath.string().c_str(), GA_ReadOnly));
    GDALDatasetH hSrc = srcGuard.handle;
    if (!hSrc)
        throw GDALException("Cannot open " + imagePath.string() + " for reading");

    const int width = GDALGetRasterXSize(hSrc);
    const int height = GDALGetRasterYSize(hSrc);
    const int bandCount = GDALGetRasterCount(hSrc);
    const GDALDataType srcType = GDALGetRasterDataType(GDALGetRasterBand(hSrc, 1));

    int targetWidth, targetHeight;
    computeThumbTargetSize(width, height, thumbSize, targetWidth, targetHeight);

    // Formula mode: vegetation index rendered with its colormap
    if (!visParams.formula.empty()) {
        const RasterWindow window{0, 0, width, height, targetWidth, targetHeight};
        const auto rgba = renderFormulaRgba(hSrc, imagePath.string(), window, visParams);

        RasterImage image(targetWidth, targetHeight, 3, true);
        for (size_t i = 0; i < image.pixels(); i++) {
            for (int b = 0; b < 3; b++) image.band(b)[i] = rgba[i * 4 + b];
            image.alpha[i] = rgba[i * 4 + 3];
        }
        writeThumbnail(image, outImagePath, outBuffer, outBufferSize);
        return;
    }

    // Band selection (explicit, preset or detected sensor) + stretch
    std::vector<int> bands = resolveVisBands(imagePath.string(), visParams, bandCount);
    if (bands.empty())
        bands = bandCount <= 2 ? std::vector<int>{1, 1, 1} : std::vector<int>{1, 2, 3};
    bands.resize(3, bands.back());

    // Explicit range for all bands; for non-Byte data otherwise the common p2-p98
    // (approximated from the statistics) of the shown bands. 8-bit data is not stretched.
    std::vector<ScaleRange> scales;
    if (!visParams.rescale.empty()) {
        const auto [rMin, rMax] = parseRescaleRange(visParams.rescale);
        scales.assign(bands.size(), {rMin, rMax});
    } else if (srcType != GDT_Byte) {
        double globalMin = std::numeric_limits<double>::max();
        double globalMax = std::numeric_limits<double>::lowest();
        for (int b : bands) {
            const auto [sMin, sMax] = stretchRange(RasterStatsProvider::instance().get(imagePath.string(), hSrc, b));
            globalMin = std::min(globalMin, sMin);
            globalMax = std::max(globalMax, sMax);
        }
        scales.assign(bands.size(), {globalMin, globalMax});
    }

    RasterImage image = translateToImage(hSrc, targetWidth, targetHeight, bands, false, scales, imagePath.string());
    if (GDALRasterBandH hAlphaBand = resolveAlphaBand(hSrc, bandCount))
        readThumbAlpha(hAlphaBand, width, height, image);

    writeThumbnail(image, outImagePath, outBuffer, outBufferSize);
}

void generatePointCloudThumb(const fs::path& copcPath,
                             int thumbSize,
                             const fs::path& outImagePath,
                             uint8_t** outBuffer,
                             int* outBufferSize,
                             uint64_t pointBudget) {
    PointCloudInfo pcInfo;

    // Load COPC information
    int span;
    if (!getCopcInfo(copcPath.string(), pcInfo, 3857, &span)) {
        throw InvalidArgsException("Cannot get COPC info for " + copcPath.string());
    }

    // Validate bounds array has required elements (minX, minY, minZ, maxX, maxY, maxZ)
    if (pcInfo.bounds.size() < 6) {
        throw InvalidArgsException("COPC bounds array does not contain at least 6 elements (minX, minY, minZ, maxX, maxY, maxZ required)");
    }

    const auto tileSize = thumbSize;
    GlobalMercator mercator(tileSize);

    // Calculate bounds based on spatial reference system
    double oMinX, oMaxX, oMaxY, oMinY;
    bool hasSpatialSystem = !pcInfo.wktProjection.empty() && !pcInfo.polyBounds.empty();

    if (hasSpatialSystem) {
        oMinX = pcInfo.polyBounds.getPoint(0).x;
        oMaxX = pcInfo.polyBounds.getPoint(2).x;
        oMaxY = pcInfo.polyBounds.getPoint(2).y;
        oMinY = pcInfo.polyBounds.getPoint(0).y;
    } else {
        oMinX = pcInfo.bounds[0];
        oMinY = pcInfo.bounds[1];
        oMaxX = pcInfo.bounds[3];
        oMaxY = pcInfo.bounds[4];
    }

    // Calculate length for zoom level determination
    auto length = std::min(std::abs(oMaxX - oMinX), std::abs(oMaxY - oMinY));

    if (length == 0) {
        // Fallback to raw bounds if transformed bounds are invalid
        oMinX = pcInfo.bounds[0];
        oMaxX = pcInfo.bounds[3];
        oMaxY = pcInfo.bounds[4];
        oMinY = pcInfo.bounds[1];

        length = std::min(std::abs(oMaxX - oMinX), std::abs(oMaxY - oMinY));

        if (length == 0) {
            throw GDALException("Cannot calculate length: point cloud has zero extent");
        }

        hasSpatialSystem = false;
    }

    const auto hasColors =
        std::find(pcInfo.dimensions.begin(), pcInfo.dimensions.end(), "Red") !=
            pcInfo.dimensions.end() &&
        std::find(pcInfo.dimensions.begin(), pcInfo.dimensions.end(), "Green") !=
            pcInfo.dimensions.end() &&
        std::find(pcInfo.dimensions.begin(), pcInfo.dimensions.end(), "Blue") !=
            pcInfo.dimensions.end();

#ifdef _WIN32
    const fs::path caBundlePath = io::getDataPath("curl-ca-bundle.crt");
    if (!caBundlePath.empty()) {
        LOGD << "ARBITRER CA Bundle: " << caBundlePath.string();
        std::stringstream ss;
        ss << "ARBITER_CA_INFO=" << caBundlePath.string();
        if (_putenv(ss.str().c_str()) != 0) {
            LOGD << "Cannot set ARBITER_CA_INFO";
        }
    }
#endif

    // Read the octree down to the deepest level whose points fit the budget (about
    // PointsPerThumbnailPixel per output pixel), not every point finer than a pixel
    const uint64_t budget = pointBudget > 0
        ? pointBudget
        : PointsPerThumbnailPixel * static_cast<uint64_t>(thumbSize) * static_cast<uint64_t>(thumbSize);
    double resolution;
    try {
        const auto hierarchy = readCopcHierarchy(copcPath.string());
        const int depth = planCopcDepth(hierarchy.pointsPerDepth, budget);
        resolution = copcResolutionForDepth(hierarchy.spacing, depth);
        LOGD << "Point cloud thumbnail reads COPC depths 0-" << depth << " (resolution " << resolution << ")";
    } catch (const AppException& e) {
        // Pixel-size heuristic: reads every level finer than a pixel
        LOGD << "Cannot plan COPC read (" << e.what() << "), using the pixel size";
        const auto tz = mercator.zoomForLength(length);
        resolution = tz < 0 ? 1 : mercator.resolution(tz);
    }

    pdal::Options copcOpts = copcReaderOptions(copcPath.string());
    copcOpts.add("resolution", resolution);

    pdal::CopcReader copcReader;
    copcReader.setOptions(copcOpts);

    // Fit the footprint in the square thumbnail, centred
    const double width = oMaxX - oMinX;
    const double height = oMaxY - oMinY;
    const double tileScaleW = tileSize / width;
    const double tileScaleH = tileSize / height;

    PixelMapping mapping;
    mapping.originX = oMinX;
    mapping.originY = oMinY;
    if (tileScaleW > tileScaleH) {
        // Taller than wider
        mapping.scaleX = mapping.scaleY = tileScaleH;
        mapping.offsetX = (tileSize - width * tileScaleH) / 2;
    } else {
        // Wider than taller
        mapping.scaleX = mapping.scaleY = tileScaleW;
        mapping.offsetY = (tileSize - height * tileScaleW) / 2;
    }
    mapping.width = mapping.height = tileSize;

    std::unique_ptr<CoordsTransformer> toMercator;
    if (hasSpatialSystem)
        toMercator = std::make_unique<CoordsTransformer>(pcInfo.wktProjection, 3857);

    const double minZ = pcInfo.bounds[2];
    const double zRange = pcInfo.bounds[5] - minZ;

    PointRasterizer rasterizer(tileSize, mapping);
    size_t pointsRead = 0;
    try {
        forEachPointBatch(copcReader, [&](PointBatch& points) {
            pointsRead += points.size();
            if (!hasColors) {
                // Grey from elevation: lower is darker
                for (size_t i = 0; i < points.size(); i++) {
                    const uint16_t intensity = zRange > 0
                        ? static_cast<uint8_t>((points.z[i] - minZ) / zRange * 255)
                        : 128;
                    points.red[i] = points.green[i] = points.blue[i] = intensity;
                }
            }
            if (toMercator)
                toMercator->transform(points.size(), points.x.data(), points.y.data());
            rasterizer.add(points);
        });
    } catch (const pdal::pdal_error& e) {
        throw PDALException("PDAL error: " + std::string(e.what()));
    }

    if (pointsRead == 0)
        throw GDALException("No points fetched from cloud, check zoom level");

    writeThumbnail(rasterizer.toImage(), outImagePath, outBuffer, outBufferSize);
}

// Shared orthographic rasteriser for Gaussian Splat previews. Inputs are already in display
// space: `colors` holds RGB in [0,1] and `opacities` holds activated opacity (NOT logits). The
// scene is projected onto the plane perpendicular to its flattest axis (so aerial scenes
// naturally render top-down) and depth-sorted with a simple z-buffer. The output size honours
// thumbSize at request time - nothing is precomputed at build time.
static void rasterizeSplatPreview(const std::vector<float>& positions,
                                  const std::vector<float>& colors,
                                  const std::vector<float>& opacities,
                                  size_t n,
                                  int thumbSize,
                                  const fs::path& outImagePath,
                                  const std::string& sourceLabel,
                                  uint8_t** outBuffer,
                                  int* outBufferSize) {
    if (n == 0 || positions.size() < n * 3)
        throw InvalidArgsException("No splats to render in " + sourceLabel);

    // Position bounds.
    double mn[3] = {std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
                    std::numeric_limits<double>::max()};
    double mx[3] = {std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(),
                    std::numeric_limits<double>::lowest()};
    for (size_t i = 0; i < n; ++i) {
        for (int k = 0; k < 3; ++k) {
            const double v = static_cast<double>(positions[i * 3 + k]);
            mn[k] = std::min(mn[k], v);
            mx[k] = std::max(mx[k], v);
        }
    }

    const double extent[3] = {mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]};

    // Depth axis = flattest dimension; the remaining two form the image plane.
    int depthAxis = 0;
    if (extent[1] < extent[depthAxis]) depthAxis = 1;
    if (extent[2] < extent[depthAxis]) depthAxis = 2;
    const int uAxis = (depthAxis == 0) ? 1 : 0;
    const int vAxis = (depthAxis == 2) ? 1 : 2;

    const double uExtent = extent[uAxis];
    const double vExtent = extent[vAxis];
    if (uExtent <= 0.0 && vExtent <= 0.0)
        throw GDALException("Gaussian Splat has zero planar extent; cannot render thumbnail");

    const int tileSize = thumbSize;
    const size_t wSize = static_cast<size_t>(tileSize) * static_cast<size_t>(tileSize);

    // Band-sequential RGB (3 planes) + a separate alpha plane, as in RasterImage
    std::vector<uint8_t> buffer(wSize * 3, 0);
    std::vector<uint8_t> alphaBuffer(wSize, 0);
    std::vector<float> zBuffer(wSize, std::numeric_limits<float>::lowest());

    // Fit the (u, v) extent into the tile preserving aspect ratio, with a 1px margin.
    const double drawable = static_cast<double>(tileSize - 1);
    const double scaleU = uExtent > 0.0 ? drawable / uExtent : 0.0;
    const double scaleV = vExtent > 0.0 ? drawable / vExtent : 0.0;
    double scale = std::min(scaleU > 0.0 ? scaleU : scaleV, scaleV > 0.0 ? scaleV : scaleU);
    if (scale <= 0.0) scale = 1.0;
    const double offX = (tileSize - uExtent * scale) / 2.0;
    const double offY = (tileSize - vExtent * scale) / 2.0;

    const auto toByte = [](double v) -> uint8_t {
        return static_cast<uint8_t>(std::lround(std::min(std::max(v, 0.0), 1.0) * 255.0));
    };

    // Point radius scales with thumbnail size so larger previews keep visual density.
    // At 256 px the radius is 1; at 1024 px it is 4. Clamped to [1, 8].
    const int radius = std::max(1, std::min(8, static_cast<int>(std::lround(static_cast<double>(tileSize) / 256.0))));
    const int radiusSq = radius * radius;

    size_t rendered = 0;
    for (size_t i = 0; i < n; ++i) {
        // Opacities are already activated; skip near-transparent splats.
        const double opacity = (i < opacities.size()) ? static_cast<double>(opacities[i]) : 1.0;
        if (opacity < 0.02)
            continue;

        const double u = static_cast<double>(positions[i * 3 + uAxis]);
        const double v = static_cast<double>(positions[i * 3 + vAxis]);
        const double d = static_cast<double>(positions[i * 3 + depthAxis]);

        int cx = static_cast<int>((u - mn[uAxis]) * scale + offX);
        int cy = static_cast<int>((v - mn[vAxis]) * scale + offY);
        // Flip vertically so "up" in world space points up in the image.
        cy = tileSize - 1 - cy;

        double rgb[3] = {0.5, 0.5, 0.5};
        if (colors.size() >= (i + 1) * 3) {
            for (int c = 0; c < 3; ++c)
                rgb[c] = static_cast<double>(colors[i * 3 + c]);
        }
        const uint8_t br = toByte(rgb[0]);
        const uint8_t bg = toByte(rgb[1]);
        const uint8_t bb = toByte(rgb[2]);
        const float dz = static_cast<float>(d);

        // Draw a filled circle, respecting the z-buffer.
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                if (dx * dx + dy * dy > radiusSq)
                    continue;
                const int px = cx + dx;
                const int py = cy + dy;
                if (px < 0 || px >= tileSize || py < 0 || py >= tileSize)
                    continue;
                const size_t idx = static_cast<size_t>(py) * static_cast<size_t>(tileSize) +
                                   static_cast<size_t>(px);
                if (dz <= zBuffer[idx])
                    continue;
                zBuffer[idx] = dz;
                buffer[0 * wSize + idx] = br;
                buffer[1 * wSize + idx] = bg;
                buffer[2 * wSize + idx] = bb;
                alphaBuffer[idx] = 255;
                ++rendered;
            }
        }
    }

    if (rendered == 0)
        throw GDALException("No splats projected into the thumbnail frame");

    RasterImage image(tileSize, tileSize, 3, false);
    image.color = std::move(buffer);
    image.alpha = std::move(alphaBuffer);
    writeThumbnail(image, outImagePath, outBuffer, outBufferSize);
}

// Render an on-demand orthographic preview of a Gaussian Splat (.spz). Colours come from the
// spherical-harmonics DC term and opacity from the stored logit; both are converted to display
// space before handing off to the shared rasteriser.
void generateSplatThumb(const fs::path& spzPath,
                        int thumbSize,
                        const fs::path& outImagePath,
                        uint8_t** outBuffer,
                        int* outBufferSize) {
    spz::GaussianCloud cloud;
    try {
        spz::UnpackOptions unpack;
        cloud = spz::loadSpz(spzPath.string(), unpack);
    } catch (const std::exception& e) {
        throw AppException(std::string("Cannot decode .spz for thumbnail: ") + e.what());
    }

    const size_t n = static_cast<size_t>(std::max(cloud.numPoints, 0));
    if (n == 0 || cloud.positions.size() < n * 3)
        throw InvalidArgsException("No splats to render in " + spzPath.string());

    // Convert SPZ-native attributes to display space: SH DC term -> RGB, logit -> opacity.
    std::vector<float> colors(n * 3, 0.5f);
    std::vector<float> opacities(n, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        const float logit = (i < cloud.alphas.size()) ? cloud.alphas[i] : 0.0f;
        opacities[i] = 1.0f / (1.0f + std::exp(-logit));
        if (cloud.colors.size() >= (i + 1) * 3) {
            for (int c = 0; c < 3; ++c)
                colors[i * 3 + c] =
                    static_cast<float>(0.5 + SH_C0 * static_cast<double>(cloud.colors[i * 3 + c]));
        }
    }

    rasterizeSplatPreview(cloud.positions, colors, opacities, n, thumbSize, outImagePath,
                          spzPath.string(), outBuffer, outBufferSize);
}

// Render an on-demand preview from a Spark RAD level-of-detail file. The two coarsest chunks are
// decoded (a low-density view of the whole scene), so this stays cheap enough to run per request -
// the same on-the-fly model used for COPC point clouds. RAD colours are already display RGB and
// opacities already activated, so no conversion is required.
void generateSplatThumbFromRad(const fs::path& radPath,
                               int thumbSize,
                               const fs::path& outImagePath,
                               uint8_t** outBuffer,
                               int* outBufferSize) {
    RadCoarseSplats splats;
    try {
        splats = readRadCoarseSplats(radPath, /*maxChunks=*/2);
    } catch (const std::exception& e) {
        throw AppException(std::string("Cannot decode .rad for thumbnail: ") + e.what());
    }

    rasterizeSplatPreview(splats.positions, splats.colors, splats.opacities, splats.count,
                          thumbSize, outImagePath, radPath.string(), outBuffer, outBufferSize);
}

// imagePath can be either absolute or relative or a network URL and it's up to the user to
// invoke the function properly as to avoid conflicts with relative paths
fs::path generateThumb(const fs::path& inputPath,
                       int thumbSize,
                       const fs::path& outImagePath,
                       bool forceRecreate,
                       uint8_t** outBuffer,
                       int* outBufferSize) {
    if (thumbSize <= 0)
        throw InvalidArgsException("thumbSize must be greater than 0");

    if (!utils::isNetworkPath(inputPath.string()) && !exists(inputPath))
        throw FSException(inputPath.string() + " does not exist");

    // Check existance of thumbnail, return if exists
    if (!utils::isNetworkPath(inputPath.string()) && exists(outImagePath) && !forceRecreate)
        return outImagePath;

    LOGD << "ImagePath = " << inputPath;
    LOGD << "OutImagePath = " << outImagePath;
    LOGD << "Size = " << thumbSize;

    if (isCopcPath(inputPath.string()))
        generatePointCloudThumb(inputPath, thumbSize, outImagePath, outBuffer, outBufferSize);
    else if (isRadPath(inputPath.string()))
        generateSplatThumbFromRad(inputPath, thumbSize, outImagePath, outBuffer, outBufferSize);
    else if (isSpzPath(inputPath.string()))
        generateSplatThumb(inputPath, thumbSize, outImagePath, outBuffer, outBufferSize);
    else
        generateImageThumb(inputPath, thumbSize, outImagePath, outBuffer, outBufferSize);

    return outImagePath;
}

void cleanupThumbsUserCache() {
    LOGD << "Cleaning up thumbs user cache";

    const time_t threshold = utils::currentUnixTimestamp() - 60 * 60 * 24 * 5;  // 5 days
    const fs::path thumbsDir = UserProfile::get()->getThumbsDir();
    std::vector<fs::path> cleanupDirs;

    // Iterate size directories
    for (auto sd = fs::recursive_directory_iterator(thumbsDir);
         sd != fs::recursive_directory_iterator();
         ++sd) {
        fs::path sizeDir = sd->path();
        if (is_directory(sizeDir)) {
            for (auto t = fs::recursive_directory_iterator(sizeDir);
                 t != fs::recursive_directory_iterator();
                 ++t) {
                fs::path thumb = t->path();
                if (io::Path(thumb).getModifiedTime() < threshold) {
                    if (fs::remove(thumb))
                        LOGD << "Cleaned " << thumb.string();
                    else
                        LOGD << "Cannot clean " << thumb.string();
                }
            }

            if (is_empty(sizeDir)) {
                // Remove directory too
                cleanupDirs.push_back(sizeDir);
            }
        }
    }

    for (auto& d : cleanupDirs) {
        if (fs::remove(d))
            LOGD << "Cleaned " << d.string();
        else
            LOGD << "Cannot clean " << d.string();
    }
}

}  // namespace ddb
