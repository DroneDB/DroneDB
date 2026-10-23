/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "dem.h"

#include "exceptions.h"

namespace ddb
{
    DemContext openDem(const std::string &path, int bandIndex)
    {
        DemContext dem;
        dem.dataset = adoptDataset(GDALOpen(path.c_str(), GA_ReadOnly));
        if (!dem.dataset)
            throw GDALException("Cannot open raster: " + path);
        const GDALDatasetH hDs = dem.handle();

        if (GDALGetGeoTransform(hDs, dem.gt) != CE_None)
            throw GDALException("Raster has no geotransform: " + path);

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

    std::unique_ptr<CoordsTransformer> DemContext::wgs84ToDem() const
    {
        return wgs84Transformer(*this, true);
    }

    std::unique_ptr<CoordsTransformer> DemContext::demToWgs84() const
    {
        return wgs84Transformer(*this, false);
    }
}
