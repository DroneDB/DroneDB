/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "gdal_raii.h"
#include "randomutil.h"

#include <cerrno>
#include <limits>
#include <string>
#include <utility>

#include "exceptions.h"

namespace ddb
{
    SrsPtr srsFromWkt(const char *wkt)
    {
        if (wkt == nullptr || wkt[0] == '\0')
            return nullptr;
        SrsPtr srs(OSRNewSpatialReference(nullptr));
        std::string wktCopy(wkt); // OSRImportFromWkt takes a char** it advances (it never writes)
        char *wktPtr = wktCopy.data();
        if (OSRImportFromWkt(srs.get(), &wktPtr) != OGRERR_NONE)
            return nullptr;
        OSRSetAxisMappingStrategy(srs.get(), OAMS_TRADITIONAL_GIS_ORDER);
        return srs;
    }

    SrsPtr srsFromEpsg(int epsg)
    {
        SrsPtr srs(OSRNewSpatialReference(nullptr));
        if (OSRImportFromEPSG(srs.get(), epsg) != OGRERR_NONE)
            return nullptr;
        OSRSetAxisMappingStrategy(srs.get(), OAMS_TRADITIONAL_GIS_ORDER);
        return srs;
    }

    SrsPtr srsFromProj4(const char *proj4)
    {
        if (proj4 == nullptr || proj4[0] == '\0')
            return nullptr;
        SrsPtr srs(OSRNewSpatialReference(nullptr));
        if (OSRImportFromProj4(srs.get(), proj4) != OGRERR_NONE)
            return nullptr;
        OSRSetAxisMappingStrategy(srs.get(), OAMS_TRADITIONAL_GIS_ORDER);
        return srs;
    }

    SrsPtr srsFromUserInput(const char *input)
    {
        if (input == nullptr || input[0] == '\0')
            return nullptr;
        SrsPtr srs(OSRNewSpatialReference(nullptr));
        if (OSRSetFromUserInput(srs.get(), input) != OGRERR_NONE)
            return nullptr;
        OSRSetAxisMappingStrategy(srs.get(), OAMS_TRADITIONAL_GIS_ORDER);
        return srs;
    }

    OGRSpatialReference horizontalSrs(const OGRSpatialReference &srs)
    {
        OGRSpatialReference horizontal(srs);
        if (horizontal.StripVertical() != OGRERR_NONE)
            throw GDALException("Cannot take the vertical part off a coordinate reference system");
        horizontal.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        return horizontal;
    }

    double verticalUnitInMeters(const OGRSpatialReference &srs)
    {
        if (!srs.IsCompound())
            return 1.0;
        const double factor = srs.GetTargetLinearUnits("VERT_CS");
        return factor > 0.0 ? factor : 1.0;
    }

    VsiMemFile::~VsiMemFile()
    {
        reset();
    }

    VsiMemFile::VsiMemFile(VsiMemFile &&other) noexcept : path_(other.release())
    {
    }

    VsiMemFile &VsiMemFile::operator=(VsiMemFile &&other) noexcept
    {
        if (this != &other)
            reset(other.release());
        return *this;
    }

    VsiMemFile VsiMemFile::unique(const std::string &prefix, const std::string &suffix)
    {
        return VsiMemFile("/vsimem/" + prefix + randomString(16) + suffix);
    }

    VsiFreePtr<GByte> stealMemFileBuffer(const std::string &path, vsi_l_offset &size)
    {
        size = 0;
        // bUnlinkAndSeize: the buffer becomes ours and the file is gone
        VsiFreePtr<GByte> data(VSIGetMemFileBuffer(path.c_str(), &size, TRUE));
        if (!data)
            size = 0;
        return data;
    }

    VsiFreePtr<GByte> stealMemFileBuffer(const std::string &path, int &size)
    {
        vsi_l_offset length = 0;
        auto data = stealMemFileBuffer(path, length);
        size = 0;
        if (!data)
            return data;
        if (length > static_cast<vsi_l_offset>(std::numeric_limits<int>::max()))
            throw GDALException("Exceeded max buf size");
        size = static_cast<int>(length);
        return data;
    }

    OutputCloseStatus closeOutput(GDALDatasetUniquePtr ds)
    {
        OutputCloseStatus result;
        if (!ds)
            return result;
        CPLErrorReset();
        errno = 0;
        const CPLErr closed = closeDataset(std::move(ds));
        result.systemError = errno;
        result.failed = closed >= CE_Failure || CPLGetLastErrorType() >= CE_Failure;
        const char *error = CPLGetLastErrorMsg();
        result.message = error && *error ? error : "<no GDAL error>";
        return result;
    }

    void closeOutputOrThrow(GDALDatasetUniquePtr ds, const std::string &what)
    {
        const auto closed = closeOutput(std::move(ds));
        if (closed.failed)
            throw GDALException("Cannot write " + what + ": " + closed.message);
    }

    void flushOrThrow(GDALDatasetH ds, const std::string &what)
    {
        CPLErrorReset();
        const CPLErr flushed = GDALFlushCache(ds);
        if (flushed >= CE_Failure || CPLGetLastErrorType() >= CE_Failure)
        {
            const char *error = CPLGetLastErrorMsg();
            throw GDALException("Cannot write " + what + ": " + (error && *error ? error : "<no GDAL error>"));
        }
    }

    void setBandColors(GDALDatasetH ds, std::initializer_list<GDALColorInterp> colors)
    {
        int band = 1;
        for (const GDALColorInterp color : colors)
        {
            if (GDALSetRasterColorInterpretation(GDALGetRasterBand(ds, band), color) != CE_None)
                throw GDALException("Cannot set the colour interpretation of band " + std::to_string(band));
            band++;
        }
    }

    void VsiMemFile::reset(std::string path) noexcept
    {
        const std::string old = std::exchange(path_, std::move(path));
        // A file that was never written, or whose buffer was stolen, is not there: no-op
        if (!old.empty())
            VSIUnlink(old.c_str());
    }

    std::string VsiMemFile::release() noexcept
    {
        return std::exchange(path_, std::string());
    }

    ScopedThreadConfigOption::ScopedThreadConfigOption(const char *key, const char *value)
        : key_(key)
    {
        const char *previous = CPLGetThreadLocalConfigOption(key, nullptr);
        if (previous)
        {
            hadPrevious_ = true;
            previous_ = previous;
        }
        CPLSetThreadLocalConfigOption(key, value);
    }

    ScopedThreadConfigOption::~ScopedThreadConfigOption()
    {
        CPLSetThreadLocalConfigOption(key_.c_str(), hadPrevious_ ? previous_.c_str() : nullptr);
    }
}
