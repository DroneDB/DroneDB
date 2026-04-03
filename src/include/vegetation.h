/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef VEGETATION_H
#define VEGETATION_H

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <cstdint>
#include <tuple>
#include "json.h"
#include "ddb_export.h"

namespace ddb {

struct VegetationFormula {
    std::string id;
    std::string name;
    std::string expr;
    std::string help;
    double rangeMin = 0;
    double rangeMax = 0;
    bool hasRange = false;
    std::string requiredBands; // e.g. "RG", "RGN", "RGNRe"
};

// Band filter maps band symbols (R, G, B, N, Re) to 0-based band indices in the raster
struct BandFilter {
    std::string id; // e.g. "RGB", "RGBNRe"
    int R = -1;
    int G = -1;
    int B = -1;
    int N = -1;
    int Re = -1;

    bool has(char c) const;
    int get(char c) const;
};

// Colormap: a 256-entry RGBA lookup table
struct ColormapEntry {
    uint8_t r, g, b, a;
};

struct Colormap {
    std::string id;
    std::string name;
    ColormapEntry entries[256];
};

class DDB_DLL VegetationEngine {
public:
    static VegetationEngine& instance();

    const std::vector<VegetationFormula>& getAllFormulas() const { return formulas_; }
    std::vector<VegetationFormula> getFormulasForFilter(const BandFilter &filter) const;

    const VegetationFormula* getFormula(const std::string &id) const;

    // Auto-detect band filter from sensor profile
    BandFilter autoDetectFilter(const std::string &rasterPath) const;

    // Parse a band filter string like "RGB", "RGBNRe" into a BandFilter struct
    static BandFilter parseFilter(const std::string &filterStr, int bandCount);

    // Apply formula pixel-by-pixel on band data buffers
    // bandData: array of float buffers, one per band referenced in formula
    // outData: output float buffer
    // pixelCount: number of pixels
    // nodata: nodata value to check/set
    void applyFormula(const VegetationFormula &formula,
                      const BandFilter &filter,
                      const std::vector<float*> &bandData,
                      float *outData,
                      size_t pixelCount,
                      float nodata) const;

    // Apply colormap to formula output
    // values: input float array (formula result)
    // rgba: output RGBA buffer (4 * pixelCount bytes)
    // pixelCount: number of pixels
    // cmap: colormap to use
    // vmin, vmax: rescale range
    void applyColormap(const float *values,
                       uint8_t *rgba,
                       size_t pixelCount,
                       const Colormap &cmap,
                       float vmin, float vmax,
                       float nodata) const;

    // Get available colormaps
    const std::vector<Colormap>& getColormaps() const { return colormaps_; }
    const Colormap* getColormap(const std::string &id) const;

    // Get colormaps as JSON (summary with sample colors)
    json getColormapsJson() const;

    // Get formulas as JSON for a given filter
    json getFormulasJson(const BandFilter &filter) const;

private:
    VegetationEngine();
    VegetationEngine(const VegetationEngine&) = delete;
    VegetationEngine& operator=(const VegetationEngine&) = delete;

    void initFormulas();
    void initColormaps();

    static void interpolateColormap(ColormapEntry *entries,
                                    const std::vector<std::tuple<float, uint8_t, uint8_t, uint8_t>> &stops);

    std::vector<VegetationFormula> formulas_;
    std::vector<Colormap> colormaps_;
};

} // namespace ddb

#endif // VEGETATION_H
