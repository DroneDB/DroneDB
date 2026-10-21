/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "gdal_raii.h"

#include <limits>
#include <utility>

#include "exceptions.h"
#include "utils.h"

namespace ddb
{
    SrsPtr srsFromWkt(const char *wkt)
    {
        if (wkt == nullptr || wkt[0] == '\0')
            return nullptr;
        SrsPtr srs(OSRNewSpatialReference(nullptr));
        char *wktPtr = const_cast<char *>(wkt);
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
        return VsiMemFile("/vsimem/" + prefix + utils::generateRandomString(16) + suffix);
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
