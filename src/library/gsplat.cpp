/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "gsplat.h"

#include <array>

#include "build_internal.h"
#include "buildlock.h"
#include "buildlod_runner.h"
#include "cancellation.h"
#include "database.h"
#include "dbops.h"
#include "entry_handler.h"
#include "exceptions.h"
#include "json.h"
#include "local_frame_georef.h"
#include "logger.h"
#include "mio.h"
#include "ply.h"
#include "rad.h"
#include "splat_convert.h"
#include "splat_format.h"
#include "splat_frame.h"

namespace ddb
{
    namespace
    {
        // build-lod exits 0 after it refuses a PLY and only prints why; the header of the file
        // says the same before the tool is launched (the rules are ply.h's), so the build fails
        // with what to change in the export
        void assureBuildableSplatSource(const std::string &input)
        {
            if (!splatAccepts(io::fromUtf8(input), SplatStage::Build))
                throw AppException("Cannot build the Gaussian splat " + input + ": its format is not a source (" +
                                   splatExtensionsText(SplatStage::Build) + " are)");

            if (splatFormatOf(io::fromUtf8(input)) != SplatFormat::Ply)
                return;
            PlyHeader header;
            std::string error;
            if (!parsePlyHeader(io::fromUtf8(input), header, &error))
                throw AppException("Cannot build the Gaussian splat " + input + ": invalid PLY header: " + error);
            const std::string problem = plySplatBuildProblem(header);
            if (!problem.empty())
                throw AppException("Cannot build the Gaussian splat " + input + ": " + problem);
        }

        // The up vector of the scene, for the sidecar; an estimate that cannot be made is logged and
        // leaves the bounding box alone
        void addUpEstimate(const fs::path &rad, json &sidecar, bool georeferenced)
        {
            try
            {
                // A georeferenced scene is upright by definition: its splats are not read
                const RadCoarseSplats splats = georeferenced
                                                   ? RadCoarseSplats()
                                                   : readRadCoarseSplats(rad, RadPreviewChunks, /*onlyTheCut=*/true);
                sidecar.update(json(estimateSplatUp(splats, georeferenced)));
            }
            catch (const std::exception &e)
            {
                LOGW << "No up vector for " << io::toUtf8(rad) << ": " << e.what();
            }
        }

        // The footprint of the bounding box on the Earth; one that cannot be made is logged and leaves
        // the sidecar without it
        void addFootprint(const LocalFrameGeoref &georef, const std::array<double, 3> &boundsMin,
                          const std::array<double, 3> &boundsMax, json &sidecar)
        {
            try
            {
                sidecar["wgs84Footprint"] =
                    localFrameFootprintGeometry(georef, LocalBounds{boundsMin[0], boundsMin[1], boundsMax[0], boundsMax[1]});
            }
            catch (const std::exception &e)
            {
                LOGW << "No footprint for the georeferenced splat: " << e.what();
            }
        }

        // Local-space sidecar for deterministic camera framing in the viewer (the streamed .rad has
        // no resident splats at init time): the bounding box, which way is up and, for a georeferenced
        // splat, where the bounding box is on the Earth. The sidecar is optional (the viewer frames the
        // scene without it): one that cannot be written is logged and the build still delivers its
        // model.rad.
        void writeSidecar(const fs::path &rad, const fs::path &boundsFile, const std::array<double, 3> &boundsMin,
                          const std::array<double, 3> &boundsMax, const std::optional<LocalFrameGeoref> &georef)
        {
            try
            {
                json j;
                j["min"] = {boundsMin[0], boundsMin[1], boundsMin[2]};
                j["max"] = {boundsMax[0], boundsMax[1], boundsMax[2]};
                addUpEstimate(rad, j, georef.has_value());
                if (georef.has_value())
                    addFootprint(*georef, boundsMin, boundsMax, j);
                io::writeFileAtomic(boundsFile, j.dump(), "bounds file");
            }
            catch (const std::exception &e)
            {
                LOGW << "Skipping " << io::toUtf8(boundsFile) << ": " << e.what();
            }
        }

        // The sidecar of a model.rad build-lod made. The box is computed by streaming the .rad centres:
        // exact, bounded memory, and in the same coordinate space the viewer renders. A .rad whose
        // centres cannot be decoded is logged and the build still delivers its model.rad.
        void writeBoundsSidecar(const fs::path &rad, const fs::path &boundsFile,
                                const std::optional<LocalFrameGeoref> &georef)
        {
            try
            {
                std::array<double, 3> boundsMin, boundsMax;
                if (!computeRadBounds(rad, boundsMin, boundsMax))
                {
                    LOGW << "Skipping " << io::toUtf8(boundsFile) << ": no centres decoded from " << io::toUtf8(rad);
                    return;
                }
                writeSidecar(rad, boundsFile, boundsMin, boundsMax, georef);
            }
            catch (const std::exception &e)
            {
                LOGW << "Skipping " << io::toUtf8(boundsFile) << ": " << e.what();
            }
        }

        // A .rad upload is already what a build makes, so it is checked and published as model.rad
        // without build-lod: the reader walks the header and every chunk's centres (a file it cannot
        // read is refused here, not served to a viewer that cannot read it either). The file is
        // copied, as an optimized COG is: a hard link would share its data with the dataset file, and
        // a rewrite of that file in place would change the published model.rad under its bounds.json
        // without a build. The copy goes into the build's scratch folder, which is published by
        // renaming it whole, so no reader sees a part of it; a full disk fails the copy with its
        // code, for the build to be retried.
        void publishRad(const std::string &input, const fs::path &source, const fs::path &outdir,
                        const std::optional<LocalFrameGeoref> &georef)
        {
            std::array<double, 3> boundsMin, boundsMax;
            bool any = false;
            try
            {
                any = computeRadBounds(source, boundsMin, boundsMax);
            }
            catch (const AppException &e)
            {
                throw AppException("Cannot build the Gaussian splat " + input + ": it is not a RAD file this can read (" +
                                   e.what() + ")");
            }
            if (!any)
                throw AppException("Cannot build the Gaussian splat " + input + ": the RAD file holds no splats");

            const fs::path rad = outdir / GsplatRadFileName;
            io::copy(source, rad);
            writeSidecar(rad, outdir / GsplatBoundsFileName, boundsMin, boundsMax, georef);
        }

        // Splat count and SH degree of a .ply, from its header
        bool plySplatInfo(const std::string &filename, GaussianSplatInfo &info)
        {
            PlyInfo ply;
            if (!getPlyInfo(io::fromUtf8(filename), ply) || !ply.isSplat)
                return false;
            info.splatCount = ply.vertexCount;
            info.shDegree = ply.shDegree >= 0 ? ply.shDegree : 0;
            return true;
        }
    } // namespace

    void to_json(json &j, const GaussianSplatInfo &info)
    {
        j = json::object();
        j["splatCount"] = info.splatCount;
        j["shDegree"] = info.shDegree;
    }

    bool getGaussianSplatInfo(const std::string &filename, GaussianSplatInfo &info)
    {
        info = GaussianSplatInfo();

        const fs::path path = io::fromUtf8(filename);
        if (!splatAccepts(path, SplatStage::Metadata))
            return false; // no metadata for this format, by design

        switch (splatFormatOf(path))
        {
            case SplatFormat::Ply:
                return plySplatInfo(filename, info);
            case SplatFormat::Splat:
            {
                if (!looksLikeSplat(path))
                    return false;
                info.splatCount = fs::file_size(path) / SplatRecordSize;
                info.shDegree = 0;
                return true;
            }
            case SplatFormat::Spz:
            {
                SpzHeader header;
                if (!readSpzHeader(path, header))
                {
                    LOGD << "Cannot read SPZ info for " << filename;
                    return false;
                }
                info.splatCount = header.numPoints;
                info.shDegree = header.shDegree;
                return true;
            }
            case SplatFormat::Ksplat:
            {
                KsplatHeader header;
                if (!readKsplatHeader(path, header))
                    return false;
                info.splatCount = header.numSplats;
                info.shDegree = header.shDegree;
                return true;
            }
            default:
                return false;
        }
    }

    void buildGsplat(const std::string &input, const std::string &outdir, const std::optional<LocalFrameGeoref> &georef)
    {
        assureBuildableSplatSource(input);

        io::assureFolderExists(io::fromUtf8(outdir));

        if (splatFormatOf(io::fromUtf8(input)) == SplatFormat::Rad)
        {
            publishRad(input, io::fromUtf8(input), io::fromUtf8(outdir), georef);
            return;
        }

        // build-lod is a mandatory dependency: model.rad is the sole delivery artifact (streamed
        // by the viewer, and decoded on demand to render thumbnails). It runs on the source splat
        // itself (staged under a name of ours), with no intermediate .spz: it reads every SPZ
        // version, the gzip container of v1-3 and the NGSP/ZSTD container of v4.
        // A missing external tool fails the build (it would never appear by uploading
        // files). After the tool is installed, the next full build picks the entry up.
        if (!buildlod::isBuildLodAvailable())
            throw AppException(
                "Building Gaussian Splats requires the build-lod tool, which was not found "
                "(set DDB_BUILDLOD_PATH or place build-lod next to the DroneDB executable)");

        const fs::path lodInput = io::fromUtf8(input);
        const fs::path outRad = io::fromUtf8(outdir) / GsplatRadFileName;
        // Its silent steps grow with the number of splats: a large splat gets a longer idle limit
        process::Options lodOptions;
        lodOptions.idleTimeout = buildlod::idleTimeoutFor(lodInput);
        const auto lodResult =
            buildlod::runBuildLodResult({lodInput, outRad, /*quality=*/true, GsplatRadMaxSh}, lodOptions);
        if (!lodResult.succeeded) {
            lodResult.throwIfRetryable("build-lod failed for " + input + ": ");
            throw AppException("build-lod failed for " + input + ": " + lodResult.error);
        }

        writeBoundsSidecar(outRad, io::fromUtf8(outdir) / GsplatBoundsFileName, georef);
    }

    void to_json(json &j, const SplatSpzExport &exported)
    {
        j = {{"path", io::toUtf8(exported.path)}, {"role", GsplatExportRole}, {"cached", exported.cached}};
    }

    namespace
    {
        // What an export found: made after the dataset file and not empty
        bool isCurrentExport(const fs::path &exported, const fs::path &source)
        {
            std::error_code ec;
            if (!fs::is_regular_file(exported, ec) || ec || fs::file_size(exported, ec) == 0 || ec)
                return false;
            const auto madeAt = fs::last_write_time(exported, ec);
            if (ec)
                return false;
            const auto changedAt = fs::last_write_time(source, ec);
            return !ec && madeAt >= changedAt;
        }
    } // namespace

    SplatSpzExport exportSplatSpz(Database *db, const std::string &entryPath)
    {
        const auto entry = getEntry(db, entryPath);
        if (!entry)
            throw EntryNotFoundException("No entry " + entryPath);
        if (entry->type != EntryType::GaussianSplat)
            throw InvalidArgsException(entryPath + " is not a Gaussian splat");

        const fs::path source = db->rootDirectory() / io::fromUtf8(entry->path);
        assureConvertibleToSpz(io::toUtf8(source));
        std::error_code ec;
        if (!fs::is_regular_file(source, ec) || ec)
            throw AppException("The file of the Gaussian splat " + entryPath + " does not exist in the dataset");
        cancel::throwIfCanceled();

        const fs::path baseOutputPath = fs::absolute(db->buildDirectory() / entry->hash);
        const fs::path outputFolder = baseOutputPath / GsplatBuildSubfolder;
        if (!fs::is_directory(baseOutputPath, ec) || ec)
            throw EntryNotFoundException("The Gaussian splat " + entryPath + " is not built: build it first");

        // The lock of the whole build of a splat is the one of its output folder: a build that runs
        // holds it until it has published gsplat/, and none starts while this export writes into it
        BuildLock lock(io::toUtf8(outputFolder));
        if (!entryHandler(entry->type).isComplete(baseOutputPath))
            throw EntryNotFoundException("The Gaussian splat " + entryPath + " is not built: build it first");

        const fs::path exported = outputFolder / GsplatExportFileName;
        if (isCurrentExport(exported, source))
            return {exported, true};

        try
        {
            ensureSufficientDiskSpace(baseOutputPath);
            cancel::throwIfCanceled();
            convertToSpz(io::toUtf8(source), io::toUtf8(exported));
        }
        catch (const FSException &e)
        {
            if (e.code() == std::errc::no_space_on_device)
                throw RetryableBuildException(e.what(), RetryableBuildReason::NoSpaceOnDevice);
            throw;
        }
        return {exported, false};
    }

} // namespace ddb
