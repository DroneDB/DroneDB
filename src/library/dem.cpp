/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "dem.h"

#include "cancellation.h"
#include "exceptions.h"
#include "gdal_open_policy.h"
#include "mathutil.h"

#include <cctype>
#include <limits>

namespace ddb
{
    DemContext openDem(const std::string &path, int bandIndex)
    {
        DemContext dem;
        // The path may be a raster a user uploaded (volume, stockpile, contour and profile
        // requests name one), so it opens with the drivers its extension allows
        dem.dataset = openUserRaster(path);
        if (!dem.dataset)
            throw GDALException("Cannot open raster: " + path);
        const GDALDatasetH hDs = dem.handle();

        requireGeoTransform(hDs, dem.gt);

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
        const char *unitType = GDALGetRasterUnitType(dem.band);
        dem.zToMeters = elevationUnitToMeters(unitType != nullptr ? unitType : "");

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
    }

    std::vector<float> DemContext::readElevations(int x, int y, int w, int h, int bufferW, int bufferH) const
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
        if (zToMeters != 1.0)
        {
            // The nodata becomes NaN, which isValid rejects: the converted value of a sample could
            // otherwise land on the nodata value
            for (float &v : samples)
                v = isValid(v) ? static_cast<float>(v * zToMeters) : std::numeric_limits<float>::quiet_NaN();
        }
        return samples;
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
