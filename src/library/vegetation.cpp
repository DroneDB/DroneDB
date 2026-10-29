/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "vegetation.h"
#include "json.h"
#include "stringutil.h"
#include "sensorprofile.h"
#include "thermal.h"
#include "exceptions.h"
#include "artefact_cache.h"
#include "gdal_raii.h"
#include "logger.h"

#include <gdal_priv.h>
#include <cmath>
#include <algorithm>
#include <limits>
#include <utility>
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
    // RGB indices
    formulas_.push_back({"VARI", "Visual Atmospheric Resistance Index", "(G - R) / (G + R - B)", "Areas of green vegetation on RGB images", -1, 1, true, "RGB"});
    formulas_.push_back({"EXG", "Excess Green Index", "(2 * G) - (R + B)", "Emphasizes green foliage", 0, 0, false, "RGB"});
    formulas_.push_back({"GLI", "Green Leaf Index", "((G * 2) - R - B) / ((G * 2) + R + B)", "Green leaves and stems", -1, 1, true, "RGB"});
    formulas_.push_back({"vNDVI", "Visible NDVI", "0.5268 * (R^-0.1294 * G^0.3389 * B^-0.3118)", "Approximated NDVI for RGB sensors", 0, 0, false, "RGB"});

    // NIR indices
    formulas_.push_back({"NDVI", "Normalized Difference Vegetation Index", "(N - R) / (N + R)", "Amount of green vegetation", -1, 1, true, "RN"});
    formulas_.push_back({"NDWI", "Normalized Difference Water Index", "(G - N) / (G + N)", "Water content", -1, 1, true, "GN"});
    formulas_.push_back({"GNDVI", "Green NDVI", "(N - G) / (N + G)", "NDVI on green spectrum", -1, 1, true, "GN"});
    formulas_.push_back({"SAVI", "Soil Adjusted Vegetation Index", "(1.5 * (N - R)) / (N + R + 0.5)", "NDVI with soil correction", -1, 1, true, "RN", true});
    formulas_.push_back({"EVI", "Enhanced Vegetation Index", "2.5 * (N - R) / (N + 6*R - 7.5*B + 1)", "Better where NDVI saturates", -1, 1, true, "RBN", true});

    // Red edge
    formulas_.push_back({"NDRE", "Normalized Difference Red Edge", "(N - Re) / (N + Re)", "Green vegetation in mature crops", -1, 1, true, "NRe"});

    // More indices
    formulas_.push_back({"NDYI", "Normalized Difference Yellowness Index", "(G - B) / (G + B)", "Yellowness detection", -1, 1, true, "GB"});
    formulas_.push_back({"MPRI", "Modified Photochemical Reflectance Index", "(G - R) / (G + R)", "Photochemical reflectance", -1, 1, true, "RG"});
    formulas_.push_back({"OSAVI", "Optimized Soil Adjusted Vegetation Index", "(N - R) / (N + R + 0.16)", "Optimized for sparse vegetation", -1, 1, true, "RN", true});
    formulas_.push_back({"GRVI", "Green Ratio Vegetation Index", "N / G", "Green ratio", 0, 0, false, "GN"});
    formulas_.push_back({"ENDVI", "Enhanced NDVI", "((N + G) - (2*B)) / ((N + G) + (2*B))", "Enhanced NDVI", 0, 0, false, "GBN"});
    formulas_.push_back({"ARVI", "Atmospherically Resistant Vegetation Index", "(N - (2*R) + B) / (N + (2*R) + B)", "Corrects atmospheric effects", -1, 1, true, "RBN"});

    // Thermal formulas
    formulas_.push_back({"CELSIUS", "Celsius Temperature", "pixel value (passthrough)", "Temperature in degrees Celsius", -40, 150, true, "T"});
    formulas_.push_back({"KELVIN", "Kelvin Temperature", "pixel + 273.15", "Temperature in Kelvin", 233.15, 423.15, true, "T"});
}

std::vector<char> VegetationEngine::requiredSymbols(const VegetationFormula &formula) {
    std::vector<char> symbols;
    const std::string &required = formula.requiredBands;
    // Parse requiredBands token-wise so that "Re" is treated as a single RedEdge band.
    for (std::size_t i = 0; i < required.size(); ) {
        if (required[i] == 'R' && (i + 1) < required.size() && required[i + 1] == 'e') {
            symbols.push_back('e');
            i += 2;
        } else {
            symbols.push_back(required[i]);
            ++i;
        }
    }
    return symbols;
}

std::vector<int> VegetationEngine::bandsRead(const VegetationFormula &formula, const BandFilter &filter) {
    std::vector<int> bands;
    for (const char symbol : requiredSymbols(formula)) {
        const int band = filter.get(symbol);
        if (band >= 0) bands.push_back(band);
    }
    std::sort(bands.begin(), bands.end());
    bands.erase(std::unique(bands.begin(), bands.end()), bands.end());
    return bands;
}

std::vector<char> VegetationEngine::missingSymbols(const VegetationFormula &formula, const BandFilter &filter) {
    std::vector<char> missing;
    for (const char symbol : requiredSymbols(formula))
        if (!filter.has(symbol)) missing.push_back(symbol);
    return missing;
}

const char *VegetationEngine::symbolName(char symbol) {
    switch (symbol) {
        case 'R': return "Red";
        case 'G': return "Green";
        case 'B': return "Blue";
        case 'N': return "NIR";
        case 'e': return "Red edge";
        case 'T': return "Thermal";
        default: return "?";
    }
}

std::string VegetationEngine::dataRefusal(const VegetationFormula &formula, const RasterDataKind &data) {
    if (!formula.needsReflectance || data.floatingPoint) return "";
    return formula.id + " needs reflectance values (0 to 1, a floating-point raster): the raster is " +
           data.typeName + ", whose values are not reflectance";
}

std::string VegetationEngine::unavailableReason(const VegetationFormula &formula, const BandFilter &filter,
                                                const RasterDataKind &data) {
    if (!missingSymbols(formula, filter).empty()) return "Requires bands: " + formula.requiredBands;
    return dataRefusal(formula, data);
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
            str::toLower(nameLC);

            if (nameLC.find("red edge") != std::string::npos ||
                nameLC.find("rededge") != std::string::npos) {
                if (bf.Re < 0) bf.Re = bi.index - 1; // 0-based
            } else if (nameLC.starts_with("red")) {
                if (bf.R < 0) bf.R = bi.index - 1;
            } else if (nameLC.starts_with("green")) {
                if (bf.G < 0) bf.G = bi.index - 1;
            } else if (nameLC.starts_with("blue")) {
                if (bf.B < 0) bf.B = bi.index - 1;
            } else if (nameLC.starts_with("nir")) {
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
    if (auto dataset = raster::openInputRaster(rasterPath)) {
        const int nBands = dataset->GetRasterCount();
        dataset.reset();
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

namespace {

// A sample with no value to compute with: the nodata value of the computation, NaN or infinity
bool hasNoValue(float v, float nodata) {
    return v == nodata || !std::isfinite(v);
}

// The source bands of one pixel by symbol (0 for a band the filter does not name)
struct PixelBands {
    float R = 0, G = 0, B = 0, N = 0, Re = 0;
};

// The formulas this file evaluates; the id of a VegetationFormula is looked up once per call, not
// compared with every id for every pixel
enum class FormulaKind { Unknown, NDVI, VARI, EXG, GLI, vNDVI, NDWI, GNDVI, SAVI, EVI, NDRE, NDYI, MPRI, OSAVI, GRVI, ENDVI, ARVI, CELSIUS, KELVIN };

FormulaKind formulaKind(const std::string &id) {
    static const std::pair<const char *, FormulaKind> kinds[] = {
        {"NDVI", FormulaKind::NDVI},   {"VARI", FormulaKind::VARI},     {"EXG", FormulaKind::EXG},
        {"GLI", FormulaKind::GLI},     {"vNDVI", FormulaKind::vNDVI},   {"NDWI", FormulaKind::NDWI},
        {"GNDVI", FormulaKind::GNDVI}, {"SAVI", FormulaKind::SAVI},     {"EVI", FormulaKind::EVI},
        {"NDRE", FormulaKind::NDRE},   {"NDYI", FormulaKind::NDYI},     {"MPRI", FormulaKind::MPRI},
        {"OSAVI", FormulaKind::OSAVI}, {"GRVI", FormulaKind::GRVI},     {"ENDVI", FormulaKind::ENDVI},
        {"ARVI", FormulaKind::ARVI},   {"CELSIUS", FormulaKind::CELSIUS}, {"KELVIN", FormulaKind::KELVIN}};
    for (const auto &[name, kind] : kinds)
        if (id == name) return kind;
    return FormulaKind::Unknown;
}

// The value of the formula over pixel i (nodata where it is undefined there): p are its bands, and
// `thermal` is the first band of the raster (or null), which the thermal formulas read as it is
template <FormulaKind kind>
inline float evaluateFormula(const PixelBands &p, const float *thermal, size_t i, float nodata) {
    const float EPS = 1e-10f;
    const float R = p.R, G = p.G, B = p.B, N = p.N, Re = p.Re;
    // A ratio is undefined where its denominator is (nearly) zero
    const auto over = [nodata, EPS](float numerator, float denominator) {
        return (std::abs(denominator) < EPS) ? nodata : numerator / denominator;
    };

    switch (kind) {
    case FormulaKind::NDVI: return over(N - R, N + R);
    case FormulaKind::VARI: return over(G - R, G + R - B);
    case FormulaKind::EXG: return (2.0f * G) - (R + B);
    case FormulaKind::GLI: return over((G * 2.0f) - R - B, (G * 2.0f) + R + B);
    case FormulaKind::vNDVI:
        if (R > EPS && G > EPS && B > EPS)
            return 0.5268f * std::pow(R, -0.1294f) * std::pow(G, 0.3389f) * std::pow(B, -0.3118f);
        return nodata;
    case FormulaKind::NDWI: return over(G - N, G + N);
    case FormulaKind::GNDVI: return over(N - G, N + G);
    case FormulaKind::SAVI: return over(1.5f * (N - R), N + R + 0.5f);
    case FormulaKind::EVI: return over(2.5f * (N - R), N + 6.0f * R - 7.5f * B + 1.0f);
    case FormulaKind::NDRE: return over(N - Re, N + Re);
    case FormulaKind::NDYI: return over(G - B, G + B);
    case FormulaKind::MPRI: return over(G - R, G + R);
    case FormulaKind::OSAVI: return over(N - R, N + R + 0.16f);
    case FormulaKind::GRVI: return over(N, G);
    case FormulaKind::ENDVI: return over((N + G) - (2.0f * B), (N + G) + (2.0f * B));
    case FormulaKind::ARVI: return over(N - (2.0f * R) + B, N + (2.0f * R) + B);
    // Thermal passthrough: the pixel value is already the temperature in °C. For single-band
    // thermal rasters, band index 0 is used directly.
    case FormulaKind::CELSIUS:
        return thermal != nullptr && !hasNoValue(thermal[i], nodata) ? thermal[i] : nodata;
    // Celsius to Kelvin
    case FormulaKind::KELVIN:
        return thermal != nullptr && !hasNoValue(thermal[i], nodata) ? thermal[i] + 273.15f : nodata;
    case FormulaKind::Unknown: break;
    }
    return nodata;
}

// What a formula runs over: a pointer per source band (null for a band the filter does not name),
// the first band of the raster for the thermal formulas, and the output
struct FormulaInput {
    const float *R, *G, *B, *N, *Re, *thermal;
    float *out;
    size_t count;
    float nodata;
};

// The formula over every pixel. One instance per formula: the choice of the formula is made once, not
// per pixel.
template <FormulaKind kind>
void formulaLoop(const FormulaInput &in) {
    const float nodata = in.nodata;
    for (size_t i = 0; i < in.count; i++) {
        float R = in.R ? in.R[i] : 0;
        float G = in.G ? in.G[i] : 0;
        float B = in.B ? in.B[i] : 0;
        float N = in.N ? in.N[i] : 0;
        float Re = in.Re ? in.Re[i] : 0;

        // A pixel has no value when a source band used has none: nodata, or not a number (NaN,
        // infinite), which no formula turns into a value
        bool isNodata = false;
        if (in.R && hasNoValue(R, nodata)) isNodata = true;
        if (in.G && hasNoValue(G, nodata)) isNodata = true;
        if (in.B && hasNoValue(B, nodata)) isNodata = true;
        if (in.N && hasNoValue(N, nodata)) isNodata = true;
        if (in.Re && hasNoValue(Re, nodata)) isNodata = true;

        if (isNodata) {
            in.out[i] = nodata;
            continue;
        }

        in.out[i] = evaluateFormula<kind>(PixelBands{R, G, B, N, Re}, in.thermal, i, nodata);
    }
}

void runFormula(FormulaKind kind, const FormulaInput &in) {
    switch (kind) {
    case FormulaKind::NDVI: return formulaLoop<FormulaKind::NDVI>(in);
    case FormulaKind::VARI: return formulaLoop<FormulaKind::VARI>(in);
    case FormulaKind::EXG: return formulaLoop<FormulaKind::EXG>(in);
    case FormulaKind::GLI: return formulaLoop<FormulaKind::GLI>(in);
    case FormulaKind::vNDVI: return formulaLoop<FormulaKind::vNDVI>(in);
    case FormulaKind::NDWI: return formulaLoop<FormulaKind::NDWI>(in);
    case FormulaKind::GNDVI: return formulaLoop<FormulaKind::GNDVI>(in);
    case FormulaKind::SAVI: return formulaLoop<FormulaKind::SAVI>(in);
    case FormulaKind::EVI: return formulaLoop<FormulaKind::EVI>(in);
    case FormulaKind::NDRE: return formulaLoop<FormulaKind::NDRE>(in);
    case FormulaKind::NDYI: return formulaLoop<FormulaKind::NDYI>(in);
    case FormulaKind::MPRI: return formulaLoop<FormulaKind::MPRI>(in);
    case FormulaKind::OSAVI: return formulaLoop<FormulaKind::OSAVI>(in);
    case FormulaKind::GRVI: return formulaLoop<FormulaKind::GRVI>(in);
    case FormulaKind::ENDVI: return formulaLoop<FormulaKind::ENDVI>(in);
    case FormulaKind::ARVI: return formulaLoop<FormulaKind::ARVI>(in);
    case FormulaKind::CELSIUS: return formulaLoop<FormulaKind::CELSIUS>(in);
    case FormulaKind::KELVIN: return formulaLoop<FormulaKind::KELVIN>(in);
    case FormulaKind::Unknown: return formulaLoop<FormulaKind::Unknown>(in);
    }
}

} // anonymous namespace

void VegetationEngine::applyFormula(const VegetationFormula &formula,
                                     const BandFilter &filter,
                                     const std::vector<float*> &bandData,
                                     float *outData,
                                     size_t pixelCount,
                                     float nodata) const {
    // Get band data pointers by symbol
    // bandData indices match the raster band order (0-based)
    auto getBand = [&](char sym) -> const float* {
        int idx = filter.get(sym);
        if (idx < 0 || idx >= static_cast<int>(bandData.size())) return nullptr;
        return bandData[idx];
    };

    const FormulaInput input{getBand('R'), getBand('G'), getBand('B'), getBand('N'), getBand('e'),
                             (!bandData.empty() && bandData[0] != nullptr) ? bandData[0] : nullptr,
                             outData, pixelCount, nodata};
    runFormula(formulaKind(formula.id), input);
}

void VegetationEngine::applyColormap(const float *values,
                                      uint8_t *rgba,
                                      size_t pixelCount,
                                      const Colormap &cmap,
                                      const ColormapScale &scale) const {
    const float vmin = scale.vmin;
    const float nodata = scale.nodata;
    float range = scale.vmax - vmin;
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

const Colormap* VegetationEngine::getColormap(const std::string &id, bool includeUnlisted) const {
    for (const auto &c : colormaps_) {
        if (c.id == id && (c.listed || includeUnlisted)) return &c;
    }
    return nullptr;
}

json VegetationEngine::getColormapsJson() const {
    json arr = json::array();
    for (const auto &c : colormaps_) {
        if (!c.listed) continue;
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

namespace {

using GradientStops = std::vector<std::tuple<float, uint8_t, uint8_t, uint8_t>>;

// How a colormap is made: from gradient stops, or by a function for the ones that are not a gradient
struct ColormapDef {
    const char *id;
    const char *name;
    bool listed;                          // false: not in the list clients choose from
    GradientStops stops;
    void (*fill)(ColormapEntry *entries); // when set, makes the entries instead of the stops
};

// 5 steps of colors (Contrast NDVI)
void fillDiscreteNdvi(ColormapEntry *entries) {
    for (int i = 0; i < 256; i++) {
        float t = static_cast<float>(i) / 255.0f;
        if (t < 0.2f) { entries[i] = {139, 0, 0, 255}; }
        else if (t < 0.4f) { entries[i] = {255, 69, 0, 255}; }
        else if (t < 0.6f) { entries[i] = {255, 215, 0, 255}; }
        else if (t < 0.8f) { entries[i] = {50, 205, 50, 255}; }
        else { entries[i] = {0, 100, 0, 255}; }
    }
}

void fillGrayscale(ColormapEntry *entries) {
    for (int i = 0; i < 256; i++) {
        entries[i] = {static_cast<uint8_t>(i), static_cast<uint8_t>(i), static_cast<uint8_t>(i), 255};
    }
}

// Light to dark, matches the frontend preview swatches
void fillGreys(ColormapEntry *entries) {
    for (int i = 0; i < 256; i++) {
        uint8_t v = static_cast<uint8_t>(255 - i);
        entries[i] = {v, v, v, 255};
    }
}

// The colormaps, in the order they are listed
const std::vector<ColormapDef> &colormapDefs() {
    static const std::vector<ColormapDef> defs = {
        // RdYlGn (Red-Yellow-Green) - standard NDVI colormap
        {"rdylgn", "Red-Yellow-Green", true, {
            {0.0f, 215, 48, 39}, {0.25f, 253, 174, 97}, {0.5f, 254, 224, 139}, {0.75f, 166, 217, 106}, {1.0f, 26, 152, 80}
        }, nullptr},
        // Discrete NDVI (5 colors)
        {"discrete_ndvi", "Contrast NDVI", true, {}, fillDiscreteNdvi},
        // Spectral
        {"spectral", "Spectral", true, {
            {0.0f, 158, 1, 66}, {0.25f, 253, 174, 97}, {0.5f, 255, 255, 191}, {0.75f, 102, 194, 165}, {1.0f, 94, 79, 162}
        }, nullptr},
        // Viridis
        {"viridis", "Viridis", true, {
            {0.0f, 68, 1, 84}, {0.25f, 59, 82, 139}, {0.5f, 33, 144, 140}, {0.75f, 93, 201, 99}, {1.0f, 253, 231, 37}
        }, nullptr},
        // Plasma
        {"plasma", "Plasma", true, {
            {0.0f, 13, 8, 135}, {0.25f, 126, 3, 168}, {0.5f, 204, 71, 120}, {0.75f, 248, 149, 64}, {1.0f, 240, 249, 33}
        }, nullptr},
        // Inferno
        {"inferno", "Inferno", true, {
            {0.0f, 0, 0, 4}, {0.25f, 87, 16, 110}, {0.5f, 188, 55, 84}, {0.75f, 249, 142, 9}, {1.0f, 252, 255, 164}
        }, nullptr},
        // Magma
        {"magma", "Magma", true, {
            {0.0f, 0, 0, 4}, {0.25f, 81, 18, 124}, {0.5f, 183, 55, 121}, {0.75f, 252, 137, 97}, {1.0f, 252, 253, 191}
        }, nullptr},
        // Grayscale
        {"grayscale", "Grayscale", true, {}, fillGrayscale},
        // Ironbow (thermal)
        {"ironbow", "Ironbow", true, {
            {0.0f, 0, 0, 0}, {0.2f, 87, 15, 106}, {0.4f, 189, 55, 84}, {0.6f, 249, 120, 8}, {0.8f, 255, 227, 68}, {1.0f, 255, 255, 255}
        }, nullptr},
        // Rainbow
        {"rainbow", "Rainbow", true, {
            {0.0f, 150, 0, 90}, {0.17f, 0, 0, 200}, {0.33f, 0, 150, 255}, {0.5f, 0, 255, 0}, {0.67f, 255, 255, 0}, {0.83f, 255, 100, 0}, {1.0f, 200, 0, 0}
        }, nullptr},
        // BuGn (Blue-Green)
        {"bugn", "Blue-Green", true, {
            {0.0f, 247, 252, 253}, {0.25f, 178, 226, 226}, {0.5f, 102, 194, 164}, {0.75f, 35, 139, 69}, {1.0f, 0, 68, 27}
        }, nullptr},
        // Whitehot (thermal)
        {"whitehot", "White Hot", true, {
            {0.0f, 0, 0, 0}, {1.0f, 255, 255, 255}
        }, nullptr},
        // Blackhot (thermal, inverted)
        {"blackhot", "Black Hot", true, {
            {0.0f, 255, 255, 255}, {1.0f, 0, 0, 0}
        }, nullptr},
        // Arctic
        {"arctic", "Arctic", true, {
            {0.0f, 0, 0, 64}, {0.25f, 0, 100, 200}, {0.5f, 100, 200, 255}, {0.75f, 200, 240, 255}, {1.0f, 255, 255, 255}
        }, nullptr},
        // Lava
        {"lava", "Lava", true, {
            {0.0f, 0, 0, 0}, {0.25f, 128, 0, 0}, {0.5f, 255, 80, 0}, {0.75f, 255, 200, 0}, {1.0f, 255, 255, 200}
        }, nullptr},
        // Terrain (elevation: water -> low land -> hills -> mountains -> snow)
        {"terrain", "Terrain", true, {
            {0.0f, 51, 51, 153}, {0.2f, 0, 180, 180}, {0.4f, 160, 217, 121}, {0.6f, 236, 216, 122}, {0.8f, 169, 116, 67}, {1.0f, 255, 255, 255}
        }, nullptr},
        // Greys (alias of grayscale; maps frontend "greys" id to the standard ramp)
        {"greys", "Greys", true, {}, fillGreys},
        // Pure red-yellow-green: the ramp of the vegetation-index map layer (renderRasterIndex),
        // kept as it looked before that layer moved onto this engine. Not a colormap clients pick:
        // it is not listed (getColormapsJson) and a request cannot name it
        {"ryg", "Red-Yellow-Green (pure)", false, {
            {0.0f, 255, 0, 0}, {0.5f, 255, 255, 0}, {1.0f, 0, 255, 0}
        }, nullptr},
    };
    return defs;
}

} // anonymous namespace

void VegetationEngine::initColormaps() {
    for (const auto &def : colormapDefs()) {
        Colormap c;
        c.id = def.id;
        c.name = def.name;
        c.listed = def.listed;
        if (def.fill)
            def.fill(c.entries);
        else
            interpolateColormap(c.entries, def.stops);
        colormaps_.push_back(c);
    }
}

} // namespace ddb
