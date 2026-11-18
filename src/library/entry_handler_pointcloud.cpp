/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// PointCloud entries: LAS/LAZ/E57/PTS/XYZ (and point-only PLY), built into a COPC file.

#include "build_layout.h"
#include "json.h"
#include "entry.h"
#include "entry_handlers_builtin.h"
#include "mio.h"
#include "pointcloud.h"
#include "thumbs.h"
#include "tilers_tiler.h"

namespace ddb {

namespace {

// The COPC artifact of a point cloud: its thumbnail and its tiles (rendered by ddb-tilers, local or at
// an http(s) URL)
class CopcThumbnailGenerator : public ThumbnailGenerator {
  public:
    bool accepts(const fs::path& input) const override { return isCopcPath(io::toUtf8(input)); }
    void generate(const fs::path& input, int thumbSize, const fs::path& outImagePath, uint8_t** outBuffer,
                  int* outBufferSize) const override {
        generatePointCloudThumb(input, thumbSize, outImagePath, outBuffer, outBufferSize);
    }
};

class CopcTilerFactory : public TilerFactory {
  public:
    bool accepts(const fs::path& tileablePath) const override { return isCopcPath(io::toUtf8(tileablePath)); }
    std::unique_ptr<Tiler> create(const fs::path& input, const fs::path& outputFolder, int tileSize,
                                  bool tms) const override {
        return std::make_unique<TilersTiler>(io::toUtf8(input), io::toUtf8(outputFolder), tileSize, tms);
    }
};

constexpr const char* CopcRole = "pointcloud.copc";

void parsePointCloudMetadata(const fs::path &path, Entry &entry)
{
    PointCloudInfo info;
    if (getPointCloudInfo(io::toUtf8(path), info))
    {
        entry.properties = info;
        entry.polygon_geom = info.polyBounds;
        entry.point_geom = info.centroid;

        // The properties above replace any warning: it is recorded last. A CRS the coordinates do
        // not fit is the reason itself ("coordinate system ignored: ...").
        if (!info.footprintWarning.empty())
            setEntryWarning(entry, info.crsStatus == PointCloudCrsStatus::Implausible
                                       ? info.footprintWarning
                                       : "footprint unavailable: " + info.footprintWarning);
    }
}

// The status of the CRS of the cloud of an entry, as its index records it. An entry indexed before
// the status was recorded has only the CRS the file states: its kind, without the check against the
// coordinates (the index has no bounds), stands for it.
std::string projectionOf(const Entry& e) {
    if (!e.properties.is_object())
        return {};
    const auto projection = e.properties.find("projection");
    return projection != e.properties.end() && projection->is_string() ? projection->get<std::string>() : std::string();
}

PointCloudCrsStatus crsStatusOf(const Entry& e) {
    if (const auto status = indexedCrsStatus(e.properties))
        return *status;
    return PointCloudCrsPolicy::classify(projectionOf(e), {}).status;
}

// What the index holds of the CRS of the cloud of an entry, as the build needs it: its status, its
// CRS and the target of its COPC, chosen from its footprint as when the helper reads the cloud.
// Nothing for an entry indexed before the status was recorded, or one whose COPC is reprojected
// and that is read without its footprint: the helper reads the cloud then.
std::optional<PointCloudInfo> indexedCrsOf(const Entry& e) {
    const auto status = indexedCrsStatus(e.properties);
    if (!status)
        return std::nullopt;
    PointCloudInfo info;
    info.crsStatus = *status;
    info.wktProjection = projectionOf(e);
    if (PointCloudCrsPolicy::reprojects(*status)) {
        if (e.polygon_geom.empty())
            return std::nullopt;
        info.polyBounds = e.polygon_geom;
    }
    chooseCopcTarget(info, e.path);
    return info;
}

class PointCloudEntryHandler : public EntryHandler {
  public:
    PointCloudEntryHandler() : EntryHandler(EntryType::PointCloud, "PointCloud") {}

    std::vector<std::string> extensions() const override { return {"laz", "las", "e57", "pts", "xyz"}; }

    void parseMetadata(const fs::path& path, Entry& entry, FingerprintContext& /*ctx*/) const override {
        parsePointCloudMetadata(path, entry);
    }

    std::string subfolder() const override { return CopcBuildSubfolder; }

    // isComplete() and describeArtifacts() read the status and the CRS of the cloud
    std::vector<std::string> buildPropertyKeys() const override { return {"crsStatus", "projection"}; }

    // A leaf inside a built EPT dataset is not built: the guard skips residual ept-data
    // folders from older builds.
    bool isBuildable(const Entry& e) const override {
        return io::toUtf8(io::fromUtf8(e.path).parent_path().filename()) != LegacyEptDataFolder;
    }

    std::vector<BuildArtifact> artifacts() const override {
        return {BuildArtifact::file(CopcRole, artifactPath(CopcBuildSubfolder, CopcFileName),
                                    "application/vnd.laszip+copc", CopcFileName),
                BuildArtifact::directory("pointcloud.ept", LegacyEptBuildSubfolder, "application/json",
                                         LegacyEptFileName)
                    .legacyFor(CopcRole)};
    }

    // The COPC folder has its output (the default rule). The COPC of a geographic or geocentric
    // cloud is built in a projected CRS, the one of a cloud whose CRS does not fit its coordinates
    // without a CRS (PointCloudCrsPolicy::changesCrs): one built under an older policy, or before the
    // policy (no build record), is incomplete, so that it is built again; the one it has is served
    // meanwhile. The status comes from the index, so the check reads no point cloud.
    bool isComplete(const Entry& e, const fs::path& baseOutputPath, bool reportRecoverable) const override {
        if (!EntryHandler::isComplete(e, baseOutputPath, reportRecoverable))
            return false;
        const fs::path folder = baseOutputPath / CopcBuildSubfolder;
        if (!PointCloudCrsPolicy::changesCrs(crsStatusOf(e)))
            return true;
        const auto record = readCopcBuildRecord(folder / CopcBuildRecordFileName);
        return record && record->crsPolicy >= PointCloudCrsPolicy::kVersion;
    }

    // The CRS of the COPC: the one its build recorded, else the CRS of the cloud (a build without a
    // record never transformed the points). Nothing for a COPC without a CRS.
    void describeArtifacts(const Entry& e, const fs::path& baseOutputPath,
                           std::vector<BuildArtifact>& artifacts) const override {
        for (BuildArtifact& a : artifacts) {
            if (a.role != CopcRole)
                continue;
            const auto record = readCopcBuildRecord(baseOutputPath / CopcBuildSubfolder / CopcBuildRecordFileName);
            const auto name = crsNameOf(record ? record->copcCrs : projectionOf(e));
            if (name)
                a.crs = ArtifactCrs{name->name, name->epsg, record && record->transformed};
        }
    }

    void build(const BuildContext& ctx) const override {
        const std::vector vec = {io::toUtf8(ctx.inputPath)};
        // Defensive cleanup: remove any legacy EPT artifacts from previous builds, in the build
        // folder and in the folder of this build (buildCopc touches nothing but its output)
        io::assureIsRemoved(ctx.baseOutputPath / LegacyEptBuildSubfolder);
        io::assureIsRemoved(ctx.tempFolder / LegacyEptFileName);
        io::assureIsRemoved(ctx.tempFolder / LegacyEptDataFolder);
        io::assureIsRemoved(ctx.tempFolder / LegacyEptHierarchyFolder);
        const CopcBuildRecord record = buildCopc(vec, io::toUtf8(ctx.tempFolder), ctx.options, true, {indexedCrsOf(ctx.entry)});
        writeCopcBuildRecord(ctx.tempFolder / CopcBuildRecordFileName, record);
    }

    ThumbnailSource thumbnailSource() const override { return ThumbnailSource::BuildArtifact; }

    const ThumbnailGenerator* thumbnailGenerator() const override { return &thumbnails_; }

    const TilerFactory* tilerFactory() const override { return &tiler_; }

  private:
    CopcThumbnailGenerator thumbnails_;
    CopcTilerFactory tiler_;
};

}  // namespace

std::unique_ptr<EntryHandler> makePointCloudEntryHandler() {
    return std::make_unique<PointCloudEntryHandler>();
}

}  // namespace ddb
