/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef SPLAT_TRAINING_EXPORT_H
#define SPLAT_TRAINING_EXPORT_H

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "fs.h"

namespace ddb
{
    /// What exportSplatTrainingData does besides the defaults of an ODM project.
    struct SplatTrainingExportOptions
    {
        /// Longest side of the exported images in pixels; an image that is not larger keeps its
        /// size. 0 exports the originals, linked (copied when a link is not possible).
        int maxImageSize = 0;

        /// Most points of the sparse model: the cloud is thinned on a 3D grid down to at most
        /// this many. 0 writes none; more than MaxPointsLimit is refused.
        size_t maxPoints = 100000;

        /// The largest maxPoints: the grid that thins the cloud holds every point it keeps and a
        /// cell for each (about 100 bytes a point), so the limit bounds its memory to about a GB.
        static constexpr size_t MaxPointsLimit = 10000000;

        /// Called with the fraction done (0 to 1) and the phase ("cameras", "points", "images",
        /// "model"); returns true to cancel the export (CanceledException). Called one at a
        /// time, from the calling thread or one of the workers of the call.
        std::function<bool(double fraction, const char *phase)> progress;
    };

    /// What was exported.
    struct SplatTrainingExportResult
    {
        /// Things the user should know before training: skipped images, an unusable point
        /// cloud, an approximated lens model, nadir-only captures.
        std::vector<std::string> warnings;
        size_t images = 0; ///< images of the model (and files in images/)
        size_t points = 0; ///< points of points3D.bin
    };

    /**
     * @brief Writes the training data of a Gaussian splat from the outputs of an ODM project.
     *
     * Reads `cameras.json` and `odm_report/shots.geojson` (the poses of the images in the
     * georeferenced CRS), the images in `images/`, `odm_georeferencing/coords.txt` (the CRS and
     * the offset of the local frame), and the point cloud
     * (`odm_georeferencing/odm_georeferenced_model.laz`, else `odm_filterpoints/point_cloud.ply`),
     * and writes in @p outFolder:
     *  - `sparse/0/cameras.bin`, `images.bin`, `points3D.bin`: a COLMAP model (binary format,
     *    little-endian) in the projected CRS minus the offset of coords.txt, so its units are
     *    meters and its axes East, North and Up;
     *  - `images/`: the images the model names, linked from the project or, when
     *    maxImageSize is set, resized (JPEG, quality 95);
     *  - `georef.json`: `{"crs": ..., "offset": [easting, northing, 0]}`. A splat trained from
     *    this model and put next to this file is georeferenced (detectLocalFrameGeoref).
     *
     * Camera models: Brown becomes OPENCV (the third radial term k3, which OPENCV does not have,
     * is folded into k1 and k2 by least squares over the image radius, with a warning), perspective
     * RADIAL, fisheye_opencv OPENCV_FISHEYE. An image of another model (spherical, ...), whose
     * file is missing or whose size is not that of its camera is left out, with a warning.
     * A shot is matched to its camera by the `camera` property of shots.geojson (with ODM's
     * "v2 " prefix), else by size and focal.
     *
     * A cloud with no points or none at all gives an empty points3D.bin and a warning (the
     * trainers start from random points). Most of the images looking straight down is warned
     * about: a splat trained from them is poor from any other angle.
     *
     * @p outFolder must not exist or be an empty folder, and must neither be, hold nor lie inside
     * the project folder (links resolved): the export replaces the files it writes (an image of
     * images/, georef.json), so it never writes where a file it did not make could be replaced, or
     * where a georef.json would become the georeference of the files beside it. Run again, it
     * needs a new or emptied folder. A failed export may leave a partial @p outFolder.
     * @throws AppException when an input of the project is missing (the message names the file) or
     * unusable, or when no image can be exported (the message gives the reasons);
     * InvalidArgsException when an argument is not usable (a folder that does not exist, an output
     * that is a file, is not empty or overlaps the project, a negative size, more points than
     * MaxPointsLimit); CanceledException when canceled.
     */
    SplatTrainingExportResult exportSplatTrainingData(const fs::path &odmProjectFolder, const fs::path &outFolder,
                                                      const SplatTrainingExportOptions &options = {});

} // namespace ddb

#endif // SPLAT_TRAINING_EXPORT_H
