/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "sensorprofile.h"
#include "json.h"
#include "gdal_metadata.h"
#include "gdal_raii.h"
#include "raster_source.h"
#include "exceptions.h"
#include "logger.h"
#include "mio.h"
#include "fileversion.h"
#include "vegetation.h"
#include "stringutil.h"

#include <fstream>
#include <algorithm>
#include <sstream>
#include <regex>

namespace ddb {

// --- JSON serialization ---

void to_json(json &j, const BandMapping &bm) {
    j = json{{"r", bm.r}, {"g", bm.g}, {"b", bm.b}};
}

void from_json(const json &j, BandMapping &bm) {
    j.at("r").get_to(bm.r);
    j.at("g").get_to(bm.g);
    j.at("b").get_to(bm.b);
}

void from_json(const json &j, BandInfo &bi) {
    j.at("index").get_to(bi.index);
    j.at("name").get_to(bi.name);
    if (j.contains("wavelength")) j.at("wavelength").get_to(bi.wavelength);
    if (j.contains("resolution")) j.at("resolution").get_to(bi.resolution);
    if (j.contains("domain")) j.at("domain").get_to(bi.domain);
    if (j.contains("description")) j.at("description").get_to(bi.description);
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

void from_json(const json &j, DetectionCriteria &dc) {
    j.at("bandCount").get_to(dc.bandCount);
    if (j.contains("dataType")) j.at("dataType").get_to(dc.dataType);
    if (j.contains("metadataPatterns")) j.at("metadataPatterns").get_to(dc.metadataPatterns);
    if (j.contains("priority")) j.at("priority").get_to(dc.priority);
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
    path = io::toUtf8(dataFile);

    if (path.empty() || !fs::exists(io::fromUtf8(path))) {
        LOGD << "Sensor profiles file not found, using empty profiles";
        loaded_ = true;
        return;
    }

    std::ifstream ifs(io::fromUtf8(path));
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
    // Memoised per file version: a tile or thumbnail request does not reopen the raster
    // and rescan its metadata, and the lock is not held during the GDAL work
    const auto version = fileVersion(io::fromUtf8(rasterPath));
    std::vector<SensorProfile> profiles;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ensureLoaded();
        if (!version.empty()) {
            for (auto it = detections_.begin(); it != detections_.end(); ++it) {
                if (it->path != rasterPath) continue;
                if (it->version != version) {
                    detections_.erase(it);
                    break;
                }
                detections_.splice(detections_.begin(), detections_, it);
                return detections_.front().result;
            }
        }
        profiles = profiles_;
    }

    auto result = detectSensorUncached(rasterPath, profiles);

    std::lock_guard<std::mutex> lock(mutex_);
    uncachedDetections_++;
    if (!version.empty()) {
        detections_.remove_if([&](const CachedDetection &d) { return d.path == rasterPath; });
        detections_.push_front(CachedDetection{rasterPath, version, result});
        if (detections_.size() > MaxCachedDetections)
            detections_.pop_back();
    }
    return result;
}

uint64_t SensorProfileManager::uncachedDetections() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return uncachedDetections_;
}

char bandSymbolOf(const std::string &name) {
    std::string n = str::toLowerCopy(str::trimView(name));
    for (auto &c : n)
        if (c == '-' || c == '_') c = ' ';
    if (n == "re" || n.find("red edge") != std::string::npos || n.find("rededge") != std::string::npos) return 'e';
    if (n == "r" || n.starts_with("red")) return 'R';
    if (n == "g" || n.starts_with("green")) return 'G';
    if (n == "b" || n.starts_with("blue")) return 'B';
    if (n == "n" || n.starts_with("nir") || n.starts_with("near infrared") || n.starts_with("nearinfrared") ||
        n.starts_with("near ir"))
        return 'N';
    return 0;
}

namespace {

// Every metadata string of the raster the profiles are matched against: the TIFF description and
// software, all domains of the dataset, and the metadata of each band
std::vector<std::string> collectMatchingMetadata(const RasterSource &source, int bandCount) {
    gdalmeta::CollectOptions options;
    options.datasetItems = {"TIFFTAG_IMAGEDESCRIPTION", "TIFFTAG_SOFTWARE"};
    options.bandCount = bandCount;
    return gdalmeta::collectMetadata(source, options);
}

// The presets of a generic profile (its bands are placeholders until GDAL's band descriptions replace them) built from
// the bands the raster has (FR-RAS-18): a colour composite takes the bands its channels name, by their names, and is
// left out when one is missing (no True Color without a Blue band, none that maps NIR to red); an index preset is left
// out when a band its formula reads is missing
std::vector<RenderPreset> presetsOfBands(const std::vector<RenderPreset> &presets, const std::vector<BandInfo> &bands) {
    std::map<char, int> indexOf; // the first band of each symbol, 1-based
    for (const auto &bi : bands) {
        const char symbol = bandSymbolOf(bi.name);
        if (symbol != 0 && indexOf.find(symbol) == indexOf.end()) indexOf[symbol] = bi.index;
    }
    // The channels (red, green, blue) of the composites of the profiles
    static const std::map<std::string, std::string> composites = {{"true-color", "RGB"}, {"false-color-ir", "NRG"}};

    std::vector<RenderPreset> result;
    for (const auto &p : presets) {
        RenderPreset built = p;
        if (p.type == "index") {
            const VegetationFormula *formula = VegetationEngine::instance().getFormula(p.formula);
            if (formula != nullptr) {
                const auto symbols = VegetationEngine::requiredSymbols(*formula);
                const bool complete = std::all_of(symbols.begin(), symbols.end(),
                                                  [&](char c) { return indexOf.find(c) != indexOf.end(); });
                if (!complete) continue;
            }
        } else if (const auto composite = composites.find(p.id); composite != composites.end()) {
            const std::string &channels = composite->second;
            const bool complete = std::all_of(channels.begin(), channels.end(),
                                              [&](char c) { return indexOf.find(c) != indexOf.end(); });
            if (!complete) continue;
            built.bandMapping = BandMapping{indexOf[channels[0]], indexOf[channels[1]], indexOf[channels[2]]};
        }
        result.push_back(built);
    }
    return result;
}

// The detection result of a matched profile: its identity, bands and presets, and the default
// preset
void describeMatch(SensorDetectionResult &result, const SensorProfile &profile, const RasterSource &source,
                   int bandCount) {
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
    // When no description names a band a formula reads ("Band 1", ...), the descriptions do not tell the order of the
    // bands: the profile's bands and presets stand, as for a raster without descriptions (FR-RAS-18)
    if (profile.detection.metadataPatterns.empty()) {
        std::vector<BandInfo> described = profile.bands;
        bool named = false;
        for (auto &bi : described) {
            if (bi.index < 1 || bi.index > bandCount) continue;
            const std::string d = source.description(bi.index);
            if (!d.empty()) {
                bi.name = d;
                if (bandSymbolOf(bi.name) != 0) named = true;
            }
        }
        if (named) {
            result.bands = described;
            // Its presets map the bands the raster has, not those the profile assumed
            result.presets = presetsOfBands(profile.presets, result.bands);
        }
    }

    // Find default preset
    for (const auto &p : result.presets) {
        if (p.isDefault) {
            result.defaultBandMapping = p.bandMapping;
            result.defaultPresetId = p.id;
            break;
        }
    }
    // If no default marked, use the first bands-type preset
    if (result.defaultPresetId.empty() && !result.presets.empty()) {
        for (const auto &p : result.presets) {
            if (p.type == "bands" || p.type.empty()) {
                result.defaultBandMapping = p.bandMapping;
                result.defaultPresetId = p.id;
                break;
            }
        }
    }
}

// The bands of a multi-band non-Byte raster that no profile matched, named from the GDAL
// descriptions or color interpretations
void describeUnmatchedBands(SensorDetectionResult &result, const RasterSource &source, int bandCount,
                            bool lastBandIsAlpha) {
    result.detected = true;
    result.sensorCategory = "multispectral";

    int lastDataBand = lastBandIsAlpha ? bandCount - 1 : bandCount;
    for (int i = 1; i <= lastDataBand; i++) {
        BandInfo bi;
        bi.index = i;

        std::string name = source.description(i);
        if (name.empty()) name = source.colorInterp(i);
        bi.name = name;
        bi.domain = "optical";
        result.bands.push_back(bi);
    }
}

}  // namespace

SensorDetectionResult SensorProfileManager::detectSensorUncached(const std::string &rasterPath,
                                                                 const std::vector<SensorProfile> &profiles) const {
    SensorDetectionResult result;

    auto source = openInputSource(rasterPath);
    if (!source) return result;

    int bandCount = source->bandCount();
    if (bandCount == 0) {
        return result;
    }

    // Get data type from first band
    GDALDataType dt = source->dataType(1);
    std::string dataType = gdalDataTypeName(dt);

    // Check if last band is alpha
    bool lastBandIsAlpha = bandCount >= 2 && source->isAlpha(bandCount);

    // RGBA disambiguation: 4-band Byte with alpha → not multispectral
    if (bandCount == 4 && dt == GDT_Byte && lastBandIsAlpha) {
        result.detected = false;
        return result;
    }

    // 3-band Byte with no wavelength metadata → standard RGB
    if (bandCount == 3 && dt == GDT_Byte) {
        result.detected = false;
        return result;
    }

    // Collect metadata for matching
    const std::vector<std::string> allMetadata = collectMatchingMetadata(*source, bandCount);

    // Try matching profiles (already sorted by priority descending)
    for (const auto &profile : profiles) {
        if (profile.sensorCategory != "multispectral" && profile.sensorCategory != "thermal") continue;

        if (matchesProfile(profile, bandCount, dataType, allMetadata, lastBandIsAlpha)) {
            describeMatch(result, profile, *source, bandCount);
            // No band preset is left (the raster lacks the bands of each): the mapping of the colour interpretations
            if (result.defaultPresetId.empty()) {
                source.reset();
                result.defaultBandMapping = getFallbackMapping(rasterPath, bandCount);
            }
            return result;
        }
    }

    // No profile matched - for multi-band non-Byte images, return a basic result
    // with fallback mapping (detection fallback).
    // Populate result.bands from GDAL descriptions / color interpretation so that
    // downstream consumers (getRasterInfoJson, autoDetectFilter) can work with
    // the detected sensor even without a matching profile.
    if (bandCount > 3 && dt != GDT_Byte) {
        describeUnmatchedBands(result, *source, bandCount, lastBandIsAlpha);

        source.reset();
        result.defaultBandMapping = getFallbackMapping(rasterPath, bandCount);
        return result;
    }

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

    auto source = openInputSource(rasterPath);
    if (!source) return bm;

    int nBands = (bandCount > 0) ? bandCount : source->bandCount();

    // Try to determine mapping from the colour interpretations
    int rBand = 0, gBand = 0, bBand = 0;
    for (int i = 1; i <= nBands; i++) {
        const std::string ci = source->colorInterp(i);
        if (ci == "Red" && rBand == 0) rBand = i;
        else if (ci == "Green" && gBand == 0) gBand = i;
        else if (ci == "Blue" && bBand == 0) bBand = i;
    }

    source.reset();

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

    const auto source = openInputSource(rasterPath);
    if (!source) throw GDALException("Cannot open " + rasterPath);

    int bandCount = source->bandCount();
    GDALDataType dt = source->dataType(1);
    int width = source->width();
    int height = source->height();

    json result;
    result["bandCount"] = bandCount;
    result["dataType"] = gdalDataTypeName(dt);
    result["width"] = width;
    result["height"] = height;

    // Expose the native CRS as a best-effort authority code (e.g. "EPSG:32634").
    // Consumed by OGC WCS DescribeCoverage and by clients wanting OUTPUTCRS=native
    // round-trips. Falls back to the raw WKT when the projection has no authority
    // (rare for orthos), or to an empty string when the raster is non-georeferenced.
    {
        const std::string wkt = source->wkt();
        std::string crs;
        if (!wkt.empty()) {
            const SrsPtr hSrs(OSRNewSpatialReference(wkt.c_str()));
            if (hSrs) {
                const char *authName = OSRGetAuthorityName(hSrs.get(), nullptr);
                const char *authCode = OSRGetAuthorityCode(hSrs.get(), nullptr);
                if (authName && *authName && authCode && *authCode) {
                    crs = std::string(authName) + ":" + authCode;
                } else {
                    crs = wkt;
                }
            }
        }
        result["crs"] = crs;
    }

    if (det.detected) {
        result["detectedSensor"] = det.sensorId;
        // The profile's name next to its id (new in 2.4); absent for bands no profile matched
        if (!det.sensorName.empty()) result["detectedSensorName"] = det.sensorName;

        json bandsArr = json::array();
        for (const auto &bi : det.bands) {
            json bj;
            bj["index"] = bi.index;
            bj["name"] = bi.name;
            if (bi.wavelength > 0) bj["wavelength"] = bi.wavelength;

            // The colour interpretation of the band
            if (bi.index <= bandCount) {
                bj["colorInterpretation"] = source->colorInterp(bi.index);
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
        // The bands named by their colour interpretations
        json bandsArr = json::array();
        for (int i = 1; i <= bandCount; i++) {
            const std::string ci = source->colorInterp(i);
            json bj;
            bj["index"] = i;
            bj["name"] = ci;
            bj["colorInterpretation"] = ci;
            bandsArr.push_back(bj);
        }
        result["bands"] = bandsArr;
        result["availablePresets"] = json::array();
        result["defaultPreset"] = nullptr;
        result["detectedSensor"] = nullptr;
    }

    return result.dump();
}

} // namespace ddb
