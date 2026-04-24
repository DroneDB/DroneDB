/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "sensorprofile.h"
#include "gdal_inc.h"
#include "exceptions.h"
#include "logger.h"
#include "mio.h"

#include <fstream>
#include <algorithm>
#include <sstream>
#include <regex>

namespace ddb {

// --- BandMapping ---

BandMapping BandMapping::Parse(const std::string &s) {
    BandMapping bm;
    // Accept both "4-3-2" and "4,3,2"
    char sep = (s.find(',') != std::string::npos) ? ',' : '-';
    std::istringstream iss(s);
    std::string token;
    std::vector<int> vals;
    while (std::getline(iss, token, sep)) {
        vals.push_back(std::stoi(token));
    }
    if (vals.size() >= 3) {
        bm.r = vals[0];
        bm.g = vals[1];
        bm.b = vals[2];
    }
    return bm;
}

std::string BandMapping::toInternalString() const {
    return std::to_string(r) + "-" + std::to_string(g) + "-" + std::to_string(b);
}

std::string BandMapping::toApiString() const {
    return std::to_string(r) + "," + std::to_string(g) + "," + std::to_string(b);
}

// --- JSON serialization ---

void to_json(json &j, const BandMapping &bm) {
    j = json{{"r", bm.r}, {"g", bm.g}, {"b", bm.b}};
}

void from_json(const json &j, BandMapping &bm) {
    j.at("r").get_to(bm.r);
    j.at("g").get_to(bm.g);
    j.at("b").get_to(bm.b);
}

void to_json(json &j, const BandInfo &bi) {
    j = json{{"index", bi.index}, {"name", bi.name}};
    if (bi.wavelength > 0) j["wavelength"] = bi.wavelength;
    if (bi.resolution > 0) j["resolution"] = bi.resolution;
    if (!bi.domain.empty()) j["domain"] = bi.domain;
    if (!bi.description.empty()) j["description"] = bi.description;
}

void from_json(const json &j, BandInfo &bi) {
    j.at("index").get_to(bi.index);
    j.at("name").get_to(bi.name);
    if (j.contains("wavelength")) j.at("wavelength").get_to(bi.wavelength);
    if (j.contains("resolution")) j.at("resolution").get_to(bi.resolution);
    if (j.contains("domain")) j.at("domain").get_to(bi.domain);
    if (j.contains("description")) j.at("description").get_to(bi.description);
}

void to_json(json &j, const RenderPreset &rp) {
    j = json{{"id", rp.id}, {"name", rp.name}};
    if (!rp.description.empty()) j["description"] = rp.description;
    if (rp.type == "bands" || rp.type.empty()) {
        j["bandMapping"] = rp.bandMapping;
    }
    j["isDefault"] = rp.isDefault;
    if (!rp.type.empty()) j["type"] = rp.type;
    if (!rp.stretch.empty()) j["stretch"] = rp.stretch;
    if (!rp.formula.empty()) j["formula"] = rp.formula;
    if (!rp.colormap.empty()) j["colormap"] = rp.colormap;
}

void from_json(const json &j, RenderPreset &rp) {
    j.at("id").get_to(rp.id);
    j.at("name").get_to(rp.name);
    if (j.contains("description")) j.at("description").get_to(rp.description);
    if (j.contains("bandMapping")) j.at("bandMapping").get_to(rp.bandMapping);
    if (j.contains("isDefault")) j.at("isDefault").get_to(rp.isDefault);
    if (j.contains("type")) j.at("type").get_to(rp.type);
    if (j.contains("stretch")) j.at("stretch").get_to(rp.stretch);
    if (j.contains("formula")) j.at("formula").get_to(rp.formula);
    if (j.contains("colormap")) j.at("colormap").get_to(rp.colormap);
}

void to_json(json &j, const DetectionCriteria &dc) {
    j = json{{"bandCount", dc.bandCount}, {"priority", dc.priority}};
    if (!dc.dataType.empty()) j["dataType"] = dc.dataType;
    if (!dc.metadataPatterns.empty()) j["metadataPatterns"] = dc.metadataPatterns;
}

void from_json(const json &j, DetectionCriteria &dc) {
    j.at("bandCount").get_to(dc.bandCount);
    if (j.contains("dataType")) j.at("dataType").get_to(dc.dataType);
    if (j.contains("metadataPatterns")) j.at("metadataPatterns").get_to(dc.metadataPatterns);
    if (j.contains("priority")) j.at("priority").get_to(dc.priority);
}

void to_json(json &j, const SensorProfile &sp) {
    j = json{
        {"id", sp.id}, {"name", sp.name}, {"sensorCategory", sp.sensorCategory},
        {"detection", sp.detection}, {"bands", sp.bands}, {"presets", sp.presets}
    };
    if (!sp.description.empty()) j["description"] = sp.description;
}

void from_json(const json &j, SensorProfile &sp) {
    j.at("id").get_to(sp.id);
    j.at("name").get_to(sp.name);
    if (j.contains("description")) j.at("description").get_to(sp.description);
    j.at("sensorCategory").get_to(sp.sensorCategory);
    j.at("detection").get_to(sp.detection);
    j.at("bands").get_to(sp.bands);
    j.at("presets").get_to(sp.presets);
}

// --- SensorProfileManager ---

SensorProfileManager& SensorProfileManager::instance() {
    static SensorProfileManager inst;
    return inst;
}

void SensorProfileManager::ensureLoaded() const {
    if (loaded_) return;

    // Lazy-load defaults (mutex already held by caller)
    std::string path;
    fs::path dataFile = io::getDataPath("sensor-profiles.json");
    path = dataFile.string();

    if (path.empty() || !fs::exists(path)) {
        LOGD << "Sensor profiles file not found, using empty profiles";
        loaded_ = true;
        return;
    }

    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        LOGW << "Cannot open sensor profiles file: " << path;
        loaded_ = true;
        return;
    }

    json root = json::parse(ifs);
    profiles_.clear();

    if (root.contains("profiles")) {
        for (const auto &pj : root["profiles"]) {
            SensorProfile sp = pj.get<SensorProfile>();
            profiles_.push_back(sp);
        }
    }

    std::sort(profiles_.begin(), profiles_.end(), [](const SensorProfile &a, const SensorProfile &b) {
        return a.detection.priority > b.detection.priority;
    });

    loaded_ = true;
    LOGD << "Lazy-loaded " << profiles_.size() << " sensor profiles";
}

void SensorProfileManager::loadDefaults(const std::string &jsonPath) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string path = jsonPath;
    if (path.empty()) {
        fs::path dataFile = io::getDataPath("sensor-profiles.json");
        path = dataFile.string();
    }

    if (path.empty() || !fs::exists(path)) {
        LOGD << "Sensor profiles file not found, using empty profiles";
        loaded_ = true;
        return;
    }

    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        throw AppException("Cannot open sensor profiles file: " + path);
    }

    json root = json::parse(ifs);
    profiles_.clear();

    if (root.contains("profiles")) {
        for (const auto &pj : root["profiles"]) {
            SensorProfile sp = pj.get<SensorProfile>();
            profiles_.push_back(sp);
        }
    }

    // Sort by priority descending
    std::sort(profiles_.begin(), profiles_.end(), [](const SensorProfile &a, const SensorProfile &b) {
        return a.detection.priority > b.detection.priority;
    });

    loaded_ = true;
    LOGD << "Loaded " << profiles_.size() << " sensor profiles";
}

void SensorProfileManager::loadOverrides(const std::string &jsonStr) {
    std::lock_guard<std::mutex> lock(mutex_);

    ensureLoaded();

    json root = json::parse(jsonStr);
    if (root.contains("profiles")) {
        for (const auto &pj : root["profiles"]) {
            SensorProfile sp = pj.get<SensorProfile>();
            // Override existing profile with same id
            auto it = std::find_if(profiles_.begin(), profiles_.end(),
                [&sp](const SensorProfile &p) { return p.id == sp.id; });
            if (it != profiles_.end()) {
                *it = sp;
            } else {
                profiles_.push_back(sp);
            }
        }
    }

    // Re-sort
    std::sort(profiles_.begin(), profiles_.end(), [](const SensorProfile &a, const SensorProfile &b) {
        return a.detection.priority > b.detection.priority;
    });
}

static std::string gdalDataTypeName(GDALDataType type) {
    switch (type) {
        case GDT_Byte: return "Byte";
        case GDT_UInt16: return "UInt16";
        case GDT_Int16: return "Int16";
        case GDT_UInt32: return "UInt32";
        case GDT_Int32: return "Int32";
        case GDT_Float32: return "Float32";
        case GDT_Float64: return "Float64";
        default: return "Unknown";
    }
}

bool SensorProfileManager::matchesProfile(const SensorProfile &profile, int bandCount,
                                           const std::string &dataType,
                                           const std::vector<std::string> &metadata,
                                           bool lastBandIsAlpha) const {
    // Band count check
    int effectiveBandCount = lastBandIsAlpha ? (bandCount - 1) : bandCount;
    if (profile.detection.bandCount != effectiveBandCount && profile.detection.bandCount != bandCount) {
        return false;
    }

    // Data type check (if specified)
    if (!profile.detection.dataType.empty() && profile.detection.dataType != dataType) {
        return false;
    }

    // Metadata patterns check (if specified, at least one must match)
    if (!profile.detection.metadataPatterns.empty()) {
        bool anyMatch = false;
        for (const auto &pattern : profile.detection.metadataPatterns) {
            for (const auto &md : metadata) {
                if (md.find(pattern) != std::string::npos) {
                    anyMatch = true;
                    break;
                }
            }
            if (anyMatch) break;
        }
        if (!anyMatch) return false;
    }

    return true;
}

SensorDetectionResult SensorProfileManager::detectSensor(const std::string &rasterPath) const {
    std::lock_guard<std::mutex> lock(mutex_);

    ensureLoaded();

    SensorDetectionResult result;

    GDALDatasetH hDataset = GDALOpen(rasterPath.c_str(), GA_ReadOnly);
    if (!hDataset) return result;

    int bandCount = GDALGetRasterCount(hDataset);
    if (bandCount == 0) {
        GDALClose(hDataset);
        return result;
    }

    // Get data type from first band
    GDALRasterBandH hBand1 = GDALGetRasterBand(hDataset, 1);
    GDALDataType dt = GDALGetRasterDataType(hBand1);
    std::string dataType = gdalDataTypeName(dt);

    // Check if last band is alpha
    bool lastBandIsAlpha = false;
    if (bandCount >= 2) {
        GDALRasterBandH hLastBand = GDALGetRasterBand(hDataset, bandCount);
        lastBandIsAlpha = (GDALGetRasterColorInterpretation(hLastBand) == GCI_AlphaBand);
    }

    // RGBA disambiguation: 4-band Byte with alpha → not multispectral
    if (bandCount == 4 && dt == GDT_Byte && lastBandIsAlpha) {
        GDALClose(hDataset);
        result.detected = false;
        return result;
    }

    // 3-band Byte with no wavelength metadata → standard RGB
    if (bandCount == 3 && dt == GDT_Byte) {
        GDALClose(hDataset);
        result.detected = false;
        return result;
    }

    // Collect metadata for matching
    std::vector<std::string> allMetadata;
    const char *mdItem = GDALGetMetadataItem(hDataset, "TIFFTAG_IMAGEDESCRIPTION", nullptr);
    if (mdItem) allMetadata.push_back(mdItem);
    mdItem = GDALGetMetadataItem(hDataset, "TIFFTAG_SOFTWARE", nullptr);
    if (mdItem) allMetadata.push_back(mdItem);

    // Collect all metadata domains
    char **mdDomains = GDALGetMetadataDomainList(hDataset);
    if (mdDomains) {
        for (int i = 0; mdDomains[i] != nullptr; i++) {
            char **md = GDALGetMetadata(hDataset, mdDomains[i]);
            if (md) {
                for (int k = 0; md[k] != nullptr; k++) {
                    allMetadata.push_back(md[k]);
                }
            }
        }
        CSLDestroy(mdDomains);
    }

    // Also check per-band metadata
    for (int b = 1; b <= bandCount; b++) {
        GDALRasterBandH hBand = GDALGetRasterBand(hDataset, b);
        char **bmd = GDALGetMetadata(hBand, nullptr);
        if (bmd) {
            for (int k = 0; bmd[k] != nullptr; k++) {
                allMetadata.push_back(bmd[k]);
            }
        }
    }

    // Try matching profiles (already sorted by priority descending)
    for (const auto &profile : profiles_) {
        if (profile.sensorCategory != "multispectral" && profile.sensorCategory != "thermal") continue;

        if (matchesProfile(profile, bandCount, dataType, allMetadata, lastBandIsAlpha)) {
            result.detected = true;
            result.sensorId = profile.id;
            result.sensorName = profile.name;
            result.sensorCategory = profile.sensorCategory;
            result.bands = profile.bands;
            result.presets = profile.presets;

            // For generic profiles (no metadata patterns, e.g. "generic-4band"),
            // prefer actual GDAL band descriptions when available, since the
            // profile-defined names are only placeholders. This avoids
            // mis-labeling bands of files that happen to match by band count
            // alone but whose actual band order differs (e.g. ODM multispectral
            // orthophoto with Red/Green/NIR/Rededge would otherwise be shown as
            // Blue/Green/Red/NIR from generic-4band).
            if (profile.detection.metadataPatterns.empty()) {
                for (auto &bi : result.bands) {
                    if (bi.index < 1 || bi.index > bandCount) continue;
                    GDALRasterBandH hB = GDALGetRasterBand(hDataset, bi.index);
                    const char *d = GDALGetDescription(hB);
                    if (d != nullptr && *d != '\0') {
                        bi.name = d;
                    }
                }
            }

            // Find default preset
            for (const auto &p : profile.presets) {
                if (p.isDefault) {
                    result.defaultBandMapping = p.bandMapping;
                    result.defaultPresetId = p.id;
                    break;
                }
            }
            // If no default marked, use the first bands-type preset
            if (result.defaultPresetId.empty() && !profile.presets.empty()) {
                for (const auto &p : profile.presets) {
                    if (p.type == "bands" || p.type.empty()) {
                        result.defaultBandMapping = p.bandMapping;
                        result.defaultPresetId = p.id;
                        break;
                    }
                }
            }

            GDALClose(hDataset);
            return result;
        }
    }

    // No profile matched — for multi-band non-Byte images, return a basic result
    // with fallback mapping (§3.3 Detection Fallback).
    // Populate result.bands from GDAL descriptions / color interpretation so that
    // downstream consumers (getRasterInfoJson, autoDetectFilter) can work with
    // the detected sensor even without a matching profile.
    if (bandCount > 3 && dt != GDT_Byte) {
        result.detected = true;
        result.sensorCategory = "multispectral";

        int lastDataBand = lastBandIsAlpha ? bandCount - 1 : bandCount;
        for (int i = 1; i <= lastDataBand; i++) {
            GDALRasterBandH hBand = GDALGetRasterBand(hDataset, i);
            BandInfo bi;
            bi.index = i;

            const char *desc = GDALGetDescription(hBand);
            std::string name = (desc != nullptr) ? std::string(desc) : std::string();
            if (name.empty()) {
                GDALColorInterp ci = GDALGetRasterColorInterpretation(hBand);
                const char *ciName = GDALGetColorInterpretationName(ci);
                if (ciName != nullptr) name = ciName;
            }
            bi.name = name;
            bi.domain = "optical";
            result.bands.push_back(bi);
        }

        GDALClose(hDataset);
        result.defaultBandMapping = getFallbackMapping(rasterPath, bandCount);
        return result;
    }

    GDALClose(hDataset);
    return result;
}

BandMapping SensorProfileManager::getDefaultBandMapping(const std::string &rasterPath) const {
    auto det = detectSensor(rasterPath);
    if (det.detected) {
        return det.defaultBandMapping;
    }
    return getFallbackMapping(rasterPath, 0);
}

BandMapping SensorProfileManager::getBandMappingForPreset(const std::string &rasterPath, const std::string &presetId) const {
    auto det = detectSensor(rasterPath);
    if (det.detected) {
        for (const auto &p : det.presets) {
            if (p.id == presetId && (p.type == "bands" || p.type.empty())) {
                return p.bandMapping;
            }
        }
    }
    return getDefaultBandMapping(rasterPath);
}

BandMapping SensorProfileManager::getFallbackMapping(const std::string &rasterPath, int bandCount) const {
    BandMapping bm;

    GDALDatasetH hDataset = GDALOpen(rasterPath.c_str(), GA_ReadOnly);
    if (!hDataset) return bm;

    int nBands = (bandCount > 0) ? bandCount : GDALGetRasterCount(hDataset);

    // Try to determine mapping from GDALGetRasterColorInterpretation
    int rBand = 0, gBand = 0, bBand = 0;
    for (int i = 1; i <= nBands; i++) {
        GDALRasterBandH hBand = GDALGetRasterBand(hDataset, i);
        GDALColorInterp ci = GDALGetRasterColorInterpretation(hBand);
        if (ci == GCI_RedBand && rBand == 0) rBand = i;
        else if (ci == GCI_GreenBand && gBand == 0) gBand = i;
        else if (ci == GCI_BlueBand && bBand == 0) bBand = i;
    }

    GDALClose(hDataset);

    if (rBand > 0 && gBand > 0 && bBand > 0) {
        bm.r = rBand;
        bm.g = gBand;
        bm.b = bBand;
    } else {
        // Ultimate fallback: {1, 2, 3} for 3+ bands, {1, 1, 1} for <3 bands
        if (nBands >= 3) {
            bm = {1, 2, 3};
        } else {
            bm = {1, 1, 1};
        }
    }

    return bm;
}

std::string SensorProfileManager::getRasterInfoJson(const std::string &rasterPath) const {
    auto det = detectSensor(rasterPath);

    GDALDatasetH hDataset = GDALOpen(rasterPath.c_str(), GA_ReadOnly);
    if (!hDataset) throw GDALException("Cannot open " + rasterPath);

    int bandCount = GDALGetRasterCount(hDataset);
    GDALRasterBandH hBand1 = GDALGetRasterBand(hDataset, 1);
    GDALDataType dt = GDALGetRasterDataType(hBand1);
    int width = GDALGetRasterXSize(hDataset);
    int height = GDALGetRasterYSize(hDataset);

    json result;
    result["bandCount"] = bandCount;
    result["dataType"] = gdalDataTypeName(dt);
    result["width"] = width;
    result["height"] = height;

    if (det.detected) {
        result["detectedSensor"] = det.sensorId;

        json bandsArr = json::array();
        for (const auto &bi : det.bands) {
            json bj;
            bj["index"] = bi.index;
            bj["name"] = bi.name;
            if (bi.wavelength > 0) bj["wavelength"] = bi.wavelength;

            // Get color interpretation from GDAL
            if (bi.index <= bandCount) {
                GDALRasterBandH hBand = GDALGetRasterBand(hDataset, bi.index);
                GDALColorInterp ci = GDALGetRasterColorInterpretation(hBand);
                bj["colorInterpretation"] = GDALGetColorInterpretationName(ci);
            }
            bandsArr.push_back(bj);
        }
        result["bands"] = bandsArr;

        json presetsArr = json::array();
        for (const auto &p : det.presets) {
            json pj;
            pj["id"] = p.id;
            pj["name"] = p.name;
            if (!p.description.empty()) pj["description"] = p.description;
            if (p.type == "bands" || p.type.empty()) {
                pj["bandMapping"] = p.bandMapping;
            }
            pj["isDefault"] = p.isDefault;
            if (p.type == "index") {
                pj["type"] = "index";
                if (!p.formula.empty()) pj["formula"] = p.formula;
                if (!p.colormap.empty()) pj["colormap"] = p.colormap;
            }
            presetsArr.push_back(pj);
        }
        result["availablePresets"] = presetsArr;
        result["defaultPreset"] = det.defaultPresetId;
    } else {
        // Build basic bands info from GDAL
        json bandsArr = json::array();
        for (int i = 1; i <= bandCount; i++) {
            GDALRasterBandH hBand = GDALGetRasterBand(hDataset, i);
            GDALColorInterp ci = GDALGetRasterColorInterpretation(hBand);
            json bj;
            bj["index"] = i;
            bj["name"] = GDALGetColorInterpretationName(ci);
            bj["colorInterpretation"] = GDALGetColorInterpretationName(ci);
            bandsArr.push_back(bj);
        }
        result["bands"] = bandsArr;
        result["availablePresets"] = json::array();
        result["defaultPreset"] = nullptr;
        result["detectedSensor"] = nullptr;
    }

    GDALClose(hDataset);

    return result.dump();
}

} // namespace ddb
