/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "pointcloud.h"
#include "json.h"
#include "stringutil.h"

#include <algorithm>
#include <fstream>
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
#include "mio.h"
#include "parallel.h"
#include "ply.h"
#include "raster_artefacts.h"
#include "raster_helper.h"
#include "raster_ops.h"
#include "tilers_call.h"

namespace ddb {

namespace {

// Fills the polygon and the centroid of the point cloud (in EPSG:footprintSrs) from its bounds. Only the
// horizontal part of the CRS of the file takes part: x and y are transformed, z is not (a height
// in the vertical unit of the CRS, converted to metres), so a vertical datum that PROJ cannot use
// here (a geoid grid that is not installed) does not matter. Throws GDALException when the
// horizontal CRS cannot be transformed.
void populatePointCloudFootprint(PointCloudInfo& info, int footprintSrs) {
    const std::vector<double>& b = info.bounds;  // minX, minY, minZ, maxX, maxY, maxZ

    const SrsPtr fileSrs = srsFromWkt(info.wktProjection.c_str());
    if (!fileSrs)
        throw GDALException("Cannot read the coordinate reference system of the file. Is PROJ available?");

    const SrsPtr tgt = srsFromEpsg(footprintSrs);
    if (!tgt)
        throw GDALException("Cannot read EPSG:" + std::to_string(footprintSrs) + ". Is PROJ available?");

    const OGRSpatialReference& fullSrs = srsOf(fileSrs);
    const OGRSpatialReference horizontal = horizontalSrs(fullSrs);
    const double zToMeters = verticalUnitInMeters(fullSrs);

    const auto transform = CoordsTransformer::tryCreate(horizontal, srsOf(tgt));
    if (!transform) {
        const char* name = horizontal.GetName();
        throw GDALException("No coordinate transformation from " +
                            std::string(name != nullptr ? name : "the CRS of the file") + " to EPSG:" +
                            std::to_string(footprintSrs));
    }

    double geoMinX = b[0];
    double geoMinY = b[1];
    double geoMaxX = b[3];
    double geoMaxY = b[4];
    double centroidX = (b[0] + b[3]) / 2.0;
    double centroidY = (b[1] + b[4]) / 2.0;

    if (!transform->tryTransform(1, &geoMinX, &geoMinY) || !transform->tryTransform(1, &geoMaxX, &geoMaxY) ||
        !transform->tryTransform(1, &centroidX, &centroidY)) {
        std::ostringstream bounds;
        bounds << "[" << b[0] << ", " << b[1] << ", " << b[3] << ", " << b[4] << "]";
        throw GDALException("Cannot transform the bounds " + bounds.str() + " to EPSG:" +
                            std::to_string(footprintSrs));
    }

    const double geoMinZ = b[2] * zToMeters;
    const double geoMaxZ = b[5] * zToMeters;

    if (geoMinZ < -30000 || geoMaxZ > 30000 || (geoMinX == -90 && geoMaxX == 90)) {
        LOGD << "Strange point cloud bounds [[" << geoMinX << ", " << geoMaxX << "], [" << geoMinY << ", "
             << geoMaxY << "], [" << geoMinZ << ", " << geoMaxZ << "]]";
        info.bounds.clear();
        return;
    }

    info.polyBounds.addPoint(geoMinX, geoMinY, geoMinZ);
    info.polyBounds.addPoint(geoMaxX, geoMinY, geoMinZ);
    info.polyBounds.addPoint(geoMaxX, geoMaxY, geoMinZ);
    info.polyBounds.addPoint(geoMinX, geoMaxY, geoMinZ);
    info.polyBounds.addPoint(geoMinX, geoMinY, geoMinZ);
    info.centroid.addPoint(centroidX, centroidY, geoMinZ);
}

// The footprint of a cloud with a CRS and bounds. A footprint that cannot be computed does not make
// the file unreadable: it is indexed and built without one (the native bounds stay), and the
// entry says why it has none.
void addFootprint(PointCloudInfo& info, int footprintSrs) {
    info.centroid.clear();
    info.polyBounds.clear();
    info.footprintWarning.clear();
    if (info.bounds.size() != 6 || info.wktProjection.empty())
        return;
    try {
        populatePointCloudFootprint(info, footprintSrs);
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
    addFootprint(info, footprintSrs);
    return info;
}

// The extensions of the point clouds ddb-tilers reads for an entry (a PLY point cloud is read
// from its header), as the point cloud entries have them
bool isTilersPointCloud(const io::Path& path) {
    return path.checkExtension({"las", "laz", "e57", "pts", "xyz"});
}

}  // namespace

PointCloudInfo readPointCloudInfo(const fs::path& path) {
    gdalopen::requireUserFileOpens("readPointCloudInfo");
    // The bounds of a text file (XYZ, PTS) were never kept: only a header gives them
    const bool text = io::Path(path).checkExtension({"pts", "xyz"});
    return tilersPointCloudInfo(io::toUtf8(path), !text, 4326);
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

void buildCopcFile(const fs::path& path, const fs::path& outFile, const CopcParams& params,
                   const ProgressCallback& progress) {
    // The builder runs a pool of its own of the threads it is given, the calling one included
    const auto cpu = acquireBuildThreads(params.threads);
    json options = {{"threads", cpu.threads()}, {"metadata", false}};
    if (!params.inputs.empty()) {
        json inputs = json::array();
        for (const fs::path& input : params.inputs)
            inputs.push_back(io::toUtf8(input));
        options["inputs"] = std::move(inputs);
    }

    LOGD << "Building COPC from " << io::toUtf8(path) << " and " << params.inputs.size()
         << " more file(s) with ddb-tilers (" << cpu.threads() << " threads)";
    tilers::BuildProgress forward("COPC", progress);
    const ddbtErr rc = ddbtBuildCopc2(io::toUtf8(path).c_str(), io::toUtf8(outFile).c_str(), options.dump().c_str(),
                                      &tilers::BuildProgress::forward, &forward);
    // An input the builder cannot read (a file that is no point cloud it knows, one without
    // points, a coordinate it cannot store) is a file the helper cannot read
    tilers::checkBuild(rc, "The COPC build",
                       [](const std::string& message) { throw RasterOpException(ErrorCode::Unreadable, message); });
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

// The COPC the helper wrote is published as the point cloud of the build and then trusted by its
// place: it has to read here as a COPC first. Its header has the exact count and bounds of the
// points of every source, whatever their format: a cloud of fewer than 2 points, or of no extent in
// X or Y, has no COPC.
void checkBuiltCopc(const fs::path& produced) {
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
}

}  // namespace

void buildCopc(const std::vector<std::string>& filenames, const std::string& outdir, const BuildOptions& options,
               bool overwrite) {
    const fs::path dest = io::fromUtf8(outdir);
    const fs::path outPath = dest / CopcFileName;
    // Before anything is read or written: the COPC replaces only cloud.copc.laz in dest
    io::assureReplaceable(outPath, overwrite, io::fromUtf8(filenames));
    io::assureFolderExists(dest);
    checkSources(filenames);

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

    // The plan is announced when it starts: the helper forwards a fraction only once it passes a
    // whole percent, and the builder reports 0.05 next
    options.reportProgress(0.0, copcPhaseOfFraction(0.0));
    const raster::ProgressCallback report = [&options](double fraction) {
        options.reportProgress(fraction, copcPhaseOfFraction(fraction));
    };
    readPointCloud("COPC build failed: ", [&] {
        raster::rasterHelper().buildCopc(io::fromUtf8(filenames.front()), produced, raster::toJson(params), report);
    });
    checkBuiltCopc(produced);

    // The output may have appeared while the COPC was built: replaced only when asked
    io::assureReplaceable(outPath, overwrite);
    raster::publishArtefact(produced, outPath);
}

void to_json(json& j, const PointCloudInfo& info) {
    j = json::object();
    j["pointCount"] = info.pointCount;
    j["projection"] = info.wktProjection;
    j["dimensions"] = info.dimensions;
}

}  // namespace ddb
