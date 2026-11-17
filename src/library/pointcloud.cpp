/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "pointcloud.h"
#include "json.h"
#include "stringutil.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <cctype>


#include "coordstransformer.h"
#include "entry.h"
#include "exceptions.h"
#include "gdal_open_policy.h"
#include "gdal_raii.h"
#include "geo.h"
#include "logger.h"
#include "build_layout.h"
#include "cancellation.h"
#include "mio.h"
#include "parallel.h"
#include "ply.h"
#include "raster_artefacts.h"
#include "raster_helper.h"
#include "raster_ops.h"
#include "tilers_call.h"

namespace ddb {

namespace {

// The envelope and the centre of a point cloud's bounds in another CRS, with the height of the
// footprint (metres)
struct Footprint {
    double minX = 0, minY = 0, maxX = 0, maxY = 0;
    double centreX = 0, centreY = 0;
    double minZ = 0, maxZ = 0;
};

std::string boundsText(const std::vector<double>& b) {
    std::ostringstream text;
    text << "[" << b[0] << ", " << b[1] << ", " << b[2] << ", " << b[3] << ", " << b[4] << ", " << b[5] << "]";
    return text.str();
}

std::unique_ptr<CoordsTransformer> transformTo(const OGRSpatialReference& from, const OGRSpatialReference& to,
                                               int toEpsg) {
    auto transform = CoordsTransformer::tryCreate(from, to);
    if (!transform) {
        const char* name = from.GetName();
        throw GDALException("No coordinate transformation from " +
                            std::string(name != nullptr ? name : "the CRS of the file") + " to EPSG:" +
                            std::to_string(toEpsg));
    }
    return transform;
}

// Only the horizontal part of the CRS of the file takes part: x and y are transformed, z is not (a
// height in the vertical unit of the CRS, converted to metres), so a vertical datum that PROJ cannot
// use here (a geoid grid that is not installed) does not matter
Footprint horizontalFootprint(const std::vector<double>& b, const OGRSpatialReference& fileSrs,
                              const OGRSpatialReference& target, int targetEpsg) {
    const auto transform = transformTo(horizontalSrs(fileSrs), target, targetEpsg);
    Footprint f{b[0], b[1], b[3], b[4], (b[0] + b[3]) / 2.0, (b[1] + b[4]) / 2.0, 0, 0};
    if (!transform->tryTransform(1, &f.minX, &f.minY) || !transform->tryTransform(1, &f.maxX, &f.maxY) ||
        !transform->tryTransform(1, &f.centreX, &f.centreY))
        throw GDALException("Cannot transform the bounds " + boundsText(b) + " to EPSG:" + std::to_string(targetEpsg));
    const double zToMeters = verticalUnitInMeters(fileSrs);
    f.minZ = b[2] * zToMeters;
    f.maxZ = b[5] * zToMeters;
    return f;
}

// What ddbtReadPoints hands over of a geocentric cloud, transformed to longitude, latitude and
// ellipsoidal height, into the envelope of its points
struct GeocentricEnvelope {
    CoordsTransformer* toLonLat = nullptr;
    Footprint f{1e300, 1e300, -1e300, -1e300, 0, 0, 1e300, -1e300};
    std::vector<double> x, y, z;
    std::vector<int> ok;

    static int batch(uint64_t count, const double* xyz, const uint16_t* /*rgb*/, void* user) {
        auto& self = *static_cast<GeocentricEnvelope*>(user);
        if (cancel::isCanceled())
            return 1;
        self.x.resize(count);
        self.y.resize(count);
        self.z.resize(count);
        self.ok.resize(count);
        for (uint64_t i = 0; i < count; ++i) {
            self.x[i] = xyz[3 * i];
            self.y[i] = xyz[3 * i + 1];
            self.z[i] = xyz[3 * i + 2];
        }
        // A point PROJ cannot transform is left out
        self.toLonLat->get()->Transform(static_cast<size_t>(count), self.x.data(), self.y.data(), self.z.data(),
                                        nullptr, self.ok.data());
        for (uint64_t i = 0; i < count; ++i) {
            if (!self.ok[i] || !std::isfinite(self.x[i]) || !std::isfinite(self.y[i]))
                continue;
            self.f.minX = std::min(self.f.minX, self.x[i]);
            self.f.maxX = std::max(self.f.maxX, self.x[i]);
            self.f.minY = std::min(self.f.minY, self.y[i]);
            self.f.maxY = std::max(self.f.maxY, self.y[i]);
        }
        return 0;
    }
};

// The footprint of a geocentric cloud. An x, y, z is a point only as a whole, and the box of a patch
// of the curved earth says little of where the patch is: its corners lie above and below the ground,
// their longitudes and latitudes tens of metres outside the cloud for every kilometre of it, and
// whether a box is a tilted plate or a block cannot be told from it. The points are read (as the
// bounds of a cloud whose header has none are) and transformed in 3-D to longitude, latitude and
// ellipsoidal height (EPSG:4979): the footprint is their envelope, and its height the one of the
// centre of the bounds.
Footprint geocentricFootprint(const std::string& utf8Path, const std::vector<double>& b,
                              const OGRSpatialReference& fileSrs, const OGRSpatialReference& target, int targetEpsg) {
    constexpr int Geographic3D = 4979;
    const SrsPtr lonLatHeight = srsFromEpsg(Geographic3D);
    if (!lonLatHeight)
        throw GDALException("Cannot read EPSG:" + std::to_string(Geographic3D) + ". Is PROJ available?");
    const auto toLonLat = transformTo(fileSrs, srsOf(lonLatHeight), Geographic3D);
    const auto cannotTransform = [&] {
        return GDALException("Cannot transform the bounds " + boundsText(b) + " to EPSG:" +
                             std::to_string(Geographic3D));
    };

    double cx = (b[0] + b[3]) / 2.0, cy = (b[1] + b[4]) / 2.0, cz = (b[2] + b[5]) / 2.0;
    if (!toLonLat->tryTransform(1, &cx, &cy, &cz))
        throw cannotTransform();

    GeocentricEnvelope reading;
    reading.toLonLat = toLonLat.get();
    const ddbtErr rc = ddbtReadPoints(utf8Path.c_str(), &GeocentricEnvelope::batch, &reading);
    cancel::throwIfCanceled();
    tilers::check(rc);
    Footprint f = reading.f;
    if (f.minX > f.maxX)
        throw cannotTransform();
    f.centreX = (f.minX + f.maxX) / 2.0;
    f.centreY = (f.minY + f.maxY) / 2.0;
    f.minZ = f.maxZ = cz;
    if (targetEpsg == 4326)
        return f;

    const SrsPtr wgs84 = srsFromEpsg(4326);
    if (!wgs84)
        throw GDALException("Cannot read EPSG:4326. Is PROJ available?");
    const auto toTarget = transformTo(srsOf(wgs84), target, targetEpsg);
    if (!toTarget->tryTransform(1, &f.minX, &f.minY) || !toTarget->tryTransform(1, &f.maxX, &f.maxY) ||
        !toTarget->tryTransform(1, &f.centreX, &f.centreY))
        throw GDALException("Cannot transform the bounds " + boundsText(b) + " to EPSG:" + std::to_string(targetEpsg));
    return f;
}

// Fills the polygon and the centroid of the point cloud (in EPSG:footprintSrs) from its bounds.
// Throws GDALException when the CRS cannot be transformed, or when a footprint in WGS 84 is not
// a longitude/latitude envelope (isWgs84Envelope): a wrong polygon is worse than none.
void populatePointCloudFootprint(PointCloudInfo& info, int footprintSrs, const std::string& utf8Path) {
    const std::vector<double>& b = info.bounds;  // minX, minY, minZ, maxX, maxY, maxZ

    const SrsPtr fileSrs = srsFromWkt(info.wktProjection.c_str());
    if (!fileSrs)
        throw GDALException("Cannot read the coordinate reference system of the file. Is PROJ available?");

    const SrsPtr tgt = srsFromEpsg(footprintSrs);
    if (!tgt)
        throw GDALException("Cannot read EPSG:" + std::to_string(footprintSrs) + ". Is PROJ available?");

    const OGRSpatialReference& fullSrs = srsOf(fileSrs);
    const Footprint f = fullSrs.IsGeocentric() ? geocentricFootprint(utf8Path, b, fullSrs, srsOf(tgt), footprintSrs)
                                               : horizontalFootprint(b, fullSrs, srsOf(tgt), footprintSrs);

    if (f.minZ < -30000 || f.maxZ > 30000) {
        LOGD << "Strange point cloud bounds [[" << f.minX << ", " << f.maxX << "], [" << f.minY << ", "
             << f.maxY << "], [" << f.minZ << ", " << f.maxZ << "]]";
        info.bounds.clear();
        return;
    }
    if (footprintSrs == 4326 && !isWgs84Envelope(f.minX, f.minY, f.maxX, f.maxY)) {
        std::ostringstream envelope;
        envelope << "[" << f.minX << ", " << f.minY << ", " << f.maxX << ", " << f.maxY << "]";
        throw GDALException("The bounds " + boundsText(b) + " give the envelope " + envelope.str() +
                            ", which is not a longitude/latitude one");
    }

    info.polyBounds.addPoint(f.minX, f.minY, f.minZ);
    info.polyBounds.addPoint(f.maxX, f.minY, f.minZ);
    info.polyBounds.addPoint(f.maxX, f.maxY, f.minZ);
    info.polyBounds.addPoint(f.minX, f.maxY, f.minZ);
    info.polyBounds.addPoint(f.minX, f.minY, f.minZ);
    info.centroid.addPoint(f.centreX, f.centreY, f.minZ);
}

// The status of the CRS of the cloud and its footprint. A footprint that cannot be computed does
// not make the file unreadable: it is indexed and built without one (the native bounds stay), and
// the entry says why it has none. A CRS the coordinates do not fit is handled as no CRS: no
// footprint, and its warning.
void addFootprint(PointCloudInfo& info, int footprintSrs, const std::string& utf8Path) {
    info.centroid.clear();
    info.polyBounds.clear();
    info.footprintWarning.clear();
    const PointCloudCrsCheck check = PointCloudCrsPolicy::classify(info.wktProjection, info.bounds);
    info.crsStatus = check.status;
    if (check.status == PointCloudCrsStatus::Implausible) {
        info.footprintWarning = check.warning;
        return;
    }
    if (info.bounds.size() != 6 || info.wktProjection.empty())
        return;
    try {
        populatePointCloudFootprint(info, footprintSrs, utf8Path);
    } catch (const GDALException& e) {
        LOGD << "No footprint for the point cloud: " << e.what();
        info.polyBounds.clear();
        info.centroid.clear();
        info.footprintWarning = e.what();
    }
}

// The summary of ddb-tilers (ddbtPointCloudInfo) of the point cloud at `utf8Path`
json tilersSummary(const std::string& utf8Path, bool readBounds) {
    char* raw = nullptr;
    tilers::check(ddbtPointCloudInfo(utf8Path.c_str(), readBounds, &raw));
    const tilers::String owned(raw);
    return json::parse(raw);
}

// The info of the point cloud at `utf8Path` as ddb-tilers reads it, with the footprint of its
// bounds in EPSG:footprintSrs when `withBounds` (the bounds are read when the cloud has a CRS and its
// header has none)
PointCloudInfo tilersPointCloudInfo(const std::string& utf8Path, bool withBounds, int footprintSrs) {
    json summary = tilersSummary(utf8Path, false);
    if (withBounds && !summary.contains("bounds") && !summary.value("wkt", std::string()).empty())
        summary = tilersSummary(utf8Path, true);

    PointCloudInfo info;
    info.pointCount = summary.at("pointCount").get<size_t>();
    info.wktProjection = summary.value("wkt", std::string());
    info.dimensions = summary.at("dimensions").get<std::vector<std::string>>();
    if (withBounds && summary.contains("bounds"))
        info.bounds = summary.at("bounds").get<std::vector<double>>();
    addFootprint(info, footprintSrs, utf8Path);
    return info;
}

// The extensions of the point clouds ddb-tilers reads for an entry (a PLY point cloud is read
// from its header), as the point cloud entries have them
bool isTilersPointCloud(const io::Path& path) {
    return path.checkExtension({"las", "laz", "e57", "pts", "xyz"});
}

}  // namespace

namespace {

LonLatEnvelope envelopeOf(const BasicPolygonGeometry& polygon) {
    LonLatEnvelope e{polygon.points.front().x, polygon.points.front().y, polygon.points.front().x,
                     polygon.points.front().y};
    for (const auto& p : polygon.points) {
        e.minLon = std::min(e.minLon, p.x);
        e.maxLon = std::max(e.maxLon, p.x);
        e.minLat = std::min(e.minLat, p.y);
        e.maxLat = std::max(e.maxLat, p.y);
    }
    return e;
}

}  // namespace

PointCloudInfo readPointCloudInfo(const fs::path& path) {
    gdalopen::requireUserFileOpens("readPointCloudInfo");
    // The bounds of a text file (XYZ, PTS) were never kept: only a header gives them
    const bool text = io::Path(path).checkExtension({"pts", "xyz"});
    PointCloudInfo info = tilersPointCloudInfo(io::toUtf8(path), !text, 4326);
    if (PointCloudCrsPolicy::reprojects(info.crsStatus) && !info.polyBounds.empty()) {
        try {
            info.targetCrs =
                PointCloudCrsPolicy::target(info.wktProjection, info.crsStatus, envelopeOf(info.polyBounds));
        } catch (const GDALException& e) {
            // The COPC keeps the CRS of the cloud, as before the policy
            LOGW << "No projected coordinate system for the COPC of " << io::toUtf8(path) << ": " << e.what();
        }
    }
    return info;
}

bool getPointCloudInfo(const std::string& filename, PointCloudInfo& info) {
    const io::Path path(io::fromUtf8(filename));

    if (path.checkExtension({"ply"})) {
        PlyInfo plyInfo;
        if (!getPlyInfo(io::fromUtf8(filename), plyInfo))
            return false;
        info = PointCloudInfo();
        info.pointCount = static_cast<size_t>(plyInfo.vertexCount);
        info.dimensions = plyInfo.dimensions;
        return true;
    }

    if (!isTilersPointCloud(path)) {
        LOGD << "Not a point cloud ddb-tilers reads: " << filename;
        return false;
    }
    readPointCloud("", [&] { info = raster::rasterHelper().pointCloudInfo(io::fromUtf8(filename)); });
    return true;
}

void readPointCloud(const std::string& context, const std::function<void()>& read) {
    try {
        read();
    } catch (const raster::RasterOpException& e) {
        throw PDALException(context + e.what());
    }
}

bool getCopcInfo(const std::string& copcPath, PointCloudInfo& info, int footprintSrs) {
    try {
        info = tilersPointCloudInfo(copcPath, true, footprintSrs);
        return true;
    } catch (const std::exception& e) {
        LOGD << "Cannot read the COPC " << copcPath << ": " << e.what();
        return false;
    }
}

bool isCopcPath(const std::string& filename) {
    return str::endsWithCI(filename, ".copc.laz");
}

const char* copcPhaseOfFraction(double fraction) {
    if (fraction < 0.05) return "planning";
    if (fraction < 0.4) return "reading";
    if (fraction < 1.0) return "building";
    return "writing";
}

namespace raster {

namespace {

// The members of the builder's report that the host needs (CopcResult)
CopcResult copcResultOfReport(const fs::path& reportFile) {
    std::ifstream in(reportFile, std::ios::binary);
    if (!in)
        throw RasterOpException(ErrorCode::Internal, "The COPC build wrote no report");
    const json report = json::parse(in, nullptr, false);
    if (!report.is_object())
        throw RasterOpException(ErrorCode::Internal, "The report of the COPC build is not a JSON object");
    CopcResult result;
    result.transformed = report.value("transformed", false);
    if (const auto it = report.find("maxNodePoints"); it != report.end() && it->is_number_unsigned())
        result.maxNodePoints = it->get<uint64_t>();
    if (const auto it = report.find("warnings"); it != report.end() && it->is_array())
        for (const auto& w : *it)
            if (w.is_string())
                result.warnings.push_back(w.get<std::string>());
    return result;
}

}  // namespace

CopcResult buildCopcFile(const fs::path& path, const fs::path& outFile, const CopcParams& params,
                         const ProgressCallback& progress) {
    // The builder runs a pool of its own of the threads it is given, the calling one included
    const auto cpu = acquireBuildThreads(params.threads);
    // The report goes beside the COPC, in the folder the build writes in
    fs::path reportFile = outFile;
    reportFile += ".report.json";
    json options = {{"threads", cpu.threads()}, {"metadata", false}, {"report", io::toUtf8(reportFile)}};
    if (!params.inputs.empty()) {
        json inputs = json::array();
        for (const fs::path& input : params.inputs)
            inputs.push_back(io::toUtf8(input));
        options["inputs"] = std::move(inputs);
    }
    if (!params.targetCrs.empty())
        options["targetCrs"] = params.targetCrs;
    if (params.dropCrs)
        options["dropCrs"] = true;

    LOGD << "Building COPC from " << io::toUtf8(path) << " and " << params.inputs.size()
         << " more file(s) with ddb-tilers (" << cpu.threads() << " threads"
         << (params.targetCrs.empty() ? std::string() : ", in " + params.targetCrs)
         << (params.dropCrs ? ", without its coordinate system" : "") << ")";
    tilers::BuildProgress forward("COPC", progress);
    const ddbtErr rc = ddbtBuildCopc2(io::toUtf8(path).c_str(), io::toUtf8(outFile).c_str(), options.dump().c_str(),
                                      &tilers::BuildProgress::forward, &forward);
    // An input the builder cannot read (a file that is no point cloud it knows, one without
    // points, a coordinate it cannot store or transform) is a file the helper cannot read
    tilers::checkBuild(rc, "The COPC build",
                       [](const std::string& message) { throw RasterOpException(ErrorCode::Unreadable, message); });
    CopcResult result = copcResultOfReport(reportFile);
    std::error_code ec;
    fs::remove(reportFile, ec);
    return result;
}

}  // namespace raster

namespace {

// Every source of a COPC build exists and is a point cloud
void checkSources(const std::vector<std::string>& filenames) {
    if (filenames.empty())
        throw InvalidArgsException("No point cloud to build a COPC of");
    for (const std::string& f : filenames) {
        if (!fs::exists(io::fromUtf8(f)))
            throw FSException(f + " does not exist");
        if (fingerprint(io::fromUtf8(f)) != EntryType::PointCloud)
            throw InvalidArgsException(f + " is not a supported point cloud file");
    }
}

// What the builder reports for an octree with a node far larger than its leaves
constexpr const char* DegenerateOctreeWarning = "degenerate octree";

// The CRS of the COPC of clouds merged into one: the target the policy chose for the first of them
// that has one (the builder transforms every input from its own CRS to it); else none when a cloud
// states a CRS its coordinates do not fit (handled as no CRS, by every reader of the COPC too);
// else the CRS of the clouds
void planCopcCrs(const std::vector<PointCloudInfo>& inputs, raster::CopcParams& params) {
    params.targetCrs.clear();
    params.dropCrs = false;
    for (const PointCloudInfo& info : inputs)
        if (!info.targetCrs.empty()) {
            params.targetCrs = info.targetCrs;
            return;
        }
    params.dropCrs = std::any_of(inputs.begin(), inputs.end(), [](const PointCloudInfo& info) {
        return info.crsStatus == PointCloudCrsStatus::Implausible;
    });
}

// The COPC the helper wrote is published as the point cloud of the build and then trusted by its
// place: it has to read here as a COPC first. Its header has the exact count and bounds of the
// points of every source, whatever their format: a cloud of fewer than 2 points, or of no extent in
// X or Y, has no COPC. When the points were to be transformed, the COPC is in that CRS and its
// octree splits the cloud: a COPC that does not would serve tiles at the cost the transform avoids.
// When the CRS was to be dropped, the COPC states none. Returns what the COPC reads as.
PointCloudInfo checkBuiltCopc(const fs::path& produced, const raster::CopcParams& params, const raster::CopcResult& result) {
    const std::string& targetCrs = params.targetCrs;
    constexpr size_t MinPointCount = 2;
    // In the point cloud's native units
    constexpr double MinExtent = 1e-10;

    PointCloudInfo info;
    if (!getCopcInfo(io::toUtf8(produced), info) || info.bounds.size() < 6)
        throw PDALException(std::string(raster::kGdalUtilName) + " wrote a COPC that cannot be read");
    if (info.pointCount < MinPointCount)
        throw InvalidArgsException("Point cloud has insufficient points (" + std::to_string(info.pointCount) +
                                   "). At least " + std::to_string(MinPointCount) +
                                   " points are required for COPC generation.");
    const double extentX = info.bounds[3] - info.bounds[0];
    const double extentY = info.bounds[4] - info.bounds[1];
    if (extentX < MinExtent || extentY < MinExtent)
        throw InvalidArgsException("Point cloud has zero or near-zero extent (X: " + std::to_string(extentX) +
                                   ", Y: " + std::to_string(extentY) +
                                   "). Point cloud must have spatial extent for COPC generation.");
    if (params.dropCrs && !info.wktProjection.empty())
        throw AppException(std::string(raster::kGdalUtilName) + " wrote a COPC in " + info.wktProjection +
                           " where no coordinate system was asked for");
    if (targetCrs.empty())
        return info;

    if (!result.transformed || !PointCloudCrsPolicy::isSame(info.wktProjection, targetCrs))
        throw AppException(std::string(raster::kGdalUtilName) + " wrote a COPC in " +
                           (info.wktProjection.empty() ? std::string("no coordinate system") : info.wktProjection) +
                           " where " + targetCrs + " was asked for");
    if (!result.maxNodePoints)
        throw AppException(std::string(raster::kGdalUtilName) + " did not report the octree of the COPC");
    for (const std::string& warning : result.warnings)
        if (warning.find(DegenerateOctreeWarning) != std::string::npos)
            throw AppException("The COPC built in " + targetCrs + " still has a degenerate octree (" +
                               std::to_string(*result.maxNodePoints) + " points in one node): " + warning);
    return info;
}

}  // namespace

void to_json(json& j, const CopcBuildRecord& record) {
    j = {{"crsPolicy", record.crsPolicy},
         {"sourceCrs", record.sourceCrs},
         {"copcCrs", record.copcCrs},
         {"transformed", record.transformed}};
}

void writeCopcBuildRecord(const fs::path& file, const CopcBuildRecord& record) {
    io::writeFileAtomic(file, json(record).dump(), "COPC build record");
}

std::optional<CopcBuildRecord> readCopcBuildRecord(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return std::nullopt;
    const json j = json::parse(in, nullptr, false);
    const auto policy = j.is_object() ? j.find("crsPolicy") : j.end();
    if (!j.is_object() || policy == j.end() || !policy->is_number_integer())
        return std::nullopt;
    CopcBuildRecord record;
    record.crsPolicy = policy->get<int>();
    record.sourceCrs = j.value("sourceCrs", std::string());
    record.copcCrs = j.value("copcCrs", std::string());
    record.transformed = j.value("transformed", false);
    return record;
}

CopcBuildRecord buildCopc(const std::vector<std::string>& filenames, const std::string& outdir,
                          const BuildOptions& options, bool overwrite) {
    const fs::path dest = io::fromUtf8(outdir);
    const fs::path outPath = dest / CopcFileName;
    // Before anything is read or written: the COPC replaces only cloud.copc.laz in dest
    io::assureReplaceable(outPath, overwrite, io::fromUtf8(filenames));
    io::assureFolderExists(dest);
    checkSources(filenames);

    // The CRS of each cloud, as the policy judges it: the host chooses the CRS of the COPC, the
    // helper builds it
    std::vector<PointCloudInfo> inputs(filenames.size());
    for (size_t i = 0; i < filenames.size(); i++)
        if (!getPointCloudInfo(filenames[i], inputs[i]))
            throw InvalidArgsException(filenames[i] + " is not a supported point cloud file");

    // The folder of the output is the caller's: the helper writes the COPC and its temporary files
    // in a private folder made in it, removed with whatever is left in it, and the COPC is moved to
    // its name once it is whole
    const io::ScopedTempDir staging = raster::makePrivateFolder(dest, CopcStagingPrefix);
    const fs::path produced = staging.path() / CopcFileName;

    // The threads of the build come from the CPU pool of this process, which bounds all the builds
    // it runs: the helper has a pool of its own and is told how many it may use. The tokens are
    // held until the helper is done.
    const auto cpu =
        parallel::tryAcquireThreads(static_cast<size_t>(BuildResourceBudget::fromConfig(options).threads));
    raster::CopcParams params;
    params.threads = static_cast<int>(cpu.threads());
    for (size_t i = 1; i < filenames.size(); i++)
        params.inputs.push_back(fs::absolute(io::fromUtf8(filenames[i])).lexically_normal());
    planCopcCrs(inputs, params);

    // The plan is announced when it starts: the helper forwards a fraction only once it passes a
    // whole percent, and the builder reports 0.05 next
    options.reportProgress(0.0, copcPhaseOfFraction(0.0));
    const raster::ProgressCallback report = [&options](double fraction) {
        options.reportProgress(fraction, copcPhaseOfFraction(fraction));
    };
    raster::CopcResult result;
    readPointCloud("COPC build failed: ", [&] {
        result = raster::copcResultFromJson(raster::rasterHelper().buildCopc(
            io::fromUtf8(filenames.front()), produced, raster::toJson(params), report));
    });
    const PointCloudInfo copc = checkBuiltCopc(produced, params, result);

    // The output may have appeared while the COPC was built: replaced only when asked
    io::assureReplaceable(outPath, overwrite);
    raster::publishArtefact(produced, outPath);

    CopcBuildRecord record;
    record.sourceCrs = inputs.front().wktProjection;
    record.copcCrs = copc.wktProjection;
    record.transformed = !params.targetCrs.empty();
    return record;
}

void to_json(json& j, const PointCloudInfo& info) {
    j = json::object();
    j["pointCount"] = info.pointCount;
    j["projection"] = info.wktProjection;
    j["dimensions"] = info.dimensions;
    j["crsStatus"] = toString(info.crsStatus);
}

}  // namespace ddb
