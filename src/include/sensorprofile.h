/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef SENSORPROFILE_H
#define SENSORPROFILE_H

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include "json.h"
#include "ddb_export.h"

namespace ddb {

struct BandMapping {
    int r = 1;
    int g = 2;
    int b = 3;

    static BandMapping Parse(const std::string &s);
    std::string toInternalString() const;
    std::string toApiString() const;

    bool operator==(const BandMapping &o) const {
        return r == o.r && g == o.g && b == o.b;
    }
};

struct BandInfo {
    int index = 0;
    std::string name;
    int wavelength = 0;
    int resolution = 0;
    std::string domain = "optical";
    std::string description;
};

struct RenderPreset {
    std::string id;
    std::string name;
    std::string description;
    BandMapping bandMapping;
    bool isDefault = false;
    std::string type = "bands";     // "bands" or "index"
    std::string stretch;
    std::string formula;
    std::string colormap;
};

struct DetectionCriteria {
    int bandCount = 0;
    std::string dataType;
    std::vector<std::string> metadataPatterns;
    int priority = 0;
};

struct SensorProfile {
    std::string id;
    std::string name;
    std::string description;
    std::string sensorCategory;
    DetectionCriteria detection;
    std::vector<BandInfo> bands;
    std::vector<RenderPreset> presets;
};

struct SensorDetectionResult {
    bool detected = false;
    std::string sensorId;
    std::string sensorName;
    std::string sensorCategory;
    BandMapping defaultBandMapping;
    std::vector<BandInfo> bands;
    std::vector<RenderPreset> presets;
    std::string defaultPresetId;
};

class DDB_DLL SensorProfileManager {
public:
    static SensorProfileManager& instance();

    void loadDefaults(const std::string &jsonPath = "");
    void loadOverrides(const std::string &jsonStr);

    SensorDetectionResult detectSensor(const std::string &rasterPath) const;
    BandMapping getDefaultBandMapping(const std::string &rasterPath) const;
    BandMapping getBandMappingForPreset(const std::string &rasterPath, const std::string &presetId) const;

    std::string getRasterInfoJson(const std::string &rasterPath) const;

    const std::vector<SensorProfile>& getProfiles() const { return profiles_; }

private:
    SensorProfileManager() = default;
    SensorProfileManager(const SensorProfileManager&) = delete;
    SensorProfileManager& operator=(const SensorProfileManager&) = delete;

    bool matchesProfile(const SensorProfile &profile, int bandCount,
                        const std::string &dataType,
                        const std::vector<std::string> &metadata,
                        bool lastBandIsAlpha) const;

    BandMapping getFallbackMapping(const std::string &rasterPath, int bandCount) const;

    mutable std::mutex mutex_;
    std::vector<SensorProfile> profiles_;
    bool loaded_ = false;
};

// JSON serialization
void to_json(json &j, const BandMapping &bm);
void from_json(const json &j, BandMapping &bm);
void to_json(json &j, const BandInfo &bi);
void from_json(const json &j, BandInfo &bi);
void to_json(json &j, const RenderPreset &rp);
void from_json(const json &j, RenderPreset &rp);
void to_json(json &j, const DetectionCriteria &dc);
void from_json(const json &j, DetectionCriteria &dc);
void to_json(json &j, const SensorProfile &sp);
void from_json(const json &j, SensorProfile &sp);

} // namespace ddb

#endif // SENSORPROFILE_H
