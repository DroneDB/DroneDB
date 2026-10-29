/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "cancellation.h"
#include "json.h"
#include "randomutil.h"
#include "stringutil.h"
#include "dbops.h"
#include "geoproject.h"
#include "logger.h"
#include "mio.h"
#include <format>
#include "gdal_open_policy.h"
#include "gdal_raii.h"
#include "gtiff_options.h"
#include "raster_artefacts.h"
#include "raster_helper.h"
#include "raster_ops.h"

namespace ddb
{

    namespace
    {

        // The folder of the projected images when there are several (or the output is a folder)
        // is created here; true when the images go to a folder
        bool prepareOutput(const std::vector<std::string> &images, const std::string &output)
        {
            bool isDirectory = fs::is_directory(io::fromUtf8(output));
            bool outputToDir = images.size() > 1 || isDirectory;
            if (outputToDir)
            {
                if (!isDirectory)
                {
                    // Bad input?
                    if (fs::is_regular_file(io::fromUtf8(output)))
                    {
                        throw FSException(output + " is a file. (Did you switch the input and output parameters?)");
                    }
                    io::createDirectories(io::fromUtf8(output));
                }
            }
            return outputToDir;
        }

        // The scale `outsize` ("N" pixels of width, or "N%") stands for an image `width` pixels
        // wide. Throws InvalidArgsException for text that is not a positive number.
        double outsizeRatio(const std::string &outsize, int width)
        {
            try {
                if (outsize.back() == '%')
                {
                    // Validate percentage format before conversion
                    std::string percentValue = outsize.substr(0, outsize.length() - 1);
                    for (char c : percentValue)
                        if (!str::isDigitChar(c) && c != '.' && c != '-' && c != '+')
                            throw InvalidArgsException("Invalid percentage format: " + outsize);

                    double value = std::stod(percentValue);
                    if (value <= 0)
                        throw InvalidArgsException("Percentage must be positive: " + outsize);

                    return value / 100.0;
                }

                // Validate numeric format before conversion
                for (char c : outsize)
                    if (!str::isDigitChar(c) && c != '.' && c != '-' && c != '+')
                        throw InvalidArgsException("Invalid numeric format: " + outsize);

                double value = std::stod(outsize);
                if (value <= 0)
                    throw InvalidArgsException("Size must be positive: " + outsize);

                return value / width;
            } catch (const std::invalid_argument&) {
                throw InvalidArgsException("Invalid size format: " + outsize);
            } catch (const std::out_of_range&) {
                throw InvalidArgsException("Size value out of range: " + outsize);
            }
        }

        // Adds the -outsize arguments of `outsize` for an image `width` pixels wide and `height`
        // high and returns the scale it stands for
        double appendOutsize(CPLStringList &targs, const std::string &outsize, int width, int height)
        {
            const double ratio = outsizeRatio(outsize, width);

            targs.AddString("-outsize");
            targs.AddString(outsize.c_str());
            if (outsize.back() == '%')
                targs.AddString(outsize.c_str());
            else
                targs.AddString(std::format("{:.6f}", ratio * height).c_str());
            return ratio;
        }

        // One -gcp: the pixel (column, row) of the scaled image is at the ground point
        void appendGcp(CPLStringList &targs, int column, int row, const std::array<double, 2> &ground)
        {
            targs.AddString("-gcp");
            targs.AddString(std::to_string(column).c_str());
            targs.AddString(std::to_string(row).c_str());
            targs.AddString(std::format("{:.13f}", ground[0]).c_str());
            targs.AddString(std::format("{:.13f}", ground[1]).c_str());
        }

        // The image as a MEM dataset that carries the four GCPs of its footprint (upper left,
        // lower left, lower right, upper right) and EPSG:4326; scaled to `outsize` when given
        GDALDatasetUniquePtr translateWithGcps(GDALDatasetH hSrc, const raster::GeoprojectParams &params,
                                               const fs::path &p, cancel::GdalProgress &progress)
        {
            const int width = params.width;
            const int height = params.height;
            const std::string &outsize = params.outsize;

            // The intermediate (GCPs + SRS) stays in a MEM dataset: no /vsimem file to name or unlink
            CPLStringList targs;
            targs.AddString("-of");
            targs.AddString("MEM");
            targs.AddString("-a_srs");
            targs.AddString("EPSG:4326");

            int scaledWidth = width;
            int scaledHeight = height;

            if (outsize.length() > 0)
            {
                const double ratio = appendOutsize(targs, outsize, width, height);

                scaledWidth = static_cast<int>(static_cast<double>(width) * ratio);
                scaledHeight = static_cast<int>(static_cast<double>(height) * ratio);

                LOGD << "Scaled width: " << scaledWidth;
                LOGD << "Scaled height: " << scaledHeight;
            }

            appendGcp(targs, 0, 0, params.footprint[0]);
            appendGcp(targs, 0, scaledHeight, params.footprint[1]);
            appendGcp(targs, scaledWidth, scaledHeight, params.footprint[2]);
            appendGcp(targs, scaledWidth, 0, params.footprint[3]);

            const GdalOptionsPtr<GDALTranslateOptions> psOptions(
                GDALTranslateOptionsNew(targs.List(), nullptr));
            if (!psOptions)
                throw GDALException("Cannot geoproject " + io::toUtf8(p) + ": invalid translate options");
            // Both steps stop when the call is canceled
            progress.attach(psOptions.get());

            auto dstDataset = adoptDataset(GDALTranslate("", hSrc, psOptions.get(), nullptr));
            if (!dstDataset)
            {
                cancel::throwIfCanceled();
                throw GDALException("Cannot geoproject " + io::toUtf8(p) + ": " + CPLGetLastErrorMsg());
            }
            return dstDataset;
        }

        // Runs gdalwarp on the GCP dataset to add transparency and apply the GCPs, into tmpOutFile
        // (JPEG-compressed GeoTIFF with an alpha band)
        void warpToFile(GDALDatasetUniquePtr &srcDataset, GDALDatasetUniquePtr &dstDataset,
                        const std::string &tmpOutFile, const fs::path &p, cancel::GdalProgress &progress)
        {
            CPLStringList wargs;
            wargs.AddString("-of");
            wargs.AddString("GTiff");
            GtiffOptions().compress(GtiffOptions::Codec::Jpeg).appendTo(wargs);
            wargs.AddString("-dstalpha");
            const GdalOptionsPtr<GDALWarpAppOptions> waOptions(
                GDALWarpAppOptionsNew(wargs.List(), nullptr));
            if (!waOptions)
                throw GDALException("Cannot geoproject " + io::toUtf8(p) + ": invalid warp options");
            progress.attach(waOptions.get());

            GDALDatasetH hDstDataset = datasetHandle(dstDataset);
            auto wrpDataset = adoptDataset(GDALWarp(tmpOutFile.c_str(),
                                                   nullptr,
                                                   1,
                                                   &hDstDataset,
                                                   waOptions.get(),
                                                   nullptr));

            srcDataset.reset();
            dstDataset.reset();

            // Check before flush/close: GDALFlushCache(nullptr) logs a misleading
            // "NULL pointer" error that clobbers the real warp failure message
            if (!wrpDataset)
            {
                io::assureIsRemoved(io::fromUtf8(tmpOutFile));
                cancel::throwIfCanceled();
                throw GDALException("Cannot geoproject " + io::toUtf8(p) + ": " + CPLGetLastErrorMsg());
            }

            // The close writes what GDAL still holds: a failure leaves a partial file
            const auto closed = closeOutput(std::move(wrpDataset));
            if (closed.failed)
            {
                io::assureIsRemoved(io::fromUtf8(tmpOutFile));
                throw GDALException("Cannot geoproject " + io::toUtf8(p) + ": " + closed.message);
            }
        }

    } // namespace

    namespace raster
    {

        void geoprojectImage(const fs::path &path, const fs::path &outFile, const GeoprojectParams &params)
        {
            // Owned: the outsize parsing in translateWithGcps throws InvalidArgsException after the open
            auto srcDataset = openUserRaster(io::toUtf8(path));
            if (!srcDataset)
                throw RasterOpException(ErrorCode::Unreadable, "Cannot project " + io::toUtf8(path) + ", cannot open raster");

            cancel::GdalProgress progress;
            auto dstDataset = translateWithGcps(datasetHandle(srcDataset), params, path, progress);
            warpToFile(srcDataset, dstDataset, io::toUtf8(outFile), path, progress);
        }

    } // namespace raster

    void geoProject(const std::vector<std::string> &images, const std::string &output, const std::string &outsize, bool stopOnError, const GeoProjectCallback &callback)
    {
        const bool outputToDir = prepareOutput(images, output);

        for (const std::string &img : images)
        {
            fs::path p = io::fromUtf8(img);
            if (!fs::exists(p))
            {
                throw FSException("Cannot project " + io::toUtf8(p) + " (does not exist)");
            }

            Entry e;
            parseEntry(p, ".", e, false);

            if (e.type != EntryType::GeoImage)
            {
                if (stopOnError)
                    throw FSException("Cannot geoproject " + io::toUtf8(p) + ", not a GeoImage");
                else
                    LOGW << "Cannot geoproject " << io::toUtf8(p) << ", not a GeoImage, skipping...";
                continue;
            }
            if (e.polygon_geom.size() < 4 || e.properties.find("width") == e.properties.end() || e.properties.find("height") == e.properties.end())
            {
                if (stopOnError)
                    throw FSException("Cannot geoproject " + io::toUtf8(p) + ", the image does not have sufficient information");
                else
                    LOGW << "Cannot geoproject " << io::toUtf8(p) << ", the image does not have sufficient information: skipping";
                continue;
            }

            std::string outFile;
            if (outputToDir)
            {
                outFile = io::toUtf8(io::fromUtf8(output) / p.filename().replace_extension(".tif"));
            }
            else
            {
                outFile = output;
            }

            // The position of the picture comes from its EXIF, read here: the helper reads none
            raster::GeoprojectParams params;
            params.width = e.properties["width"].get<int>();
            params.height = e.properties["height"].get<int>();
            for (int corner = 0; corner < 4; corner++)
            {
                const Point ground = e.polygon_geom.getPoint(corner);
                params.footprint[corner] = {ground.x, ground.y};
            }
            params.outsize = outsize;
            // Fail on a size that makes no sense before any process is started
            if (!outsize.empty())
                outsizeRatio(outsize, params.width);

            // The helper writes the GeoTIFF into a folder only this call writes to; it is
            // moved to its name once it is known to be a GeoTIFF (publishing it is one rename, so
            // processes that share a cache never see half of a file or clobber each other)
            const io::ScopedTempDir request = raster::makeArtefactRequestFolder("geoproject-");
            const fs::path produced = request.path() / "geoprojected.tif";
            try
            {
                raster::rasterHelper().geoproject(p, produced, raster::toJson(params));
            }
            catch (const raster::RasterOpException &failure)
            {
                // A failure of the helper itself is not a picture that cannot be opened
                if (failure.code() == raster::ErrorCode::Internal)
                    throw;
                if (stopOnError)
                    throw FSException("Cannot project " + io::toUtf8(p) + ", cannot open raster");
                LOGW << "Cannot project " << io::toUtf8(p) << ", cannot open raster: skipping";
                continue;
            }

            if (!raster::openArtefactRaster(produced))
                throw GDALException("Cannot geoproject " + io::toUtf8(p) + ": " + raster::kGdalUtilName +
                                    " wrote a file that is not a GeoTIFF");
            raster::publishArtefact(produced, io::fromUtf8(outFile));

            if (callback)
            {
                if (!callback(outFile))
                    return;
            }
        }
    }

}
