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
#include "json_fwd.h"
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

// Band filter maps band symbols (R, G, B, N, Re, T) to 0-based band indices in the raster
struct BandFilter {
    std::string id; // e.g. "RGB", "RGBNRe"
    int R = -1;
    int G = -1;
    int B = -1;
    int N = -1;
    int Re = -1;
    int T = -1;  // Thermal band

    bool has(char c) const;
    int get(char c) const;
};

// Colormap: a 256-entry RGBA lookup table
struct ColormapEntry {
    uint8_t r, g, b, a;
};

// The values a colormap spans: vmin maps to its first color, vmax to its last; a value equal to
// nodata (or NaN) is transparent
struct ColormapScale {
    float vmin = 0;
    float vmax = 1;
    float nodata = 0;
};

struct Colormap {
    std::string id;
    std::string name;
    ColormapEntry entries[256];
    // false for a ramp that belongs to one of the engine's own layers (the vegetation-index
    // map layer): it is not in the list clients choose from and a request cannot name it
    bool listed = true;
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
    // scale: the range of values the colormap spans, and the value that is no data
    void applyColormap(const float *values,
                       uint8_t *rgba,
                       size_t pixelCount,
                       const Colormap &cmap,
                       const ColormapScale &scale) const;

    // The symbols (R, G, B, N, e for RedEdge, T) a formula reads, from its requiredBands
    static std::vector<char> requiredSymbols(const VegetationFormula &formula);

    // The 0-based bands of the raster a formula reads with filter, ascending (the bands that
    // are not mapped by the filter are left out)
    static std::vector<int> bandsRead(const VegetationFormula &formula, const BandFilter &filter);

    // The whole table of colormaps, the unlisted ones included
    const std::vector<Colormap>& getColormaps() const { return colormaps_; }

    // The colormap of a public id (null for an unknown id, and for an unlisted colormap unless
    // includeUnlisted: the engine's own layers resolve their ramp with it)
    const Colormap* getColormap(const std::string &id, bool includeUnlisted = false) const;

    // Get the listed colormaps as JSON (summary with sample colors)
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
