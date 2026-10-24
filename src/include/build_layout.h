/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef BUILD_LAYOUT_H
#define BUILD_LAYOUT_H

#include <string>
#include <string_view>

#include "fs.h"
#include "mio.h"

/**
 * On-disk layout of the build artifacts: every folder, file and marker
 * name of a build, defined once. Code that reads or writes a build uses these names and never
 * spells one out. Registry, the Hub and existing datasets depend on these exact bytes, so a
 * value here never changes without a migration (tests/unit/build_layout_test.cpp pins them).
 *
 * Layout under the build root (<dataset>/.ddb/DDB_BUILD_PATH, see database.h):
 *
 *     <hash>.pending                   deferred build marker (PendingMarkerExtension)
 *     <hash>.pending.tmp-<rand16>      marker being replaced (PendingMarkerTempTag)
 *     <hash>/<subfolder>/...           artifacts of one entry (the *BuildSubfolder names)
 *     <hash>/<subfolder>.building      build lock of that output folder (BuildLockSuffix)
 *     <hash>/<subfolder>-temp-<rand>   scratch folder of a build (TempFolderInfix)
 *     <hash>/3dtiles-old               previous tileset during a swap (BackupFolderSuffix)
 *     <hash>/3dtiles-temp-trash-<rand> 3D Tiles leftovers moved aside by a cleanup
 *
 * The role-tagged artifact manifest is composed from these names.
 */
namespace ddb {

// ---- Markers and scratch names -------------------------------------------------------------

// "<hash>.pending": a build deferred until its missing dependencies are uploaded.
constexpr const char* PendingMarkerExtension = ".pending";
// "<hash>.pending.tmp-<rand16>": the sibling written before the atomic replace of a marker.
constexpr const char* PendingMarkerTempTag = ".tmp-";
// "<output folder>.building": the BuildLock file of an output folder.
constexpr const char* BuildLockSuffix = ".building";
// "<output folder>-temp-<rand>": the scratch folder a build renames onto its output.
constexpr const char* TempFolderInfix = "-temp-";
// "<output folder>-old": the previous output kept during a two-step swap (3D Tiles).
constexpr const char* BackupFolderSuffix = "-old";
// "<hash>/3dtiles-temp-trash-<rand16>": 3D Tiles leftovers moved aside by a cleanup.
constexpr const char* TrashFolderTag = "trash-";
// "<hash>/.vec-stage-<rand>" and "<hash>/.vec-backup-<rand16>": vector publish scratch.
constexpr const char* VectorStagingPrefix = ".vec-stage-";
constexpr const char* VectorBackupPrefix = ".vec-backup-";

// ---- Output subfolders (one per artifact family) -------------------------------------------

constexpr const char* CogBuildSubfolder = "cog";
constexpr const char* CopcBuildSubfolder = "copc";
// Primary output of a Model build. Its ".building" lock is held for the whole Model build.
constexpr const char* NexusBuildSubfolder = "nxs";
constexpr const char* VectorBuildSubfolder = "vec";
constexpr const char* MvtBuildSubfolder = "mvt";
constexpr const char* GsplatBuildSubfolder = "gsplat";
// OGC 3D Tiles: the output of a .3tz entry, and the additive output of a Model build.
constexpr const char* Tiles3DBuildSubfolder = "3dtiles";

// ---- Artifact files ------------------------------------------------------------------------

constexpr const char* CogFileName = "cog.tif";
constexpr const char* CopcFileName = "cloud.copc.laz";
constexpr const char* NexusFileName = "model.nxz";
// Vector: the GeoPackage copy, the MVT pyramid descriptor, and the drawing-coordinate
// metadata of a CAD vector without CRS (written instead of an MVT pyramid).
constexpr const char* VectorGpkgFileName = "source.gpkg";
constexpr const char* MvtMetadataFileName = "metadata.json";
constexpr const char* VectorLocalFileName = "local.json";
// Root descriptor of an OGC 3D Tiles tileset (also the root entry of a .3tz archive).
constexpr const char* TilesetFileName = "tileset.json";

// Gaussian Splat. Mirrors the COPC/COG/NXS conventions.
constexpr const char* GsplatFileName = "model.spz";
// Level-of-detail delivery artifact (Spark "RAD" container). Produced by the vendored
// build-lod tool; streamed progressively by the viewer via HTTP range requests (paged: true).
// This is the sole delivery artifact - the intermediate .spz is not kept after a successful build.
constexpr const char* GsplatRadFileName = "model.rad";
// Tiny sidecar with the splat's local-space axis-aligned bounding box, written at build
// time. The viewer uses it to frame the camera deterministically. This is essential for the
// paged .rad path: a streamed mesh has no resident splats (hence no computable bounds) when
// it first initializes, so client-side auto-framing cannot work without this hint.
constexpr const char* GsplatBoundsFileName = "bounds.json";
// Thumbnail filename constant (kept for reference in test assertions).
// Thumbnails are generated on demand from the .rad/.spz at request time - no build-time
// thumbnail artifact is produced. The constant is used to verify that buildGsplat does
// not write a thumbnail file alongside the delivery artifact.
constexpr const char* GsplatThumbFileName = "thumbnail.webp";

// Legacy Entwine Point Tiles (EPT), the point cloud output before COPC. Never written any
// more; builds remove leftovers, and a point cloud inside an "ept-data" folder is not built.
constexpr const char* LegacyEptBuildSubfolder = "ept";
constexpr const char* LegacyEptFileName = "ept.json";
constexpr const char* LegacyEptDataFolder = "ept-data";
constexpr const char* LegacyEptHierarchyFolder = "ept-hierarchy";

// ---- Composed names ------------------------------------------------------------------------

// "<outputPath>.building"
inline std::string buildLockPath(const std::string& outputPath) {
    return outputPath + BuildLockSuffix;
}

// "<build root>/<hash>.pending" for the entry folder "<build root>/<hash>"
inline fs::path pendingMarkerPath(const fs::path& entryBuildDir) {
    return io::concat(entryBuildDir, PendingMarkerExtension);
}

// ".pending.tmp-": the infix of a marker being replaced, after the 64-character hash
inline std::string pendingMarkerTempInfix() {
    return std::string(PendingMarkerExtension) + PendingMarkerTempTag;
}

// "<dir>-old", next to <dir>
inline fs::path backupFolderPath(const fs::path& dir) {
    return dir.parent_path() / io::concat(dir.filename(), BackupFolderSuffix);
}

// "3dtiles-old"
inline std::string tiles3DBackupFolderName() {
    return std::string(Tiles3DBuildSubfolder) + BackupFolderSuffix;
}

// "3dtiles-temp-trash-": the prefix of the 3D Tiles leftovers moved aside by a cleanup
inline std::string tiles3DTrashPrefix() {
    return std::string(Tiles3DBuildSubfolder) + TempFolderInfix + TrashFolderTag;
}

// Build artifact names are content hashes (SHA-256 hex, 64 characters). Callers check a
// candidate name before acting on it, so unrelated files in a user-specified output path are
// never touched.
inline bool isHashLike(std::string_view s) {
    if (s.size() != 64)
        return false;
    for (const char c : s) {
        const bool digit = c >= '0' && c <= '9';
        const bool lower = c >= 'a' && c <= 'f';
        const bool upper = c >= 'A' && c <= 'F';
        if (!digit && !lower && !upper)
            return false;
    }
    return true;
}

}  // namespace ddb

#endif  // BUILD_LAYOUT_H
