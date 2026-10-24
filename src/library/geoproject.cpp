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
#include "gdal_raii.h"
#include "gtiff_options.h"

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

        // Adds the -outsize arguments of `outsize` ("N" pixels of width, or "N%") for an image
        // `width` pixels wide and returns the scale it stands for. Throws InvalidArgsException
        // for text that is not a positive number.
        double appendOutsize(CPLStringList &targs, const std::string &outsize, int width, int height)
        {
            double ratio = 1.0;

            targs.AddString("-outsize");
            targs.AddString(outsize.c_str());
            try {
                if (outsize.back() == '%')
                {
                    targs.AddString(outsize.c_str());

                    // Validate percentage format before conversion
                    std::string percentValue = outsize.substr(0, outsize.length() - 1);
                    for (char c : percentValue)
                        if (!str::isDigitChar(c) && c != '.' && c != '-' && c != '+')
                            throw InvalidArgsException("Invalid percentage format: " + outsize);

                    double value = std::stod(percentValue);
                    if (value <= 0)
                        throw InvalidArgsException("Percentage must be positive: " + outsize);

                    ratio = value / 100.0;
                }
                else
                {
                    // Validate numeric format before conversion
                    for (char c : outsize)
                        if (!str::isDigitChar(c) && c != '.' && c != '-' && c != '+')
                            throw InvalidArgsException("Invalid numeric format: " + outsize);

                    double value = std::stod(outsize);
                    if (value <= 0)
                        throw InvalidArgsException("Size must be positive: " + outsize);

                    ratio = value / width;
                    targs.AddString(std::format("{:.6f}", ratio * height).c_str());
                }
            } catch (const std::invalid_argument&) {
                throw InvalidArgsException("Invalid size format: " + outsize);
            } catch (const std::out_of_range&) {
                throw InvalidArgsException("Size value out of range: " + outsize);
            }
            return ratio;
        }

        // One -gcp: the pixel (column, row) of the scaled image is at the ground point
        void appendGcp(CPLStringList &targs, int column, int row, const Point &ground)
        {
            targs.AddString("-gcp");
            targs.AddString(std::to_string(column).c_str());
            targs.AddString(std::to_string(row).c_str());
            targs.AddString(std::format("{:.13f}", ground.x).c_str());
            targs.AddString(std::format("{:.13f}", ground.y).c_str());
        }

        // The image as a MEM dataset that carries the four GCPs of its footprint (upper left,
        // lower left, lower right, upper right) and EPSG:4326; scaled to `outsize` when given
        GDALDatasetUniquePtr translateWithGcps(GDALDatasetH hSrc, Entry &e, const fs::path &p,
                                               const std::string &outsize, cancel::GdalProgress &progress)
        {
            const int width = e.properties["width"].get<int>();
            const int height = e.properties["height"].get<int>();

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

            appendGcp(targs, 0, 0, e.polygon_geom.getPoint(0));
            appendGcp(targs, 0, scaledHeight, e.polygon_geom.getPoint(1));
            appendGcp(targs, scaledWidth, scaledHeight, e.polygon_geom.getPoint(2));
            appendGcp(targs, scaledWidth, 0, e.polygon_geom.getPoint(3));

            const GdalOptionsPtr<GDALTranslateOptions> psOptions(
                GDALTranslateOptionsNew(targs.List(), nullptr));
            if (!psOptions)
                throw GDALException("Cannot geoproject " + io::toUtf8(p) + ": invalid translate options");
            // Both steps stop when the call is canceled (DQ-15)
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

            // Unique per call so concurrent processes sharing a cache don't clobber each other
            std::string tmpOutFile = io::toUtf8(io::fromUtf8(outFile).replace_extension("." + randomString(8) + ".tif.tmp"));

            // Owned: the outsize parsing below throws InvalidArgsException after the open
            auto srcDataset = adoptDataset(GDALOpen(io::toUtf8(p).c_str(), GA_ReadOnly));
            if (!srcDataset)
            {
                if (stopOnError)
                    throw FSException("Cannot project " + io::toUtf8(p) + ", cannot open raster");
                else
                    LOGW << "Cannot project " << io::toUtf8(p) << ", cannot open raster: skipping";
                continue;
            }

            cancel::GdalProgress progress;
            auto dstDataset = translateWithGcps(datasetHandle(srcDataset), e, p, outsize, progress);
            warpToFile(srcDataset, dstDataset, tmpOutFile, p, progress);

            io::rename(io::fromUtf8(tmpOutFile), io::fromUtf8(outFile));

            if (callback)
            {
                if (!callback(outFile))
                    return;
            }
        }
    }

}
