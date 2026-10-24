/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "json.h"
#include "merge_multispectral.h"

namespace ddb {

std::string mergeValidationToJson(const MergeValidationResult &r) {
    json j;
    j["ok"] = r.ok;
    j["errors"] = r.errors;
    j["warnings"] = r.warnings;
    j["summary"] = {
        {"totalBands", r.summary.totalBands},
        {"dataType", r.summary.dataType},
        {"width", r.summary.width},
        {"height", r.summary.height},
        {"crs", r.summary.crs},
        {"pixelSizeX", r.summary.pixelSizeX},
        {"pixelSizeY", r.summary.pixelSizeY},
        {"estimatedSize", r.summary.estimatedSize}
    };

    // Alignment info
    json bands = json::array();
    for (size_t i = 0; i < r.alignment.bands.size(); i++) {
        const auto &b = r.alignment.bands[i];
        bands.push_back({
            {"index", i},
            {"name", b.bandName},
            {"wavelength", b.centralWavelength},
            {"shiftX", b.shiftX},
            {"shiftY", b.shiftY}
        });
    }
    j["alignment"] = {
        {"detected", r.alignment.detected},
        {"maxShiftPixels", r.alignment.maxShiftPixels},
        {"correctionApplied", r.alignment.correctionApplied},
        {"shiftSource", r.alignment.shiftSource},
        {"bands", bands}
    };
    return j.dump();
}

}  // namespace ddb
