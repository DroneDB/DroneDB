/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "cancellation.h"
#include "dbops.h"
#include "geoproject.h"
#include "mio.h"
#include "utils.h"
#include "gdal_raii.h"

namespace ddb
{

    void geoProject(const std::vector<std::string> &images, const std::string &output, const std::string &outsize, bool stopOnError, const GeoProjectCallback &callback)
    {
        bool isDirectory = fs::is_directory(output);
        bool outputToDir = images.size() > 1 || isDirectory;
        if (outputToDir)
        {
            if (!isDirectory)
            {
                // Bad input?
                if (fs::is_regular_file(output))
                {
                    throw FSException(output + " is a file. (Did you switch the input and output parameters?)");
                }
                io::createDirectories(output);
            }
        }

        for (const std::string &img : images)
        {
            fs::path p = img;
            if (!fs::exists(p))
            {
                throw FSException("Cannot project " + p.string() + " (does not exist)");
            }

            Entry e;
            parseEntry(p, ".", e, false);

            if (e.type != EntryType::GeoImage)
            {
                if (stopOnError)
                    throw FSException("Cannot geoproject " + p.string() + ", not a GeoImage");
                else
                    std::cerr << "Cannot geoproject " << p.string() << ", not a GeoImage, skipping..." << std::endl;
                continue;
            }
            if (e.polygon_geom.size() < 4 || e.properties.find("width") == e.properties.end() || e.properties.find("height") == e.properties.end())
            {
                if (stopOnError)
                    throw FSException("Cannot geoproject " + p.string() + ", the image does not have sufficient information");
                else
                    std::cerr << "Cannot geoproject " << p.string() << ", the image does not have sufficient information: skipping" << std::endl;
                continue;
            }

            int width = e.properties["width"].get<int>();
            int height = e.properties["height"].get<int>();

            std::string outFile;
            if (outputToDir)
            {
                outFile = (fs::path(output) / p.filename().replace_extension(".tif")).string();
            }
            else
            {
                outFile = output;
            }

            // Unique per call so concurrent processes sharing a cache don't clobber each other
            std::string tmpOutFile = fs::path(outFile).replace_extension("." + utils::generateRandomString(8) + ".tif.tmp").string();

            Point ul = e.polygon_geom.getPoint(0);
            Point ll = e.polygon_geom.getPoint(1);
            Point lr = e.polygon_geom.getPoint(2);
            Point ur = e.polygon_geom.getPoint(3);

            // Owned: the outsize parsing below throws InvalidArgsException after the open
            auto srcDataset = adoptDataset(GDALOpen(p.string().c_str(), GA_ReadOnly));
            if (!srcDataset)
            {
                if (stopOnError)
                    throw FSException("Cannot project " + p.string() + ", cannot open raster");
                else
                    std::cout << "Cannot project " << p.string() << ", cannot open raster: skipping" << std::endl;
                continue;
            }

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
                            if (!utils::isDigitChar(c) && c != '.' && c != '-' && c != '+')
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
                            if (!utils::isDigitChar(c) && c != '.' && c != '-' && c != '+')
                                throw InvalidArgsException("Invalid numeric format: " + outsize);

                        double value = std::stod(outsize);
                        if (value <= 0)
                            throw InvalidArgsException("Size must be positive: " + outsize);

                        ratio = value / width;
                        targs.AddString(utils::toStr(ratio * height).c_str());
                    }
                } catch (const std::invalid_argument& e) {
                    throw InvalidArgsException("Invalid size format: " + outsize);
                } catch (const std::out_of_range& e) {
                    throw InvalidArgsException("Size value out of range: " + outsize);
                }

                scaledWidth = static_cast<int>(static_cast<double>(width) * ratio);
                scaledHeight = static_cast<int>(static_cast<double>(height) * ratio);

                LOGD << "Scaled width: " << scaledWidth;
                LOGD << "Scaled height: " << scaledHeight;
            }

            targs.AddString("-gcp");
            targs.AddString("0");
            targs.AddString("0");
            targs.AddString(utils::toStr(ul.x, 13).c_str());
            targs.AddString(utils::toStr(ul.y, 13).c_str());

            targs.AddString("-gcp");
            targs.AddString("0");
            targs.AddString(std::to_string(scaledHeight).c_str());
            targs.AddString(utils::toStr(ll.x, 13).c_str());
            targs.AddString(utils::toStr(ll.y, 13).c_str());

            targs.AddString("-gcp");
            targs.AddString(std::to_string(scaledWidth).c_str());
            targs.AddString(std::to_string(scaledHeight).c_str());
            targs.AddString(utils::toStr(lr.x, 13).c_str());
            targs.AddString(utils::toStr(lr.y, 13).c_str());

            targs.AddString("-gcp");
            targs.AddString(std::to_string(scaledWidth).c_str());
            targs.AddString("0");
            targs.AddString(utils::toStr(ur.x, 13).c_str());
            targs.AddString(utils::toStr(ur.y, 13).c_str());

            const GdalOptionsPtr<GDALTranslateOptions> psOptions(
                GDALTranslateOptionsNew(targs.List(), nullptr));
            if (!psOptions)
                throw GDALException("Cannot geoproject " + p.string() + ": invalid translate options");
            // Both steps stop when the call is canceled (DQ-15)
            cancel::GdalProgress progress;
            progress.attach(psOptions.get());

            auto dstDataset = adoptDataset(
                GDALTranslate("", datasetHandle(srcDataset), psOptions.get(), nullptr));
            if (!dstDataset)
            {
                cancel::throwIfCanceled();
                throw GDALException("Cannot geoproject " + p.string() + ": " + CPLGetLastErrorMsg());
            }

            // Run gdalwarp to add trasparency, apply GCPs
            CPLStringList wargs;
            wargs.AddString("-of");
            wargs.AddString("GTiff");
            wargs.AddString("-co");
            wargs.AddString("COMPRESS=JPEG");
            wargs.AddString("-dstalpha");
            const GdalOptionsPtr<GDALWarpAppOptions> waOptions(
                GDALWarpAppOptionsNew(wargs.List(), nullptr));
            if (!waOptions)
                throw GDALException("Cannot geoproject " + p.string() + ": invalid warp options");
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
                io::assureIsRemoved(tmpOutFile);
                cancel::throwIfCanceled();
                throw GDALException("Cannot geoproject " + p.string() + ": " + CPLGetLastErrorMsg());
            }

            GDALFlushCache(datasetHandle(wrpDataset));
            wrpDataset.reset();

            io::rename(tmpOutFile, outFile);

            if (callback)
            {
                if (!callback(outFile))
                    return;
            }
        }
    }

}
