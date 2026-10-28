/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "gsplat.h"

#include <array>

#include "buildlod_runner.h"
#include "exceptions.h"
#include "json.h"
#include "logger.h"
#include "mio.h"
#include "ply.h"
#include "rad.h"
#include "splat_convert.h"
#include "splat_format.h"

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

        // Local-space bounding box sidecar for deterministic camera framing in the viewer (the
        // streamed .rad has no resident splats at init time). Computed by streaming the .rad
        // centres: exact, bounded memory, and in the same coordinate space the viewer renders.
        // The sidecar is optional (the viewer frames the scene without it): a .rad whose centres
        // cannot be decoded, or a file that cannot be written, is logged and the build still
        // delivers its model.rad.
        void writeBoundsSidecar(const fs::path &rad, const fs::path &boundsFile)
        {
            try
            {
                std::array<double, 3> boundsMin, boundsMax;
                if (!computeRadBounds(rad, boundsMin, boundsMax))
                {
                    LOGW << "Skipping " << io::toUtf8(boundsFile) << ": no centres decoded from " << io::toUtf8(rad);
                    return;
                }
                json j;
                j["min"] = {boundsMin[0], boundsMin[1], boundsMin[2]};
                j["max"] = {boundsMax[0], boundsMax[1], boundsMax[2]};
                io::writeFileAtomic(boundsFile, j.dump(), "bounds file");
            }
            catch (const std::exception &e)
            {
                LOGW << "Skipping " << io::toUtf8(boundsFile) << ": " << e.what();
            }
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

    void buildGsplat(const std::string &input, const std::string &outdir)
    {
        assureBuildableSplatSource(input);

        io::assureFolderExists(io::fromUtf8(outdir));

        // build-lod is a mandatory dependency: model.rad is the sole delivery artifact (streamed
        // by the viewer, and decoded on demand to render thumbnails). Run it directly on the
        // source splat so no intermediate .spz is materialized - memory stays bounded even for
        // multi-gigabyte inputs, since build-lod reads the file in chunks.
        // A missing external tool fails the build (it would never appear by uploading
        // files). After the tool is installed, the next full build picks the entry up.
        if (!buildlod::isBuildLodAvailable())
            throw AppException(
                "Building Gaussian Splats requires the build-lod tool, which was not found "
                "(set DDB_BUILDLOD_PATH or place build-lod next to the DroneDB executable)");

        // build-lod reads gzip SPZ (v1-3) only: a NGSP (v4) upload is rewritten as v3 in a
        // scratch folder that is gone when this function returns, before the folder is published
        const fs::path source = io::fromUtf8(input);
        io::ScopedTempDir transcoded;
        fs::path lodInput = source;
        if (splatFormatOf(source) == SplatFormat::Spz && sniffSpzContainer(source) == SpzContainer::Ngsp)
        {
            transcoded = io::ScopedTempDir::create(io::fromUtf8(outdir), ".spz-v3-");
            lodInput = transcoded.path() / "source.spz";
            transcodeSpzToV3(source, lodInput);
        }

        const fs::path outRad = io::fromUtf8(outdir) / GsplatRadFileName;
        const auto lodResult = buildlod::runBuildLodResult({lodInput, outRad, /*quality=*/true, GsplatRadMaxSh});
        if (!lodResult.succeeded) {
            lodResult.throwIfRetryable("build-lod failed for " + input + ": ");
            throw AppException("build-lod failed for " + input + ": " + lodResult.error);
        }

        writeBoundsSidecar(outRad, io::fromUtf8(outdir) / GsplatBoundsFileName);
    }

} // namespace ddb
