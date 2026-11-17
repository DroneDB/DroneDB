/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "cog_build_policy.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#include "coordstransformer.h"
#include "exceptions.h"
#include "json.h"
#include "mio.h"
#include "tilers_raster.h"

namespace ddb {

namespace {

constexpr const char* kAlpha = "Alpha";
constexpr const char* kPalette = "Palette";
// The latitude where the square of Web Mercator ends (EPSG:3857), in degrees
constexpr double kWebMercatorMaxLatitude = 85.0511287798066;

// The colour interpretation of a band of the `bands` property of an entry; "" when it has none
std::string colorInterpOf(const json& band) {
    const auto it = band.is_object() ? band.find("colorInterp") : band.end();
    return it != band.end() && it->is_string() ? it->get<std::string>() : std::string();
}

}  // namespace

bool CogBuildPolicy::outdates(const RasterTransparency& source) {
    if (source.palette)
        return true;
    if (source.alpha)
        return false;
    const bool noTransparency = !source.mask && source.noDataBands == 0;
    return noTransparency || source.noDataBands > 1;
}

std::optional<RasterTransparency> CogBuildPolicy::indexed(const json& properties) {
    if (!properties.is_object())
        return std::nullopt;
    const auto bands = properties.find("bands");
    if (bands == properties.end() || !bands->is_array() || bands->empty())
        return std::nullopt;

    RasterTransparency t;
    t.palette = colorInterpOf(bands->front()) == kPalette;
    for (const json& band : *bands) {
        if (colorInterpOf(band) == kAlpha)
            t.alpha = true;
        else if (band.is_object() && band.contains("noData"))
            t.noDataBands++;
    }
    if (t.palette || t.alpha)
        return t;

    const auto mask = properties.find("hasMask");
    if (mask == properties.end() || !mask->is_boolean())
        return std::nullopt;
    t.mask = mask->get<bool>();
    return t;
}

BuiltCog CogBuildPolicy::ofBuiltCog(const fs::path& cog) {
    // Opened for this check alone: the build that follows an outdated COG replaces the file
    const auto raster = tilers::Raster::openPrivate(cog);
    if (!raster)
        throw AppException("Cannot read the COG " + io::toUtf8(cog));
    const auto& gt = raster->geoTransform();
    if (!gt)
        throw AppException("The COG " + io::toUtf8(cog) + " has no geotransform");

    BuiltCog built;
    RasterTransparency& t = built.transparency;
    for (int b = 1; b <= raster->bandCount(); b++) {
        const auto& band = raster->band(b);
        if (band.colorInterp == kAlpha)
            t.alpha = true;
        else if (band.nodata)
            t.noDataBands++;
    }
    t.palette = raster->bandCount() > 0 && !raster->band(1).colorTable.empty();
    t.mask = raster->hasDatasetMask();

    built.wkt = raster->wkt();
    // The box of the corners of the grid (a geotransform may rotate it)
    const auto& g = *gt;
    const double w = raster->width(), h = raster->height();
    const double xs[4] = {g[0], g[0] + w * g[1], g[0] + h * g[2], g[0] + w * g[1] + h * g[2]};
    const double ys[4] = {g[3], g[3] + w * g[4], g[3] + h * g[5], g[3] + w * g[4] + h * g[5]};
    built.extent = {*std::min_element(xs, xs + 4), *std::min_element(ys, ys + 4), *std::max_element(xs, xs + 4),
                    *std::max_element(ys, ys + 4)};
    built.pixelSize = std::max(std::hypot(g[1], g[4]), std::hypot(g[2], g[5]));
    return built;
}

bool CogBuildPolicy::misplaces(const BuiltCog& cog, const BasicPolygonGeometry& footprint) {
    if (footprint.points.empty())
        return false;
    std::vector<double> xs, ys;
    for (const auto& p : footprint.points) {
        xs.push_back(p.x);
        ys.push_back(std::clamp(p.y, -kWebMercatorMaxLatitude, kWebMercatorMaxLatitude));
    }
    CoordsTransformer toCog(4326, cog.wkt);
    if (!toCog.tryTransform(xs.size(), xs.data(), ys.data()))
        throw AppException("The footprint cannot be placed in the CRS of the COG");
    const double tolerance = kPlacementTolerancePixels * cog.pixelSize;
    const auto& [minX, minY, maxX, maxY] = cog.extent;
    for (size_t i = 0; i < xs.size(); i++)
        if (xs[i] < minX - tolerance || xs[i] > maxX + tolerance || ys[i] < minY - tolerance ||
            ys[i] > maxY + tolerance)
            return true;
    return false;
}

void writeCogBuildRecord(const fs::path& file, const CogBuildRecord& record) {
    io::writeFileAtomic(file, json{{"version", record.version}}.dump(), "COG build record");
}

std::optional<CogBuildRecord> readCogBuildRecord(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return std::nullopt;
    const json j = json::parse(in, nullptr, false);
    const auto version = j.is_object() ? j.find("version") : j.end();
    if (!j.is_object() || version == j.end() || !version->is_number_integer())
        return std::nullopt;
    return CogBuildRecord{version->get<int>()};
}

}  // namespace ddb
