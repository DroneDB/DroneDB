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
#include "raster_artefacts.h"
#include "raster_helper.h"
#include "raster_ops.h"

namespace ddb
{

    namespace
    {

        // Whether the images go to a folder: there are several, or the output is a folder or
        // is named as one (a trailing separator)
        bool outputIsFolder(const std::vector<std::string> &images, const std::string &output)
        {
            const bool namedAsFolder = !output.empty() && (output.back() == '/' || output.back() == '\\');
            const bool outputToDir = images.size() > 1 || namedAsFolder || fs::is_directory(io::fromUtf8(output));
            // Bad input?
            if (outputToDir && fs::is_regular_file(io::fromUtf8(output)))
                throw FSException(output + " is a file. (Did you switch the input and output parameters?)");
            return outputToDir;
        }

        // The file each image is projected to: `output`, or <output>/<image name>.tif
        std::vector<std::pair<fs::path, fs::path>> projectedFiles(const std::vector<std::string> &images,
                                                                  const std::string &output, bool outputToDir)
        {
            std::vector<std::pair<fs::path, fs::path>> targets;
            targets.reserve(images.size());
            for (const std::string &img : images)
            {
                const fs::path p = io::fromUtf8(img);
                targets.emplace_back(p, outputToDir ? io::fromUtf8(output) / p.filename().replace_extension(".tif")
                                                    : io::fromUtf8(output));
            }
            return targets;
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

    } // namespace

    namespace raster
    {

        void geoprojectImage(const fs::path &path, const fs::path &outFile, const GeoprojectParams &params)
        {
            // Placed on the ground by its corners as ground control points and warped in their
            // CRS, as gdalwarp warps a photo that gdal_translate gave its GCPs (and -outsize)
            const double scale = params.outsize.empty() ? 1.0 : outsizeRatio(params.outsize, params.width);
            json options = {{"gcps", photoGcps(params, scale)}, {"gcpSrs", "EPSG:4326"}, {"targetSrs", "source"}};
            if (scale != 1.0)
                options["outScale"] = scale;
            buildWithTilers(path, outFile, std::nullopt, std::move(options), {}, [&path](const std::string &message) {
                throw RasterOpException(ErrorCode::Unreadable, "Cannot project " + io::toUtf8(path) + ": " + message);
            });
        }

    } // namespace raster

    std::optional<raster::PhotoGeoreference> photoGeoreferenceOf(const fs::path &photo, std::string &problem)
    {
        Entry e;
        parseEntry(photo, ".", e, false);
        if (e.type != EntryType::GeoImage)
        {
            problem = "not a GeoImage";
            return std::nullopt;
        }
        if (e.polygon_geom.size() < 4 || e.properties.find("width") == e.properties.end() ||
            e.properties.find("height") == e.properties.end())
        {
            problem = "the image does not have sufficient information";
            return std::nullopt;
        }
        raster::PhotoGeoreference result;
        result.width = e.properties["width"].get<int>();
        result.height = e.properties["height"].get<int>();
        for (size_t corner = 0; corner < result.footprint.size(); corner++)
        {
            const Point ground = e.polygon_geom.getPoint(static_cast<int>(corner));
            result.footprint[corner] = {ground.x, ground.y};
        }
        return result;
    }

    void geoProject(const std::vector<std::string> &images, const std::string &output, const std::string &outsize, bool stopOnError, const GeoProjectCallback &callback,
                    bool overwrite)
    {
        const bool outputToDir = outputIsFolder(images, output);

        // Every projected file before the first is written, then the folder they go to
        const auto targets = projectedFiles(images, output, outputToDir);
        io::assureReplaceable(targets, overwrite);
        if (outputToDir)
            io::createDirectories(io::fromUtf8(output));

        for (const auto &[p, target] : targets)
        {
            if (!fs::exists(p))
            {
                throw FSException("Cannot project " + io::toUtf8(p) + " (does not exist)");
            }

            // The position of the picture comes from its EXIF, read here: the helper reads none
            std::string problem;
            const auto photo = photoGeoreferenceOf(p, problem);
            if (!photo)
            {
                if (stopOnError)
                    throw FSException("Cannot geoproject " + io::toUtf8(p) + ", " + problem);
                LOGW << "Cannot geoproject " << io::toUtf8(p) << ", " << problem << ": skipping";
                continue;
            }

            const std::string outFile = io::toUtf8(target);
            raster::GeoprojectParams params;
            static_cast<raster::PhotoGeoreference &>(params) = *photo;
            params.outsize = outsize;
            // Fail on a size that makes no sense before any process is started
            if (!outsize.empty())
                outsizeRatio(outsize, params.width);

            // The helper writes the GeoTIFF into a folder only this call writes to; it is
            // moved to its name once it is known to be a GeoTIFF (publishing it is one rename, so
            // processes that share a cache never see half of a file or clobber each other)
            const raster::RequestFolder request("geoproject-");
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

            if (!raster::writtenRasterOpens(produced))
                throw GDALException("Cannot geoproject " + io::toUtf8(p) + ": " + raster::kGdalUtilName +
                                    " wrote a file that is not a GeoTIFF");
            // The output may have appeared while it was projected: replaced only when asked
            io::assureReplaceable(target, overwrite);
            raster::publishArtefact(produced, io::fromUtf8(outFile));

            if (callback)
            {
                if (!callback(outFile))
                    return;
            }
        }
    }

}
