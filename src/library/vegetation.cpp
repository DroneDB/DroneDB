/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "vegetation.h"
#include "sensorprofile.h"
#include "thermal.h"
#include "exceptions.h"
#include "logger.h"

#include <gdal_priv.h>
#include <cmath>
#include <algorithm>
#include <limits>
#include <tuple>
#include <cstdio>

namespace ddb {

// --- BandFilter ---

bool BandFilter::has(char c) const {
    switch (c) {
        case 'R': return R >= 0;
        case 'G': return G >= 0;
        case 'B': return B >= 0;
        case 'N': return N >= 0;
        case 'e': return Re >= 0; // 'e' for RedEdge
        case 'T': return T >= 0;  // Thermal
        default: return false;
    }
}

int BandFilter::get(char c) const {
    switch (c) {
        case 'R': return R;
        case 'G': return G;
        case 'B': return B;
        case 'N': return N;
        case 'e': return Re;
        case 'T': return T;
        default: return -1;
    }
}

BandFilter VegetationEngine::parseFilter(const std::string &filterStr, int bandCount) {
    BandFilter bf;
    bf.id = filterStr;

    // Map filter string characters to 0-based band indices
    int idx = 0;
    for (size_t i = 0; i < filterStr.size() && idx < bandCount; i++) {
        char c = filterStr[i];
        if (c == 'R' && (i + 1) < filterStr.size() && filterStr[i+1] == 'e') {
            bf.Re = idx++;
            i++; // skip 'e'
        } else {
            switch (c) {
                case 'R': bf.R = idx++; break;
                case 'G': bf.G = idx++; break;
                case 'B': bf.B = idx++; break;
                case 'N': bf.N = idx++; break;
                case 'T': bf.T = idx++; break; // Thermal
                case 'L': idx++; break; // LWIR, skip
                case 'P': idx++; break; // Pan, skip
                default: idx++; break;
            }
        }
    }

    return bf;
}

// --- VegetationEngine ---

VegetationEngine::VegetationEngine() {
    initFormulas();
    initColormaps();
}

VegetationEngine& VegetationEngine::instance() {
    static VegetationEngine inst;
    return inst;
}

void VegetationEngine::initFormulas() {
    // Phase 0 - RGB indices
    formulas_.push_back({"VARI", "Visual Atmospheric Resistance Index", "(G - R) / (G + R - B)", "Areas of green vegetation on RGB images", -1, 1, true, "RGB"});
    formulas_.push_back({"EXG", "Excess Green Index", "(2 * G) - (R + B)", "Emphasizes green foliage", 0, 0, false, "RGB"});
    formulas_.push_back({"GLI", "Green Leaf Index", "((G * 2) - R - B) / ((G * 2) + R + B)", "Green leaves and stems", -1, 1, true, "RGB"});
    formulas_.push_back({"vNDVI", "Visible NDVI", "0.5268 * (R^-0.1294 * G^0.3389 * B^-0.3118)", "Approximated NDVI for RGB sensors", 0, 0, false, "RGB"});

    // Phase 0 - NIR indices
    formulas_.push_back({"NDVI", "Normalized Difference Vegetation Index", "(N - R) / (N + R)", "Amount of green vegetation", -1, 1, true, "RN"});
    formulas_.push_back({"NDWI", "Normalized Difference Water Index", "(G - N) / (G + N)", "Water content", -1, 1, true, "GN"});
    formulas_.push_back({"GNDVI", "Green NDVI", "(N - G) / (N + G)", "NDVI on green spectrum", -1, 1, true, "GN"});
    formulas_.push_back({"SAVI", "Soil Adjusted Vegetation Index", "(1.5 * (N - R)) / (N + R + 0.5)", "NDVI with soil correction", -1, 1, true, "RN"});
    formulas_.push_back({"EVI", "Enhanced Vegetation Index", "2.5 * (N - R) / (N + 6*R - 7.5*B + 1)", "Better where NDVI saturates", -1, 1, true, "RBN"});

    // Phase 0 - Red Edge
    formulas_.push_back({"NDRE", "Normalized Difference Red Edge", "(N - Re) / (N + Re)", "Green vegetation in mature crops", -1, 1, true, "NRe"});

    // Phase 1 formulas
    formulas_.push_back({"NDYI", "Normalized Difference Yellowness Index", "(G - B) / (G + B)", "Yellowness detection", -1, 1, true, "GB"});
    formulas_.push_back({"MPRI", "Modified Photochemical Reflectance Index", "(G - R) / (G + R)", "Photochemical reflectance", -1, 1, true, "RG"});
    formulas_.push_back({"OSAVI", "Optimized Soil Adjusted Vegetation Index", "(N - R) / (N + R + 0.16)", "Optimized for sparse vegetation", -1, 1, true, "RN"});
    formulas_.push_back({"GRVI", "Green Ratio Vegetation Index", "N / G", "Green ratio", 0, 0, false, "GN"});
    formulas_.push_back({"ENDVI", "Enhanced NDVI", "((N + G) - (2*B)) / ((N + G) + (2*B))", "Enhanced NDVI", 0, 0, false, "GBN"});
    formulas_.push_back({"ARVI", "Atmospherically Resistant Vegetation Index", "(N - (2*R) + B) / (N + (2*R) + B)", "Corrects atmospheric effects", -1, 1, true, "RBN"});

    // Thermal formulas
    formulas_.push_back({"CELSIUS", "Celsius Temperature", "pixel value (passthrough)", "Temperature in degrees Celsius", -40, 150, true, "T"});
    formulas_.push_back({"KELVIN", "Kelvin Temperature", "pixel + 273.15", "Temperature in Kelvin", 233.15, 423.15, true, "T"});
}

std::vector<VegetationFormula> VegetationEngine::getFormulasForFilter(const BandFilter &filter) const {
    std::vector<VegetationFormula> result;
    for (const auto &f : formulas_) {
        bool compatible = true;
        // Parse requiredBands token-wise so that "Re" is treated as a single RedEdge band.
        for (std::size_t i = 0; i < f.requiredBands.size() && compatible; ) {
            char c = f.requiredBands[i];

            // Treat "Re" as a single RedEdge requirement.
            if (c == 'R' && (i + 1) < f.requiredBands.size() && f.requiredBands[i + 1] == 'e') {
                if (!filter.has('e')) {
                    compatible = false;
                }
                i += 2;
                continue;
            }

            if (c == 'R' && !filter.has('R')) { compatible = false; }
            else if (c == 'G' && !filter.has('G')) { compatible = false; }
            else if (c == 'B' && !filter.has('B')) { compatible = false; }
            else if (c == 'N' && !filter.has('N')) { compatible = false; }
            else if (c == 'e' && !filter.has('e')) { compatible = false; }
            else if (c == 'T' && !filter.has('T')) { compatible = false; }

            ++i;
        }
        if (compatible) result.push_back(f);
    }
    return result;
}

const VegetationFormula* VegetationEngine::getFormula(const std::string &id) const {
    for (const auto &f : formulas_) {
        if (f.id == id) return &f;
    }
    return nullptr;
}

BandFilter VegetationEngine::autoDetectFilter(const std::string &rasterPath) const {
    auto det = SensorProfileManager::instance().detectSensor(rasterPath);
    if (det.detected && !det.bands.empty()) {
        BandFilter bf;
        bf.id = "";
        // Map band names to filter
        for (const auto &bi : det.bands) {
            std::string nameLC = bi.name;
            std::transform(nameLC.begin(), nameLC.end(), nameLC.begin(), ::tolower);

            if (nameLC.find("red edge") != std::string::npos ||
                nameLC.find("rededge") != std::string::npos) {
                if (bf.Re < 0) bf.Re = bi.index - 1; // 0-based
            } else if (nameLC == "red" || nameLC.find("red") == 0) {
                if (bf.R < 0) bf.R = bi.index - 1;
            } else if (nameLC == "green" || nameLC.find("green") == 0) {
                if (bf.G < 0) bf.G = bi.index - 1;
            } else if (nameLC == "blue" || nameLC.find("blue") == 0) {
                if (bf.B < 0) bf.B = bi.index - 1;
            } else if (nameLC == "nir" || nameLC.find("nir") == 0) {
                if (bf.N < 0) bf.N = bi.index - 1;
            } else if (nameLC == "lwir" || nameLC.find("thermal") != std::string::npos) {
                if (bf.T < 0) bf.T = bi.index - 1;
            }
        }

        // Build filter string
        std::string fid;
        if (bf.R >= 0) fid += "R";
        if (bf.G >= 0) fid += "G";
        if (bf.B >= 0) fid += "B";
        if (bf.N >= 0) fid += "N";
        if (bf.Re >= 0) fid += "Re";
        if (bf.T >= 0) fid += "T";
        bf.id = fid;

        return bf;
    }

    // Fallback for undetected sensors
    BandFilter bf;

    // Check if single-band thermal raster
    GDALDatasetH hDs = GDALOpen(rasterPath.c_str(), GA_ReadOnly);
    if (hDs) {
        int nBands = GDALGetRasterCount(hDs);
        GDALClose(hDs);
        if (nBands == 1 && isThermalImage(rasterPath)) {
            bf.id = "T";
            bf.T = 0;
            return bf;
        }
    }

    bf.id = "RGB";
    bf.R = 0;
    bf.G = 1;
    bf.B = 2;
    return bf;
}

void VegetationEngine::applyFormula(const VegetationFormula &formula,
                                     const BandFilter &filter,
                                     const std::vector<float*> &bandData,
                                     float *outData,
                                     size_t pixelCount,
                                     float nodata) const {
    const float EPS = 1e-10f;
    const std::string &id = formula.id;

    // Get band data pointers by symbol
    // bandData indices match the raster band order (0-based)
    auto getBand = [&](char sym) -> const float* {
        int idx = filter.get(sym);
        if (idx < 0 || idx >= static_cast<int>(bandData.size())) return nullptr;
        return bandData[idx];
    };

    const float *pR = getBand('R');
    const float *pG = getBand('G');
    const float *pB = getBand('B');
    const float *pN = getBand('N');
    const float *pRe = getBand('e');

    for (size_t i = 0; i < pixelCount; i++) {
        float R = pR ? pR[i] : 0;
        float G = pG ? pG[i] : 0;
        float B = pB ? pB[i] : 0;
        float N = pN ? pN[i] : 0;
        float Re = pRe ? pRe[i] : 0;

        // Check nodata on any source band used
        bool isNodata = false;
        if (pR && R == nodata) isNodata = true;
        if (pG && G == nodata) isNodata = true;
        if (pB && B == nodata) isNodata = true;
        if (pN && N == nodata) isNodata = true;
        if (pRe && Re == nodata) isNodata = true;

        if (isNodata) {
            outData[i] = nodata;
            continue;
        }

        float result = nodata;

        if (id == "NDVI") {
            float denom = N + R;
            result = (std::abs(denom) < EPS) ? nodata : (N - R) / denom;
        } else if (id == "VARI") {
            float denom = G + R - B;
            result = (std::abs(denom) < EPS) ? nodata : (G - R) / denom;
        } else if (id == "EXG") {
            result = (2.0f * G) - (R + B);
        } else if (id == "GLI") {
            float denom = (G * 2.0f) + R + B;
            result = (std::abs(denom) < EPS) ? nodata : ((G * 2.0f) - R - B) / denom;
        } else if (id == "vNDVI") {
            if (R > EPS && G > EPS && B > EPS) {
                result = 0.5268f * std::pow(R, -0.1294f) * std::pow(G, 0.3389f) * std::pow(B, -0.3118f);
            } else {
                result = nodata;
            }
        } else if (id == "NDWI") {
            float denom = G + N;
            result = (std::abs(denom) < EPS) ? nodata : (G - N) / denom;
        } else if (id == "GNDVI") {
            float denom = N + G;
            result = (std::abs(denom) < EPS) ? nodata : (N - G) / denom;
        } else if (id == "SAVI") {
            float denom = N + R + 0.5f;
            result = (std::abs(denom) < EPS) ? nodata : (1.5f * (N - R)) / denom;
        } else if (id == "EVI") {
            float denom = N + 6.0f * R - 7.5f * B + 1.0f;
            result = (std::abs(denom) < EPS) ? nodata : 2.5f * (N - R) / denom;
        } else if (id == "NDRE") {
            float denom = N + Re;
            result = (std::abs(denom) < EPS) ? nodata : (N - Re) / denom;
        } else if (id == "NDYI") {
            float denom = G + B;
            result = (std::abs(denom) < EPS) ? nodata : (G - B) / denom;
        } else if (id == "MPRI") {
            float denom = G + R;
            result = (std::abs(denom) < EPS) ? nodata : (G - R) / denom;
        } else if (id == "OSAVI") {
            float denom = N + R + 0.16f;
            result = (std::abs(denom) < EPS) ? nodata : (N - R) / denom;
        } else if (id == "GRVI") {
            result = (std::abs(G) < EPS) ? nodata : N / G;
        } else if (id == "ENDVI") {
            float denom = (N + G) + (2.0f * B);
            result = (std::abs(denom) < EPS) ? nodata : ((N + G) - (2.0f * B)) / denom;
        } else if (id == "ARVI") {
            float denom = N + (2.0f * R) + B;
            result = (std::abs(denom) < EPS) ? nodata : (N - (2.0f * R) + B) / denom;
        } else if (id == "CELSIUS") {
            // Thermal passthrough: pixel value is already temperature in °C
            // For single-band thermal rasters, use band index 0 directly
            if (bandData.size() > 0 && bandData[0] != nullptr) {
                float val = bandData[0][i];
                result = (val == nodata) ? nodata : val;
            }
        } else if (id == "KELVIN") {
            // Convert Celsius to Kelvin
            if (bandData.size() > 0 && bandData[0] != nullptr) {
                float val = bandData[0][i];
                result = (val == nodata) ? nodata : val + 273.15f;
            }
        }

        outData[i] = result;
    }
}

void VegetationEngine::applyColormap(const float *values,
                                      uint8_t *rgba,
                                      size_t pixelCount,
                                      const Colormap &cmap,
                                      float vmin, float vmax,
                                      float nodata) const {
    float range = vmax - vmin;
    if (std::abs(range) < 1e-10f) range = 1.0f;

    for (size_t i = 0; i < pixelCount; i++) {
        float v = values[i];
        if (v == nodata || std::isnan(v)) {
            rgba[i * 4 + 0] = 0;
            rgba[i * 4 + 1] = 0;
            rgba[i * 4 + 2] = 0;
            rgba[i * 4 + 3] = 0; // transparent
            continue;
        }

        float normalized = (v - vmin) / range;
        normalized = std::max(0.0f, std::min(1.0f, normalized));
        int idx = static_cast<int>(normalized * 255.0f);
        idx = std::max(0, std::min(255, idx));

        rgba[i * 4 + 0] = cmap.entries[idx].r;
        rgba[i * 4 + 1] = cmap.entries[idx].g;
        rgba[i * 4 + 2] = cmap.entries[idx].b;
        rgba[i * 4 + 3] = cmap.entries[idx].a;
    }
}

const Colormap* VegetationEngine::getColormap(const std::string &id) const {
    for (const auto &c : colormaps_) {
        if (c.id == id) return &c;
    }
    return nullptr;
}

json VegetationEngine::getColormapsJson() const {
    json arr = json::array();
    for (const auto &c : colormaps_) {
        json cj;
        cj["id"] = c.id;
        cj["name"] = c.name;
        // Sample 6 colors for preview
        json colors = json::array();
        for (int i : {0, 51, 102, 153, 204, 255}) {
            char hex[8];
            snprintf(hex, sizeof(hex), "#%02x%02x%02x", c.entries[i].r, c.entries[i].g, c.entries[i].b);
            colors.push_back(hex);
        }
        cj["colors"] = colors;
        arr.push_back(cj);
    }
    return arr;
}

json VegetationEngine::getFormulasJson(const BandFilter &filter) const {
    auto formulas = getFormulasForFilter(filter);
    json arr = json::array();
    for (const auto &f : formulas) {
        json fj;
        fj["id"] = f.id;
        fj["name"] = f.name;
        fj["expr"] = f.expr;
        fj["help"] = f.help;
        if (f.hasRange) {
            fj["range"] = {f.rangeMin, f.rangeMax};
        }
        arr.push_back(fj);
    }
    return arr;
}

// --- Colormap initialization ---

void VegetationEngine::interpolateColormap(ColormapEntry *entries,
    const std::vector<std::tuple<float, uint8_t, uint8_t, uint8_t>> &stops) {

    for (int i = 0; i < 256; i++) {
        float t = static_cast<float>(i) / 255.0f;

        // Find surrounding stops
        size_t lo = 0, hi = stops.size() - 1;
        for (size_t s = 0; s < stops.size() - 1; s++) {
            if (t >= std::get<0>(stops[s]) && t <= std::get<0>(stops[s + 1])) {
                lo = s;
                hi = s + 1;
                break;
            }
        }

        float tLo = std::get<0>(stops[lo]);
        float tHi = std::get<0>(stops[hi]);
        float frac = (tHi > tLo) ? (t - tLo) / (tHi - tLo) : 0.0f;
        frac = std::max(0.0f, std::min(1.0f, frac));

        entries[i].r = static_cast<uint8_t>(std::get<1>(stops[lo]) + frac * (std::get<1>(stops[hi]) - std::get<1>(stops[lo])));
        entries[i].g = static_cast<uint8_t>(std::get<2>(stops[lo]) + frac * (std::get<2>(stops[hi]) - std::get<2>(stops[lo])));
        entries[i].b = static_cast<uint8_t>(std::get<3>(stops[lo]) + frac * (std::get<3>(stops[hi]) - std::get<3>(stops[lo])));
        entries[i].a = 255;
    }
}

void VegetationEngine::initColormaps() {
    // RdYlGn (Red-Yellow-Green) - standard NDVI colormap
    {
        Colormap c;
        c.id = "rdylgn";
        c.name = "Red-Yellow-Green";
        interpolateColormap(c.entries, {
            {0.0f, 215, 48, 39},
            {0.25f, 253, 174, 97},
            {0.5f, 254, 224, 139},
            {0.75f, 166, 217, 106},
            {1.0f, 26, 152, 80}
        });
        colormaps_.push_back(c);
    }

    // Discrete NDVI (5 colors)
    {
        Colormap c;
        c.id = "discrete_ndvi";
        c.name = "Contrast NDVI";
        for (int i = 0; i < 256; i++) {
            float t = static_cast<float>(i) / 255.0f;
            if (t < 0.2f) { c.entries[i] = {139, 0, 0, 255}; }
            else if (t < 0.4f) { c.entries[i] = {255, 69, 0, 255}; }
            else if (t < 0.6f) { c.entries[i] = {255, 215, 0, 255}; }
            else if (t < 0.8f) { c.entries[i] = {50, 205, 50, 255}; }
            else { c.entries[i] = {0, 100, 0, 255}; }
        }
        colormaps_.push_back(c);
    }

    // Spectral
    {
        Colormap c;
        c.id = "spectral";
        c.name = "Spectral";
        interpolateColormap(c.entries, {
            {0.0f, 158, 1, 66},
            {0.25f, 253, 174, 97},
            {0.5f, 255, 255, 191},
            {0.75f, 102, 194, 165},
            {1.0f, 94, 79, 162}
        });
        colormaps_.push_back(c);
    }

    // Viridis
    {
        Colormap c;
        c.id = "viridis";
        c.name = "Viridis";
        interpolateColormap(c.entries, {
            {0.0f, 68, 1, 84},
            {0.25f, 59, 82, 139},
            {0.5f, 33, 144, 140},
            {0.75f, 93, 201, 99},
            {1.0f, 253, 231, 37}
        });
        colormaps_.push_back(c);
    }

    // Plasma
    {
        Colormap c;
        c.id = "plasma";
        c.name = "Plasma";
        interpolateColormap(c.entries, {
            {0.0f, 13, 8, 135},
            {0.25f, 126, 3, 168},
            {0.5f, 204, 71, 120},
            {0.75f, 248, 149, 64},
            {1.0f, 240, 249, 33}
        });
        colormaps_.push_back(c);
    }

    // Inferno
    {
        Colormap c;
        c.id = "inferno";
        c.name = "Inferno";
        interpolateColormap(c.entries, {
            {0.0f, 0, 0, 4},
            {0.25f, 87, 16, 110},
            {0.5f, 188, 55, 84},
            {0.75f, 249, 142, 9},
            {1.0f, 252, 255, 164}
        });
        colormaps_.push_back(c);
    }

    // Magma
    {
        Colormap c;
        c.id = "magma";
        c.name = "Magma";
        interpolateColormap(c.entries, {
            {0.0f, 0, 0, 4},
            {0.25f, 81, 18, 124},
            {0.5f, 183, 55, 121},
            {0.75f, 252, 137, 97},
            {1.0f, 252, 253, 191}
        });
        colormaps_.push_back(c);
    }

    // Grayscale
    {
        Colormap c;
        c.id = "grayscale";
        c.name = "Grayscale";
        for (int i = 0; i < 256; i++) {
            c.entries[i] = {static_cast<uint8_t>(i), static_cast<uint8_t>(i), static_cast<uint8_t>(i), 255};
        }
        colormaps_.push_back(c);
    }

    // Ironbow (thermal)
    {
        Colormap c;
        c.id = "ironbow";
        c.name = "Ironbow";
        interpolateColormap(c.entries, {
            {0.0f, 0, 0, 0},
            {0.2f, 87, 15, 106},
            {0.4f, 189, 55, 84},
            {0.6f, 249, 120, 8},
            {0.8f, 255, 227, 68},
            {1.0f, 255, 255, 255}
        });
        colormaps_.push_back(c);
    }

    // Rainbow
    {
        Colormap c;
        c.id = "rainbow";
        c.name = "Rainbow";
        interpolateColormap(c.entries, {
            {0.0f, 150, 0, 90},
            {0.17f, 0, 0, 200},
            {0.33f, 0, 150, 255},
            {0.5f, 0, 255, 0},
            {0.67f, 255, 255, 0},
            {0.83f, 255, 100, 0},
            {1.0f, 200, 0, 0}
        });
        colormaps_.push_back(c);
    }

    // BuGn (Blue-Green)
    {
        Colormap c;
        c.id = "bugn";
        c.name = "Blue-Green";
        interpolateColormap(c.entries, {
            {0.0f, 247, 252, 253},
            {0.25f, 178, 226, 226},
            {0.5f, 102, 194, 164},
            {0.75f, 35, 139, 69},
            {1.0f, 0, 68, 27}
        });
        colormaps_.push_back(c);
    }

    // Whitehot (thermal)
    {
        Colormap c;
        c.id = "whitehot";
        c.name = "White Hot";
        interpolateColormap(c.entries, {
            {0.0f, 0, 0, 0},
            {1.0f, 255, 255, 255}
        });
        colormaps_.push_back(c);
    }

    // Blackhot (thermal, inverted)
    {
        Colormap c;
        c.id = "blackhot";
        c.name = "Black Hot";
        interpolateColormap(c.entries, {
            {0.0f, 255, 255, 255},
            {1.0f, 0, 0, 0}
        });
        colormaps_.push_back(c);
    }

    // Arctic
    {
        Colormap c;
        c.id = "arctic";
        c.name = "Arctic";
        interpolateColormap(c.entries, {
            {0.0f, 0, 0, 64},
            {0.25f, 0, 100, 200},
            {0.5f, 100, 200, 255},
            {0.75f, 200, 240, 255},
            {1.0f, 255, 255, 255}
        });
        colormaps_.push_back(c);
    }

    // Lava
    {
        Colormap c;
        c.id = "lava";
        c.name = "Lava";
        interpolateColormap(c.entries, {
            {0.0f, 0, 0, 0},
            {0.25f, 128, 0, 0},
            {0.5f, 255, 80, 0},
            {0.75f, 255, 200, 0},
            {1.0f, 255, 255, 200}
        });
        colormaps_.push_back(c);
    }

    // Terrain (elevation: water -> low land -> hills -> mountains -> snow)
    {
        Colormap c;
        c.id = "terrain";
        c.name = "Terrain";
        interpolateColormap(c.entries, {
            {0.0f, 51, 51, 153},
            {0.2f, 0, 180, 180},
            {0.4f, 160, 217, 121},
            {0.6f, 236, 216, 122},
            {0.8f, 169, 116, 67},
            {1.0f, 255, 255, 255}
        });
        colormaps_.push_back(c);
    }

    // Greys (alias of grayscale; maps frontend "greys" id to the standard ramp)
    {
        Colormap c;
        c.id = "greys";
        c.name = "Greys";
        for (int i = 0; i < 256; i++) {
            // Light to dark, matches the frontend preview swatches.
            uint8_t v = static_cast<uint8_t>(255 - i);
            c.entries[i] = {v, v, v, 255};
        }
        colormaps_.push_back(c);
    }
}

} // namespace ddb
