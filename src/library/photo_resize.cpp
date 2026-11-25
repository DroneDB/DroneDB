/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "photo_resize.h"

#include <algorithm>
#include <atomic>
#include <mutex>

#include "cancellation.h"
#include "exceptions.h"
#include "gdal_raii.h"
#include "mio.h"
#include "parallel.h"
#include "raster_input.h"

namespace ddb::raster
{
    namespace
    {
        // The photos resized at once when the host says nothing
        constexpr size_t kDefaultThreads = 8;

        // Writes the photo resampled to a JPEG of the given size
        void resizeToJpeg(const ResizeImagesParams::Image &image, const fs::path &target, int quality)
        {
            const std::string source = io::toUtf8(image.source);
            const auto src = openInputRaster(source);
            if (!src)
                throw GDALException("Cannot open " + source);
            GDALDatasetH hSrc = datasetHandle(src);
            const int bands = GDALGetRasterCount(hSrc);

            // No EXIF: the camera's metadata (its orientation among it) does not describe the resampled pixels
            std::vector<std::string> args = {"-of", "JPEG", "-co", "QUALITY=" + std::to_string(quality),
                                             "-co", "WRITE_EXIF_METADATA=NO",
                                             "-outsize", std::to_string(image.width), std::to_string(image.height),
                                             "-r", "average"};
            // JPEG holds bytes: 16-bit pixels (the bands of a multispectral camera) are scaled from the
            // range of their type, the same for every image, so the images keep their relative exposure
            const GDALDataType type = GDALGetRasterDataType(GDALGetRasterBand(hSrc, 1));
            if (type == GDT_UInt16)
                args.insert(args.end(), {"-ot", "Byte", "-scale", "0", "65535", "0", "255"});
            else if (type == GDT_Int16)
                args.insert(args.end(), {"-ot", "Byte", "-scale", "0", "32767", "0", "255"});
            else if (type != GDT_Byte)
                throw GDALException(source + " has " + GDALGetDataTypeName(type) +
                                    " pixels, which cannot be written as a JPEG");
            // JPEG holds 1 or 3 bands: grey, or the colours of an image with an alpha or more bands
            const int keep = bands == 2 ? 1 : std::min(bands, 3);
            if (keep != bands)
                for (int b = 1; b <= keep; ++b)
                    args.insert(args.end(), {"-b", std::to_string(b)});

            CPLStringList argv = cslFromStrings(args);
            const GdalOptionsPtr<GDALTranslateOptions> options(GDALTranslateOptionsNew(argv.List(), nullptr));
            if (!options)
                throw GDALException("Cannot resize " + source);
            cancel::GdalProgress progress;
            progress.attach(options.get());

            auto out = adoptDataset(GDALTranslate(io::toUtf8(target).c_str(), hSrc, options.get(), nullptr));
            if (!out)
            {
                cancel::throwIfCanceled();
                throw GDALException("Cannot resize " + source + ": " + CPLGetLastErrorMsg());
            }
            if (out->Close() != CE_None)
                throw GDALException("Cannot write " + image.name + ": " + CPLGetLastErrorMsg());
        }
    } // namespace

    std::vector<std::string> resizePhotos(const ResizeImagesParams &params, const fs::path &folder,
                                          const ProgressCallback &progress)
    {
        std::vector<std::string> errors(params.images.size());
        const size_t threads = params.threads ? static_cast<size_t>(*params.threads) : kDefaultThreads;
        std::atomic<size_t> done{0};
        std::mutex reporting;
        parallel::forEachIndex(params.images.size(), threads, [&](size_t i) {
            const auto &image = params.images[i];
            const fs::path target = folder / io::fromUtf8(image.name);
            try
            {
                resizeToJpeg(image, target, params.quality);
            }
            catch (const CanceledException &)
            {
                throw;
            }
            catch (const std::exception &e)
            {
                std::error_code ec;
                fs::remove(target, ec);
                errors[i] = e.what();
            }
            const size_t finished = ++done;
            if (progress)
            {
                const std::lock_guard lock(reporting);
                progress(static_cast<double>(finished) / static_cast<double>(params.images.size()));
            }
        });
        return errors;
    }
} // namespace ddb::raster
