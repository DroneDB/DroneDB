/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "cog_build_policy.h"

#include <fstream>
#include <string>

#include "exceptions.h"
#include "json.h"
#include "mio.h"
#include "tilers_raster.h"

namespace ddb {

namespace {

constexpr const char* kAlpha = "Alpha";
constexpr const char* kPalette = "Palette";

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

RasterTransparency CogBuildPolicy::ofBuiltCog(const fs::path& cog) {
    // Opened for this check alone: the build that follows an outdated COG replaces the file
    const auto raster = tilers::Raster::openPrivate(cog);
    if (!raster)
        throw AppException("Cannot read the COG " + io::toUtf8(cog));
    RasterTransparency t;
    for (int b = 1; b <= raster->bandCount(); b++) {
        const auto& band = raster->band(b);
        if (band.colorInterp == kAlpha)
            t.alpha = true;
        else if (band.nodata)
            t.noDataBands++;
    }
    t.palette = raster->bandCount() > 0 && !raster->band(1).colorTable.empty();
    t.mask = raster->hasDatasetMask();
    return t;
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
