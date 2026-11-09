/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "dem.h"

#include "cancellation.h"
#include "exceptions.h"
#include "artefact_cache.h"
#include "mathutil.h"

#include <algorithm>
#include <cctype>
#include <limits>

namespace ddb
{
    DemContext openDem(const std::string &path, int bandIndex)
    {
        return openDem(path, bandIndex, BandValueOptions());
    }

    DemContext openDem(const std::string &path, int bandIndex, const BandValueOptions &options, bool requireGeo)
    {
        DemContext dem;
        // The path may be a raster a user uploaded (volume, stockpile, contour and profile
        // requests name one): it is read as an artefact, the COG of a build as it is and any
        // other raster as the normalized copy the raster helper wrote
        dem.dataset = raster::openInputRaster(path);
        if (!dem.dataset)
            throw GDALException("Cannot open raster: " + path);
        const GDALDatasetH hDs = dem.handle();

        if (requireGeo)
            requireGeoTransform(hDs, dem.gt, path);
        else if (GDALGetGeoTransform(hDs, dem.gt) != CE_None)
        {
            dem.hasGeoTransform = false;
            const double identity[6] = {0, 1, 0, 0, 0, 1};
            std::copy(identity, identity + 6, dem.gt);
        }

        dem.width = GDALGetRasterXSize(hDs);
        dem.height = GDALGetRasterYSize(hDs);
        const int bandCount = GDALGetRasterCount(hDs);
        if (bandCount < 1)
            throw GDALException("Raster has no bands");
        if (bandIndex < 1 || bandIndex > bandCount)
            throw InvalidArgsException("Invalid band index: " + std::to_string(bandIndex));

        dem.band = GDALGetRasterBand(hDs, bandIndex);
        if (dem.band == nullptr)
            throw GDALException("Cannot access raster band");
        dem.nodata = readBandNodata(dem.band);
        dem.nodataInvalid = dem.nodata.has && nodataOutOfRange(GDALGetRasterDataType(dem.band), dem.nodata.value);
        if (options.nodata)
        {
            // The override replaces the NoData value of the band, valid or not
            dem.nodata = BandNodata{true, *options.nodata};
            dem.nodataOverridden = true;
            dem.nodataInvalid = nodataOutOfRange(GDALGetRasterDataType(dem.band), *options.nodata);
        }

        // The transparency of the band that is not its NoData value: an alpha band or a mask of the
        // dataset (the NoData value is compared here, so that the override can replace it)
        const int maskFlags = GDALGetMaskFlags(dem.band);
        if ((maskFlags & (GMF_ALPHA | GMF_PER_DATASET)) != 0 && (maskFlags & (GMF_NODATA | GMF_ALL_VALID)) == 0)
            dem.maskBand = GDALGetMaskBand(dem.band);

        int hasScale = FALSE, hasOffset = FALSE;
        const double scale = GDALGetRasterScale(dem.band, &hasScale);
        const double offset = GDALGetRasterOffset(dem.band, &hasOffset);
        if ((hasScale && scale != 1.0) || (hasOffset && offset != 0.0))
        {
            dem.hasScale = true;
            dem.scale = hasScale ? scale : 1.0;
            dem.offset = hasOffset ? offset : 0.0;
        }

        const char *unitType = GDALGetRasterUnitType(dem.band);
        dem.unit = unitType != nullptr ? unitType : "";
        dem.zToMeters = elevationUnitToMeters(dem.unit);

        const char *projRef = GDALGetProjectionRef(hDs);
        dem.projection = projRef != nullptr ? projRef : "";
        dem.srs = srsFromWkt(dem.projection.c_str());
        dem.isGeographic = dem.srs && OSRIsGeographic(dem.srs.get()) != 0;
        return dem;
    }

    namespace
    {
        std::unique_ptr<CoordsTransformer> wgs84Transformer(const DemContext &dem, bool toDem)
        {
            if (!dem.srs)
                return nullptr;
            const SrsPtr wgs84 = srsFromEpsg(4326);
            if (!wgs84)
                throw GDALException("Cannot import EPSG:4326 spatial reference system. Is PROJ available?");
            auto t = toDem ? CoordsTransformer::tryCreate(srsOf(wgs84), srsOf(dem.srs))
                           : CoordsTransformer::tryCreate(srsOf(dem.srs), srsOf(wgs84));
            if (!t)
                throw GDALException(toDem ? "Cannot create WGS84 -> raster coordinate transformation"
                                          : "Cannot create raster -> WGS84 coordinate transformation");
            return t;
        }

        // Whether the raster spans the antimeridian: its outline, moved to WGS84, has a longitude beyond
        // +-180 (a geographic raster in 0..360) or jumps by more than 180 degrees between two neighbour
        // points (a projected raster across 180, whose longitudes go from +179.9 to -179.9)
        bool crossesAntimeridian(const DemContext &dem)
        {
            const auto toWgs84 = dem.demToWgs84();
            constexpr int steps = 32;
            const auto lonOf = [&](double col, double row, double &lon) {
                double x = dem.gt[0] + col * dem.gt[1] + row * dem.gt[2];
                double y = dem.gt[3] + col * dem.gt[4] + row * dem.gt[5];
                if (toWgs84 && !toWgs84->tryTransform(1, &x, &y))
                    return false;
                lon = x;
                return std::isfinite(lon);
            };
            const double w = dem.width, h = dem.height;
            // The four sides, each from one corner to the next
            const double corners[5][2] = {{0, 0}, {w, 0}, {w, h}, {0, h}, {0, 0}};
            for (int side = 0; side < 4; side++)
            {
                bool hasPrevious = false;
                double previous = 0;
                for (int i = 0; i <= steps; i++)
                {
                    const double t = static_cast<double>(i) / steps;
                    const double col = corners[side][0] + (corners[side + 1][0] - corners[side][0]) * t;
                    const double row = corners[side][1] + (corners[side + 1][1] - corners[side][1]) * t;
                    double lon;
                    if (!lonOf(col, row, lon))
                    {
                        hasPrevious = false;
                        continue;
                    }
                    if (lon > 180.0 || lon < -180.0)
                        return true;
                    if (hasPrevious && std::fabs(lon - previous) > 180.0)
                        return true;
                    previous = lon;
                    hasPrevious = true;
                }
            }
            return false;
        }
    }

    void DemContext::wgs84BoxToPixelBounds(double minLon, double minLat, double maxLon, double maxLat, int &x0,
                                           int &y0, int &x1, int &y1) const
    {
        // A box whose west side is east of its east side runs across the antimeridian. Swapped, it would be the
        // other ~340 degrees of the Earth (a window over the whole raster): it is refused, unless the raster
        // spans the antimeridian too, where the box goes east from minLon over 180 to maxLon
        if (minLon > maxLon)
        {
            if (!crossesAntimeridian(*this))
                throw InvalidArgsException("Invalid bbox: minLon (" + std::to_string(minLon) +
                                           ") is greater than maxLon (" + std::to_string(maxLon) +
                                           "), a box across the antimeridian, which the raster does not cross. "
                                           "Give the west side as minLon and the east side as maxLon");
            maxLon += 360.0;
        }
        if (minLat > maxLat)
            std::swap(minLat, maxLat);

        // The four sides, 8 steps each
        constexpr int steps = 8;
        double loX = std::numeric_limits<double>::infinity(), hiX = -loX;
        double loY = loX, hiY = -loX;
        const auto add = [&](double lon, double lat) {
            double px, py;
            wgs84ToPixel(lon, lat, px, py);
            loX = std::min(loX, px);
            hiX = std::max(hiX, px);
            loY = std::min(loY, py);
            hiY = std::max(hiY, py);
        };
        for (int i = 0; i <= steps; i++)
        {
            const double lon = minLon + (maxLon - minLon) * i / steps;
            const double lat = minLat + (maxLat - minLat) * i / steps;
            add(lon, minLat);
            add(lon, maxLat);
            add(minLon, lat);
            add(maxLon, lat);
        }

        const auto bound = [](double lo, double hi, int &first, int &last) {
            const double clampLo = std::max(-1e9, std::min(1e9, lo));
            const double clampHi = std::max(-1e9, std::min(1e9, hi));
            first = static_cast<int>(std::ceil(clampLo - 0.5));
            last = static_cast<int>(std::floor(clampHi - 0.5));
            if (last < first)
                first = last = static_cast<int>(std::floor((clampLo + clampHi) / 2.0));
        };
        bound(loX, hiX, x0, x1);
        bound(loY, hiY, y0, y1);
    }

    std::vector<float> DemContext::readRaw(int x, int y, int w, int h, int bufferW, int bufferH) const
    {
        std::vector<float> samples(static_cast<size_t>(bufferW) * bufferH);
        // The read stops when the call is canceled
        cancel::GdalProgress progress;
        GDALRasterIOExtraArg extra = progress.rasterIOExtraArg();
        if (GDALRasterIOEx(band, GF_Read, x, y, w, h, samples.data(), bufferW, bufferH, GDT_Float32, 0, 0, &extra) !=
            CE_None)
        {
            cancel::throwIfCanceled();
            throw AppException("Cannot read raster data");
        }

        std::vector<uint8_t> mask;
        if (maskBand != nullptr)
        {
            mask.resize(samples.size());
            GDALRasterIOExtraArg maskExtra = progress.rasterIOExtraArg();
            if (GDALRasterIOEx(maskBand, GF_Read, x, y, w, h, mask.data(), bufferW, bufferH, GDT_Byte, 0, 0,
                               &maskExtra) != CE_None)
            {
                cancel::throwIfCanceled();
                throw AppException("Cannot read raster data");
            }
        }

        // A pixel with no value reads back as NaN: the NoData value (the override, else the one of the
        // band), a pixel the mask says is transparent, a sample that is not a number
        const float nan = std::numeric_limits<float>::quiet_NaN();
        for (size_t i = 0; i < samples.size(); i++)
        {
            const float v = samples[i];
            if (!std::isfinite(v) || isNodata(v, nodata) || (!mask.empty() && mask[i] == 0))
                samples[i] = nan;
        }
        return samples;
    }

    std::vector<float> DemContext::readValues(int x, int y, int w, int h, int bufferW, int bufferH) const
    {
        std::vector<float> samples = readRaw(x, y, w, h, bufferW, bufferH);
        if (hasScale)
            for (float &v : samples)
                if (!std::isnan(v))
                    v = static_cast<float>(toPhysical(v));
        return samples;
    }

    std::vector<float> DemContext::readElevations(int x, int y, int w, int h, int bufferW, int bufferH) const
    {
        if (!readsNormalised())
        {
            // The samples as the band holds them: the callers compare them with its NoData value (isValid)
            std::vector<float> samples(static_cast<size_t>(bufferW) * bufferH);
            cancel::GdalProgress progress;
            GDALRasterIOExtraArg extra = progress.rasterIOExtraArg();
            if (GDALRasterIOEx(band, GF_Read, x, y, w, h, samples.data(), bufferW, bufferH, GDT_Float32, 0, 0,
                               &extra) != CE_None)
            {
                cancel::throwIfCanceled();
                throw AppException("Cannot read raster data");
            }
            return samples;
        }

        std::vector<float> samples = readValues(x, y, w, h, bufferW, bufferH);
        if (zToMeters != 1.0)
            for (float &v : samples)
                if (!std::isnan(v))
                    v = static_cast<float>(v * zToMeters);
        return samples;
    }

    void DemContext::wgs84ToPixel(double lon, double lat, double &px, double &py) const
    {
        double x = lon, y = lat;
        if (const auto toDem = wgs84ToDem())
            if (!toDem->tryTransform(1, &x, &y))
                throw AppException("Cannot transform the point to the coordinate system of the raster");
        GeoPixelMap(gt).toPixel(x, y, px, py);
    }

    void DemContext::wgs84ToPixelIndex(double lon, double lat, int &col, int &row) const
    {
        double px, py;
        wgs84ToPixel(lon, lat, px, py);
        col = static_cast<int>(std::max(-1e9, std::min(1e9, std::floor(px))));
        row = static_cast<int>(std::max(-1e9, std::min(1e9, std::floor(py))));
    }

    double elevationUnitToMeters(const std::string &unitType)
    {
        std::string unit;
        for (const char c : unitType)
            if (std::isalnum(static_cast<unsigned char>(c)))
                unit += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        constexpr double kFoot = 0.3048;
        constexpr double kUsSurveyFoot = 1200.0 / 3937.0;
        if (unit == "ft" || unit == "foot" || unit == "feet" || unit == "internationalfoot" || unit == "intlfoot")
            return kFoot;
        if (unit == "usft" || unit == "ftus" || unit == "ussurveyfoot" || unit == "ussurveyfeet" ||
            unit == "surveyfoot" || unit == "footsurveyus" || unit == "footussurvey")
            return kUsSurveyFoot;
        return 1.0;
    }

    PixelSize DemContext::pixelSizeMeters(double x, double y) const
    {
        PixelSize size{std::fabs(gt[1]), std::fabs(gt[5])};
        if (isGeographic || !srs)
        {
            const double mPerDeg = 111320.0; // good-enough WGS84 approximation
            size.x = std::fabs(gt[1]) * mPerDeg * std::cos(y * ddb::kDegToRad);
            size.y = std::fabs(gt[5]) * mPerDeg;
            if (size.x < 1e-6) size.x = mPerDeg * std::fabs(gt[1]);
            return size;
        }

        // The pixel with its corner at (x, y): the two sides, and the true area for a sheared or
        // rotated one. GDAL moves the ring to the ellipsoid of the CRS and measures it there.
        const double x1 = x + gt[1], y1 = y + gt[4];
        const double x2 = x1 + gt[2], y2 = y1 + gt[5];
        const double x3 = x + gt[2], y3 = y + gt[5];
        OGRLinearRing ring;
        ring.addPoint(x, y);
        ring.addPoint(x1, y1);
        ring.addPoint(x2, y2);
        ring.addPoint(x3, y3);
        ring.closeRings();
        OGRPolygon pixel;
        pixel.addRing(&ring);
        OGRLineString side;
        side.addPoint(x, y);
        side.addPoint(x1, y1);

        const double sideMeters = side.get_GeodesicLength(&srsOf(srs));
        // Already unsigned; -1 when GDAL cannot measure it, which the check below rejects
        const double areaMeters = pixel.get_GeodesicArea(&srsOf(srs));
        if (!(sideMeters > 0) || !(areaMeters > 0))
            throw AppException("Cannot measure the size of a pixel of the raster on the ground");
        return {sideMeters, areaMeters / sideMeters};
    }

    std::unique_ptr<CoordsTransformer> DemContext::wgs84ToDem() const
    {
        return wgs84Transformer(*this, true);
    }

    std::unique_ptr<CoordsTransformer> DemContext::demToWgs84() const
    {
        return wgs84Transformer(*this, false);
    }
}
