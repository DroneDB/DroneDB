/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The normalize operation of the raster helper (raster_ops.h): the raster of a user rewritten as
// a tiled GeoTIFF that libddb wrote, so the host can read it (the analyses, the raster cache)
// without ever opening the file of the user.

#include <cerrno>
#include <cstring>
#include <string>

#include "cpl_conv.h"
#include "cpl_string.h"
#include "exceptions.h"
#include "gdal_open_policy.h"
#include "gdal_priv.h"
#include "gdal_raii.h"
#include "logger.h"
#include "mio.h"
#include "raster_ops.h"

namespace ddb {
namespace raster {

namespace {

// The size of a tile of a TIFF is a multiple of this, in pixels
constexpr int kTileMultiple = 16;

// Whether two lists of "NAME=value" items (or of plain strings) hold the same items in the same order
bool sameItems(CSLConstList a, CSLConstList b) {
    if (CSLCount(a) != CSLCount(b)) return false;
    for (int i = 0; a != nullptr && a[i] != nullptr; i++)
        if (std::strcmp(a[i], b[i]) != 0) return false;
    return true;
}

// The metadata domains of `from` that `to` does not hold yet. A GeoTIFF written by a copy keeps the
// default domain and the few the format has tags for (RPC, XMP...), but GDAL can store any domain
// in it, and the analyses read them all (the keywords of a sensor, the tags of a thermal camera).
// Left behind: the physical layout of the source (IMAGE_STRUCTURE describes how the source is
// stored, not the copy) and the domains that name other datasets.
void copyMetadataDomains(GDALMajorObject &from, GDALMajorObject &to) {
    const CPLStringList domains(from.GetMetadataDomainList(), TRUE);
    for (int i = 0; i < domains.size(); i++) {
        const std::string domain = domains[i];
        if (EQUAL(domain.c_str(), "IMAGE_STRUCTURE") || gdalopen::isFileNamingDomain(domain)) continue;
        CSLConstList items = from.GetMetadata(domain.c_str());
        if (items == nullptr || sameItems(items, to.GetMetadata(domain.c_str()))) continue;
        if (to.SetMetadata(items, domain.c_str()) != CE_None) {
            LOGW << "Cannot keep the metadata domain '" << domain << "' of a raster in its normalized copy";
        }
    }
}

std::string pixelsMessage(const fs::path &path, int64_t width, int64_t height) {
    return io::toUtf8(path) + " has " + std::to_string(width) + " x " + std::to_string(height) + " pixels, more than the " +
           std::to_string(kMaxNormalizePixels) + " that can be read in one piece: build the dataset first and use its Cloud Optimized GeoTIFF";
}

// What the copy writes: a compression that loses nothing, so every value is the one of the
// source, and the blocks and overviews of the source. GDAL's approximate statistics (the ones of
// the raster analyses) read an overview when there is one, and else sample whole blocks, so a
// copy with other overviews or blocks than the raster would read other pixels and give other
// statistics for the same raster. A strip layout stays strips (a JPEG or a PNG is read a row at a
// time), a tiled one keeps its tiles, as long as the format can write them; the overviews the
// raster offers (a JPEG decoded at a smaller scale, the ones of a TIFF) are copied as they are.
CPLStringList creationOptions(GDALDataset &source) {
    CPLStringList options;
    options.SetNameValue("COMPRESS", "DEFLATE");
    options.SetNameValue("ZLEVEL", "1");
    // A copy that grows beyond 4 GB is a BigTIFF; one that does not stays readable by any reader
    options.SetNameValue("BIGTIFF", "IF_SAFER");
    options.SetNameValue("COPY_SRC_OVERVIEWS", "YES");

    int blockWidth = 0;
    int blockHeight = 0;
    source.GetRasterBand(1)->GetBlockSize(&blockWidth, &blockHeight);
    if (blockWidth >= source.GetRasterXSize()) {
        options.SetNameValue("TILED", "NO");
        options.SetNameValue("BLOCKYSIZE", std::to_string(blockHeight).c_str());
    } else if (blockWidth % kTileMultiple == 0 && blockHeight % kTileMultiple == 0) {
        options.SetNameValue("TILED", "YES");
        options.SetNameValue("BLOCKXSIZE", std::to_string(blockWidth).c_str());
        options.SetNameValue("BLOCKYSIZE", std::to_string(blockHeight).c_str());
    } else {
        // The tiles of a TIFF are multiples of 16 pixels wide and high
        options.SetNameValue("TILED", "YES");
    }
    return options;
}

}  // namespace

void normalizeRaster(const fs::path &path, const fs::path &outFile) {
    const auto source = openUserRaster(io::toUtf8(path));
    if (!source)
        throw RasterOpException(ErrorCode::Unreadable, "Cannot open " + io::toUtf8(path) + " for reading");

    // Before anything is written: a raster too large to read in one piece leaves no file behind
    const int64_t width = source->GetRasterXSize();
    const int64_t height = source->GetRasterYSize();
    if (width * height > kMaxNormalizePixels)
        throw RasterOpException(ErrorCode::Limit, pixelsMessage(path, width, height));
    if (source->GetRasterCount() < 1)
        throw RasterOpException(ErrorCode::Unreadable, io::toUtf8(path) + " has no bands");

    GDALDriver *driver = GetGDALDriverManager()->GetDriverByName("GTiff");
    if (driver == nullptr)
        throw GDALException("The GTiff driver is not available");

    const std::string target = io::toUtf8(outFile);
    const CPLStringList options = creationOptions(*source);
    errno = 0;
    auto copy = adoptDataset(GDALCreateCopy(GDALDriver::ToHandle(driver), target.c_str(), datasetHandle(source), FALSE,
                                            options.List(), nullptr, nullptr));
    const int createError = errno;
    if (!copy) {
        io::assureIsRemoved(outFile);
        if (createError == ENOSPC)
            throw RetryableBuildException("No space left to normalize " + io::toUtf8(path), RetryableBuildReason::NoSpaceOnDevice);
        // The copy could not be written: not a defect of the raster, so not reported as one
        throw AppException("Cannot normalize " + io::toUtf8(path) + ": " + CPLGetLastErrorMsg());
    }

    copyMetadataDomains(*source, *copy);
    for (int band = 1; band <= source->GetRasterCount(); band++)
        copyMetadataDomains(*source->GetRasterBand(band), *copy->GetRasterBand(band));

    // The close writes what GDAL still holds (the metadata, the directory of the file): a failure
    // leaves a file that is not whole
    const auto closed = closeOutput(std::move(copy));
    if (closed.failed) {
        io::assureIsRemoved(outFile);
        if (closed.systemError == ENOSPC)
            throw RetryableBuildException("No space left to normalize " + io::toUtf8(path), RetryableBuildReason::NoSpaceOnDevice);
        throw AppException("Cannot write the normalized copy of " + io::toUtf8(path) + ": " + closed.message);
    }
}

}  // namespace raster
}  // namespace ddb
