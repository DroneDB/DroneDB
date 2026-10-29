/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef GDAL_RAII_H
#define GDAL_RAII_H

// The one RAII vocabulary for GDAL, OGR, OSR and VSI resources. Every resource that a
// GDAL call hands over is owned by one of these types from the line that receives it, so a
// throw on any later line releases it. No GDALClose, OSRDestroySpatialReference, CSLDestroy,
// VSIFree or VSIUnlink is called by hand outside this header (and gdal_raii.cpp).
//
//   Datasets             GDAL's own GDALDatasetUniquePtr. adoptDataset() takes a handle from the
//                        C API (GDALOpen, GDALCreate, GDALTranslate...), datasetHandle() gives it
//                        back for C calls. Ownership leaves only through an explicit release().
//   Features, geometries GDAL's own OGRFeatureUniquePtr / OGRGeometryUniquePtr (a C geometry
//                        handle through OGRGeometry::FromHandle); FeaturePtr for the C feature
//                        handles (OGR_L_GetNextFeature).
//   Spatial references   OGRSpatialReference by value in C++ code; SrsPtr for the C handles, made
//                        by srsFromWkt/srsFromEpsg/srsFromProj4/srsFromUserInput (always with
//                        the traditional GIS axis order).
//   Transforms           CoordsTransformer (coordstransformer.h) for every point transformation.
//   Option lists         GDAL's own CPLStringList (pass .List() to the C API).
//   Utility options      GdalOptionsPtr<T> (GDALTranslateOptions, GDALWarpAppOptions...).
//   VSI memory           VsiFreePtr<T> for buffers to release with VSIFree/CPLFree;
//                        VsiFilePtr for VSILFILE handles; VsiMemFile for a /vsimem/ file.
//   Config options       ScopedThreadConfigOption (thread-local, restores the previous value).
//
// The handle-typed deleters name the handle type itself (`using pointer = ...H`): the C handles
// are void* in release GDAL builds but typed pointers when GDAL's DEBUG define is on.

#include <initializer_list>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "gdal_inc.h"

namespace ddb
{
    struct VsiFreeDeleter
    {
        void operator()(void *p) const noexcept { VSIFree(p); }
    };
    // A buffer to release with VSIFree/CPLFree (exportToWkt, VSIGetMemFileBuffer, VSIMalloc...)
    template <typename T = void>
    using VsiFreePtr = std::unique_ptr<T, VsiFreeDeleter>;

    // Takes ownership of a dataset handle returned by the GDAL C API (may be null)
    inline GDALDatasetUniquePtr adoptDataset(GDALDatasetH h) noexcept
    {
        return GDALDatasetUniquePtr(GDALDataset::FromHandle(h));
    }

    // The C handle of an owned dataset, for the GDAL C API; ownership stays with ds
    inline GDALDatasetH datasetHandle(const GDALDatasetUniquePtr &ds) noexcept
    {
        return GDALDataset::ToHandle(ds.get());
    }

    // Closes an owned dataset now and returns the error of the close (CE_None before GDAL 3.7):
    // for outputs, whose close writes the data. The deleter of GDALDatasetUniquePtr ignores it.
    inline CPLErr closeDataset(GDALDatasetUniquePtr ds)
    {
        if (!ds)
            return CE_None;
#if GDAL_VERSION_NUM >= 3070000
        return GDALClose(GDALDataset::ToHandle(ds.release()));
#else
        GDALClose(GDALDataset::ToHandle(ds.release()));
        return CE_None;
#endif
    }

    // What closing an output dataset reported (closeOutput)
    struct OutputCloseStatus
    {
        int systemError = 0;   // errno right after the close (ENOSPC: the volume is full)
        bool failed = false;   // GDAL reported a failure: the data was not (all) written
        std::string message;   // GDAL's last error message ("<no GDAL error>" when it has none)
    };

    // Closes an output dataset and reports how it went. An output's close writes what GDAL still
    // holds in its caches (and the header or directory of the file), so a failure is a write that
    // failed, however well the calls before it went. Captures the OS error and GDAL's message
    // before anything else (releasing the source datasets, removing a staging file, logging) can
    // replace them.
    OutputCloseStatus closeOutput(GDALDatasetUniquePtr ds);

    // closeOutput that throws GDALException("Cannot write <what>: <GDAL's message>") on a failure.
    // For the outputs that are not builds (a build maps a full volume to a retryable failure, with
    // the status of closeOutput).
    void closeOutputOrThrow(GDALDatasetUniquePtr ds, const std::string &what);

    // GDALFlushCache of a dataset that stays open (an output that gets overviews next), throwing
    // GDALException("Cannot write <what>: <GDAL's message>") when the flush fails
    void flushOrThrow(GDALDatasetH ds, const std::string &what);

    // Sets the colour interpretation of the bands 1..n of a dataset, throwing GDALException when
    // a band refuses it (a dataset opened read-only does)
    void setBandColors(GDALDatasetH ds, std::initializer_list<GDALColorInterp> colors);

    // The C handles of owned datasets, for the C calls that take an array of them
    // (GDALBuildVRT, GDALWarp)
    inline std::vector<GDALDatasetH> datasetHandles(
        const std::vector<GDALDatasetUniquePtr> &datasets)
    {
        std::vector<GDALDatasetH> out;
        out.reserve(datasets.size());
        for (const auto &ds : datasets)
            out.push_back(datasetHandle(ds));
        return out;
    }

    // The argument list of a GDAL utility (GDALTranslateOptionsNew...): a CPLStringList that
    // owns a copy of every string, so nothing is cast away from a std::string. Pass .List().
    inline CPLStringList cslFromStrings(const std::vector<std::string> &strings)
    {
        CPLStringList list;
        for (const auto &s : strings)
            list.AddString(s.c_str());
        return list;
    }

    // Takes a string that GDAL allocated (exportToWkt, OSRExportToProj4...): returns a copy and
    // frees it, also when the copy throws. "" for null.
    inline std::string takeGdalString(char *s)
    {
        const VsiFreePtr<char> owned(s);
        return s != nullptr ? std::string(s) : std::string();
    }

    // Takes the bytes of the /vsimem/ file at path, which no longer exists after (GDAL's
    // unlink-and-seize). Null (size 0) when there is no such file.
    VsiFreePtr<GByte> stealMemFileBuffer(const std::string &path, vsi_l_offset &size);
    // The same for the int sizes of the C API: throws GDALException when the file is larger than
    // an int (the buffer is freed).
    VsiFreePtr<GByte> stealMemFileBuffer(const std::string &path, int &size);

    struct SrsDeleter
    {
        using pointer = OGRSpatialReferenceH;
        void operator()(OGRSpatialReferenceH srs) const noexcept
        {
            OSRDestroySpatialReference(srs);
        }
    };
    // An owned OGRSpatialReferenceH (OSRNewSpatialReference, OSRClone...)
    using SrsPtr = std::unique_ptr<std::remove_pointer_t<OGRSpatialReferenceH>, SrsDeleter>;

    // A new SRS read from WKT, with the traditional GIS axis order (x = easting or longitude).
    // Null when wkt is null or empty or does not parse.
    SrsPtr srsFromWkt(const char *wkt);
    // A new SRS of an EPSG code, with the traditional GIS axis order. Null when unknown.
    SrsPtr srsFromEpsg(int epsg);
    // A new SRS from a PROJ string, with the traditional GIS axis order. Null when it does not
    // parse.
    SrsPtr srsFromProj4(const char *proj4);
    // A new SRS from anything GDAL accepts ("EPSG:3857", a WKT, a PROJ string, a URN), with the
    // traditional GIS axis order. Null when it does not parse.
    SrsPtr srsFromUserInput(const char *input);

    // The C++ object behind an owned SRS handle (for the C++ API; the handle keeps ownership)
    inline const OGRSpatialReference &srsOf(const SrsPtr &srs) noexcept
    {
        return *OGRSpatialReference::FromHandle(srs.get());
    }

    // A copy of srs without its vertical component, with the traditional GIS axis order: a
    // compound CRS (a projected or geographic CRS plus a height) is left with its horizontal
    // part, any other CRS is copied as it is. For the transformations that need x and y only, which
    // must not depend on the vertical datum (a geoid grid that is not installed, for one).
    // Throws GDALException when GDAL cannot take the vertical part off.
    OGRSpatialReference horizontalSrs(const OGRSpatialReference &srs);

    // Metres in one unit of the vertical CRS of a compound srs; 1 when srs has no vertical CRS
    // (a height in a CRS without one is taken to be metres)
    double verticalUnitInMeters(const OGRSpatialReference &srs);

    struct FeatureDeleter
    {
        using pointer = OGRFeatureH;
        void operator()(OGRFeatureH f) const noexcept { OGR_F_Destroy(f); }
    };
    // An owned OGRFeatureH (OGR_L_GetNextFeature, OGR_F_Create)
    using FeaturePtr = std::unique_ptr<std::remove_pointer_t<OGRFeatureH>, FeatureDeleter>;

    template <typename T, void (*Free)(T *)>
    struct FreeWith
    {
        void operator()(T *p) const noexcept { Free(p); }
    };

    // The free function of each GDAL utility option type
    template <typename T>
    struct GdalOptionsDeleter;
    template <>
    struct GdalOptionsDeleter<GDALTranslateOptions>
        : FreeWith<GDALTranslateOptions, GDALTranslateOptionsFree>
    {
    };
    template <>
    struct GdalOptionsDeleter<GDALWarpAppOptions>
        : FreeWith<GDALWarpAppOptions, GDALWarpAppOptionsFree>
    {
    };
    template <>
    struct GdalOptionsDeleter<GDALBuildVRTOptions>
        : FreeWith<GDALBuildVRTOptions, GDALBuildVRTOptionsFree>
    {
    };
    template <>
    struct GdalOptionsDeleter<GDALVectorTranslateOptions>
        : FreeWith<GDALVectorTranslateOptions, GDALVectorTranslateOptionsFree>
    {
    };
    template <>
    struct GdalOptionsDeleter<GDALNearblackOptions>
        : FreeWith<GDALNearblackOptions, GDALNearblackOptionsFree>
    {
    };
    template <>
    struct GdalOptionsDeleter<GDALWarpOptions> : FreeWith<GDALWarpOptions, GDALDestroyWarpOptions>
    {
    };

    // Owned options of a GDAL utility (GDALTranslateOptionsNew, GDALWarpAppOptionsNew...)
    // or a GDALWarpOptions (GDALCreateWarpOptions)
    template <typename T>
    using GdalOptionsPtr = std::unique_ptr<T, GdalOptionsDeleter<T>>;

    struct VsiFileCloser
    {
        // Releases a handle: a file that was written is closed with VSIFCloseL(release()), whose
        // result says whether its last bytes were written
        void operator()(VSILFILE *f) const noexcept { VSIFCloseL(f); }
    };
    // An open VSILFILE (VSIFOpenL)
    using VsiFilePtr = std::unique_ptr<VSILFILE, VsiFileCloser>;

    // A file of GDAL's virtual file system, normally /vsimem/, removed (VSIUnlink) when the scope
    // exits unless its buffer was stolen or the path released. Movable, not copyable.
    class VsiMemFile
    {
        std::string path_;

    public:
        VsiMemFile() = default;
        // Takes ownership of path (the file need not exist yet)
        explicit VsiMemFile(std::string path) noexcept : path_(std::move(path)) {}
        ~VsiMemFile();

        VsiMemFile(const VsiMemFile &) = delete;
        VsiMemFile &operator=(const VsiMemFile &) = delete;
        VsiMemFile(VsiMemFile &&other) noexcept;
        VsiMemFile &operator=(VsiMemFile &&other) noexcept;

        // Owns the unique path /vsimem/<prefix><16 random characters><suffix> (not created)
        static VsiMemFile unique(const std::string &prefix, const std::string &suffix);

        // stealMemFileBuffer of the owned file
        VsiFreePtr<GByte> stealBuffer(int &size) { return ddb::stealMemFileBuffer(path_, size); }

        // Removes the owned file (if any; never throws), then owns path
        void reset(std::string path = std::string()) noexcept;
        // Gives up ownership: the file is kept and its path returned
        std::string release() noexcept;

        const std::string &path() const noexcept { return path_; }
        const char *c_str() const noexcept { return path_.c_str(); }
    };

    // Sets a GDAL configuration option for the current thread only, and restores the value it had
    // (or unsets it) when the scope exits. Thread-local, so concurrent builds in one process never
    // see each other's setting. A null value unsets the option for the scope.
    class ScopedThreadConfigOption
    {
        std::string key_;
        bool hadPrevious_ = false;
        std::string previous_;

    public:
        ScopedThreadConfigOption(const char *key, const char *value);
        ~ScopedThreadConfigOption();

        ScopedThreadConfigOption(const ScopedThreadConfigOption &) = delete;
        ScopedThreadConfigOption &operator=(const ScopedThreadConfigOption &) = delete;
    };

}

#endif // GDAL_RAII_H
