/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef TILERS_RASTER_H
#define TILERS_RASTER_H

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "copied_metadata.h"
#include "fs.h"
#include "gdal_metadata.h"

struct ddbtRaster;

namespace ddb::tilers
{
    // A raster libddb reads in this process with ddb-tilers: the COG of a build, an artefact, or the
    // normalized copy of any other raster (raster::artefactFor), the file raster::openInputRaster opens
    // with GDAL. What it reports and reads is what GDAL reports and reads of the file (ddbtRasterInfo,
    // ddbtRasterRead: nearest resampling, GDAL's choice of overview). A raster is read by several
    // threads at once.
    class Raster
    {
      public:
        // One band, as GDAL reports it
        struct Band
        {
            std::string colorInterp;     // GDALGetColorInterpretationName
            std::optional<double> nodata;
            std::optional<double> scale, offset;
            std::string unit, description;
            std::vector<std::pair<std::string, std::string>> metadata;  // the items of its default domain
            std::vector<std::array<uint8_t, 4>> colorTable;             // band 1: RGBA entries; empty: none
        };

        // The raster libddb reads for `path` (UTF-8), as raster::openInputRaster opens it: none for a
        // file that is not there or that cannot be read. Only in the host (std::logic_error in the
        // raster helper, which reads the files of users with GDAL).
        static std::unique_ptr<Raster> openInput(const std::string &path);
        // A file of libddb itself (a build output, an artefact). @throws GDALException, FSException
        static std::unique_ptr<Raster> open(const fs::path &file);
        // The same, the file opened for this raster alone and closed with it (open keeps it for the
        // next reader): for a file renamed or removed right after it is read, which Windows refuses
        // while a process holds it open. @throws GDALException, FSException
        static std::unique_ptr<Raster> openPrivate(const fs::path &file);

        ~Raster();
        Raster(const Raster &) = delete;
        Raster &operator=(const Raster &) = delete;

        // The file read: the artefact of the path openInput was given
        const fs::path &file() const { return file_; }
        int width() const { return width_; }
        int height() const { return height_; }
        int bandCount() const { return static_cast<int>(bands_.size()); }
        // GDAL's name of the data type ("Byte", "Float32"...)
        const std::string &dataType() const { return dataType_; }
        int blockWidth() const { return blockWidth_; }
        int blockHeight() const { return blockHeight_; }
        const Band &band(int band1) const { return bands_.at(static_cast<size_t>(band1 - 1)); }
        // The sizes (width, height) of the overviews, largest first (GDALGetOverview(band, i))
        const std::vector<std::array<int, 2>> &overviews() const { return overviews_; }
        const std::optional<std::array<double, 6>> &geoTransform() const { return geoTransform_; }
        // The CRS as WKT1 GDAL; empty without one
        const std::string &wkt() const { return wkt_; }
        // The metadata of the raster as GDAL lists it (ddbtRasterMetadata), read on the first call for the file
        // @throws GDALException
        gdalmeta::RasterMetadata metadata() const;

        // Reads the window (x, y, width, height) of `bands` (1-based) into bufWidth x bufHeight samples
        // per band, band after band, as GDAL's RasterIO reads it into Float32 or Float64; `mask`, when
        // set, receives the dataset mask (255 without one). @throws InvalidArgsException, GDALException
        void read(int x, int y, int width, int height, int bufWidth, int bufHeight, std::span<const int> bands,
                  float *out, uint8_t *mask = nullptr) const;
        void read(int x, int y, int width, int height, int bufWidth, int bufHeight, std::span<const int> bands,
                  double *out, uint8_t *mask = nullptr) const;

        // Whether `band1` has a mask that is not its NoData value (GDALGetMaskFlags with GMF_ALPHA or
        // GMF_PER_DATASET and without GMF_NODATA or GMF_ALL_VALID): the mask of the file, or the alpha band
        // of an RGBA or grey + alpha raster for its other bands. The bands that have one share it.
        bool hasMask(int band1) const;
        // Whether band 1's mask is a mask of the dataset that is neither the alpha band nor the
        // NoData value (GDALGetMaskFlags == GMF_PER_DATASET: an internal mask, which wins over an
        // alpha band)
        bool hasDatasetMask() const { return mask_ == Mask::PerDataset; }

        // read() of `bands`, and in `mask` (when one of them hasMask, else left untouched) the samples of
        // their mask band as GDAL's RasterIO reads it into Byte: 0 where a pixel is transparent.
        // @throws InvalidArgsException, GDALException
        void readMasked(int x, int y, int width, int height, int bufWidth, int bufHeight, std::span<const int> bands,
                        float *out, uint8_t *mask) const;
        void readMasked(int x, int y, int width, int height, int bufWidth, int bufHeight, std::span<const int> bands,
                        double *out, uint8_t *mask) const;
        // readMasked() of one band
        void readBand(int band1, int x, int y, int width, int height, int bufWidth, int bufHeight, float *out,
                      uint8_t *mask) const;
        void readBand(int band1, int x, int y, int width, int height, int bufWidth, int bufHeight, double *out,
                      uint8_t *mask) const;
        // The window (x, y, width, height) of level `level` (0: the raster, i: overviews()[i - 1]) of `bands`,
        // pixel for pixel, with their mask as readMasked() gives it. @throws InvalidArgsException, GDALException
        void readLevel(int level, int x, int y, int width, int height, std::span<const int> bands, double *out,
                       uint8_t *mask) const;

      private:
        // What GDALGetMaskFlags says of band 1
        enum class Mask
        {
            AllValid,
            PerDataset,
            Nodata,
            Alpha
        };

        Raster(ddbtRaster *handle, fs::path file);
        static std::unique_ptr<Raster> openWithFlags(const fs::path &file, unsigned int flags);
        // Calls read(bands, sampleType, out, datasetMask) for `bands` (then for the alpha band, in its own type,
        // when it is their mask), and fills `mask` as readMasked() does; `count` is the samples of a band
        template <typename T, typename Read>
        void readWithMask(std::span<const int> bands, size_t count, T *out, uint8_t *mask, const Read &read) const;

        ddbtRaster *handle_;
        fs::path file_;
        int width_ = 0, height_ = 0;
        std::string dataType_;
        int blockWidth_ = 0, blockHeight_ = 0;
        Mask mask_ = Mask::AllValid;
        int alphaBand_ = 0;
        std::vector<Band> bands_;
        std::vector<std::array<int, 2>> overviews_;
        std::optional<std::array<double, 6>> geoTransform_;
        std::string wkt_;
    };

    // A warp of a raster into memory on a grid, as gdalwarp -of MEM -t_srs -te [-te_srs] -ts -r [-b]
    // [-dstalpha] [-dstnodata] makes it (ddbtWarpRaster)
    struct WarpRequest
    {
        std::string targetCrs;            // -t_srs (any text PROJ reads)
        std::array<double, 4> extent{};   // -te
        std::string extentCrs;            // -te_srs ("": targetCrs)
        int width = 0, height = 0;        // -ts
        bool nearest = false;             // -r near, else bilinear
        std::vector<int> bands;           // -b (1-based; none: every band but a last alpha band)
        bool dstAlpha = false;            // -dstalpha
        std::optional<double> dstNodata;  // -dstnodata
        std::string outputType;           // -ot (GDAL's name; "": the source's type)
        bool noSrcAlpha = false;          // -nosrcalpha: a last alpha band is warped as data
        bool setColorInterp = false;      // -setci: the bands named as their source bands
    };

    // The output of a warp: what GDAL reports of gdalwarp's MEM dataset, and its samples
    struct WarpedImage
    {
        int width = 0, height = 0;
        std::string dataType;                            // GDAL's name ("Byte", "Float32"...)
        std::array<double, 6> geoTransform{};
        std::string wkt;                                 // the CRS (WKT1 GDAL)
        std::vector<std::string> colorInterp;            // one per band
        std::optional<double> nodata;                    // of every band but the alpha band
        int alphaBand = 0;                               // 1-based, 0 when there is none
        std::vector<std::array<uint8_t, 4>> colorTable;  // of band 1 (RGBA), empty when it has none
        std::vector<uint8_t> pixels;                     // band after band, rows of width samples

        int bandCount() const { return static_cast<int>(colorInterp.size()); }
        size_t sampleSize() const;
        // The samples of band `band1`
        const uint8_t *band(int band1) const;
        // The samples of band `band1` as T: float or double (GDALCopyWords to Float32 or Float64)
        template <typename T> void readBand(int band1, std::vector<T> &out) const;
    };

    // Warps `file` (a file of libddb: Raster::file()). @throws InvalidArgsException, GDALException
    WarpedImage warp(const fs::path &file, const WarpRequest &request);

    // How writeGeoTiff writes an image
    struct GeoTiffOptions
    {
        std::string crs;          // a CRS with an EPSG code; empty: the image is not georeferenced
        bool compress = true;     // DEFLATE (with the predictor of its type), else none
        CopiedMetadata metadata;  // what it takes from the raster it was made from
    };

    // Writes `image` to `output`, a tiled GeoTIFF that GDAL reads as it reads gdalwarp's GeoTIFF of the
    // same warp: the samples, the colour interpretations, the nodata value, the colour table of band 1
    // and the metadata of the options. @throws InvalidArgsException, GDALException
    void writeGeoTiff(const WarpedImage &image, const fs::path &output, const GeoTiffOptions &options);
}

#endif // TILERS_RASTER_H
