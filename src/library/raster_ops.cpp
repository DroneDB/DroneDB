/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "raster_ops.h"

#include <algorithm>
#include <iomanip>
#include <new>
#include <sstream>
#include <string>
#include <system_error>

#include "build_budget.h"

namespace ddb {
namespace raster {

namespace {

bool isNoSpace(const std::error_code &code) {
    return code == std::errc::no_space_on_device;
}

// `band` at `overview`, or the band itself at the full resolution (-1) or when it has no such overview
GDALRasterBandH levelOf(GDALRasterBandH band, int overview) {
    GDALRasterBandH level = overview >= 0 ? GDALGetOverview(band, overview) : nullptr;
    return level != nullptr ? level : band;
}

// Refuses `band` when one decoding of a block of it makes more than kMaxDecodedBlockBytes: a block
// of `interleaved` bands (all the bands of a pixel-interleaved raster, decoded together) of its type.
// In floating point: a block of 2^31 x 2^31 pixels of thousands of bands does not fit in 64 bits.
void requireDecodableBlock(GDALRasterBandH band, int interleaved, const std::string &path) {
    int blockWidth = 0, blockHeight = 0;
    GDALGetBlockSize(band, &blockWidth, &blockHeight);
    const double bytes = static_cast<double>(std::max(blockWidth, 0)) * static_cast<double>(std::max(blockHeight, 0)) *
                         GDALGetDataTypeSizeBytes(GDALGetRasterDataType(band)) * std::max(interleaved, 1);
    if (bytes <= static_cast<double>(kMaxDecodedBlockBytes))
        return;
    constexpr double mib = 1024.0 * 1024.0;
    std::ostringstream message;
    message << std::fixed << std::setprecision(0) << path << " is stored in blocks of " << blockWidth << " x "
            << blockHeight << " pixels";
    if (interleaved > 1)
        message << " of " << interleaved << " bands";
    message << " at " << GDALGetRasterBandXSize(band) << " x " << GDALGetRasterBandYSize(band) << " pixels, "
            << bytes / mib << " MiB to decode for each, more than the " << static_cast<double>(kMaxDecodedBlockBytes) / mib
            << " MiB a read of a raster may decode at once";
    throw RasterOpException(ErrorCode::Limit, message.str());
}

}  // namespace

int overviewReadFor(GDALRasterBandH band, int bufWidth, int bufHeight, GDALRIOResampleAlg resampling) {
    int x = 0, y = 0;
    int width = GDALGetRasterBandXSize(band), height = GDALGetRasterBandYSize(band);
    GDALRasterIOExtraArg extraArg;
    INIT_RASTERIO_EXTRA_ARG(extraArg);
    extraArg.eResampleAlg = resampling;
    return GDALBandGetBestOverviewLevel2(GDALRasterBand::FromHandle(band), x, y, width, height, std::max(bufWidth, 1),
                                         std::max(bufHeight, 1), &extraArg);
}

void requireDecodableBlocks(GDALDatasetH ds, int overview, const std::string &path) {
    const int bandCount = GDALGetRasterCount(ds);
    const char *interleave = GDALGetMetadataItem(ds, "INTERLEAVE", "IMAGE_STRUCTURE");
    const int interleaved = interleave != nullptr && EQUAL(interleave, "PIXEL") ? bandCount : 1;
    for (int b = 1; b <= bandCount; b++)
        requireDecodableBlock(levelOf(GDALGetRasterBand(ds, b), overview), interleaved, path);
    // A mask of its own (a TIFF's internal mask) has blocks of its own; the others are a band, or
    // computed from the blocks of the band
    GDALRasterBandH first = bandCount > 0 ? GDALGetRasterBand(ds, 1) : nullptr;
    if (first != nullptr && GDALGetMaskFlags(first) != GMF_ALL_VALID)
        requireDecodableBlock(GDALGetMaskBand(levelOf(first, overview)), 1, path);
}

void requireDecodableLevels(GDALDatasetH ds, const std::string &path) {
    requireDecodableBlocks(ds, -1, path);
    GDALRasterBandH first = GDALGetRasterCount(ds) > 0 ? GDALGetRasterBand(ds, 1) : nullptr;
    for (int i = 0; first != nullptr && i < GDALGetOverviewCount(first); i++)
        requireDecodableBlocks(ds, i, path);
}

ErrorCode classify(const std::exception &failure) {
    if (const auto *op = dynamic_cast<const RasterOpException *>(&failure))
        return op->code();
    if (dynamic_cast<const std::bad_alloc *>(&failure) != nullptr)
        return ErrorCode::Limit;
    if (const auto *fs = dynamic_cast<const FSException *>(&failure))
        return isNoSpace(fs->code()) ? ErrorCode::NoSpace : ErrorCode::Internal;
    if (const auto *retryable = dynamic_cast<const RetryableBuildException *>(&failure)) {
        return retryable->reason() == RetryableBuildReason::IdleTimeout ? ErrorCode::Timeout : ErrorCode::NoSpace;
    }
    if (const auto *system = dynamic_cast<const std::system_error *>(&failure))
        return isNoSpace(system->code()) ? ErrorCode::NoSpace : ErrorCode::Internal;
    // What GDAL could not read of a file is the file's fault
    if (dynamic_cast<const GDALException *>(&failure) != nullptr)
        return ErrorCode::Unreadable;
    return ErrorCode::Internal;
}

parallel::CpuTokens acquireBuildThreads(const std::optional<int> &cap) {
    BuildOptions options;
    options.threads = cap;
    return parallel::tryAcquireThreads(static_cast<size_t>(BuildResourceBudget::fromConfig(options).threads));
}

BuildThreads::BuildThreads(const std::optional<int> &granted)
    : tokens_(acquireBuildThreads(granted)),
      numThreads_("GDAL_NUM_THREADS", std::to_string(tokens_.threads()).c_str()) {}

}  // namespace raster
}  // namespace ddb
