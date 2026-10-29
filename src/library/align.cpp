/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "align.h"
#include "mathutil.h"
#include "artefact_cache.h"
#include "raster_artefacts.h"
#include "gdal_raii.h"
#include "gtiff_options.h"
#include "raster_utils.h"
#include "cog.h"        // buildCog() - DRY reuse of the COG pattern
#include "exceptions.h"
#include "logger.h"
#include "json.h"
#include "fs.h"          // fs::absolute - used by applyWarp() for VRT path resolution
#include "mio.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <random>
#include <utility>
#include <vector>

namespace ddb {

// ─── A  Internal types ──────────────────────────────────────────────────────

struct Vec2 { double x = 0, y = 0; };
struct TiePoint { Vec2 src; Vec2 ref; };

/** 2D similarity transform: X_ref = scale·R·X_src + t */
struct Similarity2D {
    double scale     = 1.0;
    double cosTheta  = 1.0;
    double sinTheta  = 0.0;
    Vec2   t;

    Vec2 apply(Vec2 p) const noexcept {
        return { scale * (cosTheta * p.x - sinTheta * p.y) + t.x,
                 scale * (sinTheta * p.x + cosTheta * p.y) + t.y };
    }
    double residual(const TiePoint &tp) const noexcept {
        Vec2 q = apply(tp.src);
        double dx = q.x - tp.ref.x, dy = q.y - tp.ref.y;
        return std::sqrt(dx * dx + dy * dy);
    }
};

// The extent of a raster in map units, normalized so that x0 < x1 and y0 < y1 regardless of the
// sign of the pixel sizes (west-up and south-up geotransforms have negative ones)
struct Extent {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;

    static Extent of(GDALDatasetH hDs, const double gt[6]) {
        Extent e;
        e.x0 = gt[0];
        e.x1 = gt[0] + gt[1] * GDALGetRasterXSize(hDs);
        e.y0 = gt[3] + gt[5] * GDALGetRasterYSize(hDs);
        e.y1 = gt[3];
        if (e.x1 < e.x0) std::swap(e.x0, e.x1);
        if (e.y1 < e.y0) std::swap(e.y0, e.y1);
        return e;
    }

    // The area both extents cover (empty or inverted when they do not meet)
    Extent overlap(const Extent &o) const {
        return {std::max(x0, o.x0), std::max(y0, o.y0), std::min(x1, o.x1), std::min(y1, o.y1)};
    }

    // How much of this extent the other one covers, in percent
    double overlapPercent(const Extent &o) const {
        const double ox = std::max(0.0, std::min(x1, o.x1) - std::max(x0, o.x0));
        const double oy = std::max(0.0, std::min(y1, o.y1) - std::max(y0, o.y0));
        const double area = (x1 - x0) * (y1 - y0);
        return (area > 0) ? 100.0 * (ox * oy) / area : 0.0;
    }
};

// ─── B  Raster to common float32 grid ───────────────────────────────────────

static bool detectIsDem(GDALDatasetH hDs) {
    int n = GDALGetRasterCount(hDs);
    if (n != 1) return false;
    GDALDataType dt = GDALGetRasterDataType(GDALGetRasterBand(hDs, 1));
    return dt == GDT_Float32 || dt == GDT_Float64 ||
           dt == GDT_Int16   || dt == GDT_Int32;
}

struct RasterGrid {
    std::vector<float> data;
    int    width = 0, height = 0;
    double gt[6] = {};      // GeoTransform of the grid (corner + pixel size)
};

/**
 * Re-reads hDs in the bbox [x0,y0]→[x1,y1] (in the dataset CRS) at the target GSD.
 * Produces a scalar float32 signal:
 *   ortho → luminance (BT.601 coefficients)
 *   DEM   → standardized elevation: (v − μ) / σ
 */
static RasterGrid readToCommonGrid(GDALDatasetH hDs,
                                   const std::string &name,
                                   const Extent &area,
                                   double targetGsd,
                                   bool isDem)
{
    const double x0 = area.x0, y0 = area.y0, x1 = area.x1, y1 = area.y1;
    double gt[6];
    requireGeoTransform(hDs, gt, name);

    // Source window in pixels
    int px0 = static_cast<int>(std::floor((x0 - gt[0]) / gt[1]));
    int py0 = static_cast<int>(std::floor((y1 - gt[3]) / gt[5]));  // gt[5] < 0
    int px1 = static_cast<int>(std::ceil ((x1 - gt[0]) / gt[1]));
    int py1 = static_cast<int>(std::ceil ((y0 - gt[3]) / gt[5]));
    px0 = std::clamp(px0, 0, GDALGetRasterXSize(hDs));
    py0 = std::clamp(py0, 0, GDALGetRasterYSize(hDs));
    px1 = std::clamp(px1, 0, GDALGetRasterXSize(hDs));
    py1 = std::clamp(py1, 0, GDALGetRasterYSize(hDs));
    if (px1 <= px0 || py1 <= py0)
        throw AppException("Empty source window when building common grid");

    double regionW = x1 - x0, regionH = y1 - y0;
    int outW = std::max(1, static_cast<int>(std::round(regionW / targetGsd)));
    int outH = std::max(1, static_cast<int>(std::round(regionH / targetGsd)));

    int nbands = GDALGetRasterCount(hDs);
    std::vector<std::vector<float>> bands(nbands, std::vector<float>(static_cast<size_t>(outW) * outH, 0.f));
    for (int b = 1; b <= nbands; b++) {
        GDALRasterBandH hBand = GDALGetRasterBand(hDs, b);
        if (!hBand) throw GDALException("Cannot get band " + std::to_string(b));
        if (GDALRasterIO(hBand, GF_Read,
                         px0, py0, px1 - px0, py1 - py0,
                         bands[b - 1].data(), outW, outH,
                         GDT_Float32, 0, 0) != CE_None)
            throw GDALException("GDALRasterIO failed for band " + std::to_string(b));
    }

    std::vector<float> signal(static_cast<size_t>(outW) * outH);
    if (isDem) {
        double sum = 0, sum2 = 0;
        for (float v : bands[0]) { sum += v; sum2 += v * static_cast<double>(v); }
        double count = static_cast<double>(outW) * outH;
        double mean = sum / count;
        double stdv = std::sqrt(std::max(0.0, sum2 / count - mean * mean));
        if (stdv < 1e-9) stdv = 1.0;
        for (size_t i = 0; i < signal.size(); i++)
            signal[i] = static_cast<float>((bands[0][i] - mean) / stdv);
    } else {
        // BT.601 luminance
        for (size_t i = 0; i < signal.size(); i++) {
            float r = nbands >= 1 ? bands[0][i] : 0.f;
            float g = nbands >= 2 ? bands[1][i] : r;
            float b = nbands >= 3 ? bands[2][i] : r;
            signal[i] = 0.299f * r + 0.587f * g + 0.114f * b;
        }
    }

    RasterGrid grid;
    grid.data   = std::move(signal);
    grid.width  = outW;
    grid.height = outH;
    // GeoTransform of the common grid (north-up, pixel size = targetGsd)
    grid.gt[0] = x0;   grid.gt[1] = targetGsd; grid.gt[2] = 0;
    grid.gt[3] = y1;   grid.gt[4] = 0;          grid.gt[5] = -targetGsd;
    return grid;
}

// ─── C  Integral images (SAT) - O(1) NCC ────────────────────────────────────

struct IntegralImages {
    std::vector<double> sat, sat2;
    int W = 0, H = 0;

    IntegralImages() = default;
    IntegralImages(const std::vector<float> &img, int w, int h)
        : sat(static_cast<size_t>(w + 1) * (h + 1), 0.0), sat2(static_cast<size_t>(w + 1) * (h + 1), 0.0), W(w), H(h)
    {
        for (int r = 0; r < h; r++) {
            for (int c = 0; c < w; c++) {
                double v = img[static_cast<size_t>(r) * w + c];
                size_t idx = static_cast<size_t>(r + 1) * (w + 1) + (c + 1);
                sat [idx] = v       + sat [static_cast<size_t>(r) * (w + 1) + (c + 1)] + sat [static_cast<size_t>(r + 1) * (w + 1) + c] - sat [static_cast<size_t>(r) * (w + 1) + c];
                sat2[idx] = v * v   + sat2[static_cast<size_t>(r) * (w + 1) + (c + 1)] + sat2[static_cast<size_t>(r + 1) * (w + 1) + c] - sat2[static_cast<size_t>(r) * (w + 1) + c];
            }
        }
    }

    double boxSum (int r0, int c0, int r1, int c1) const noexcept {
        return sat [static_cast<size_t>(r1 + 1) * (W + 1) + (c1 + 1)] - sat [static_cast<size_t>(r0) * (W + 1) + (c1 + 1)]
             - sat [static_cast<size_t>(r1 + 1) * (W + 1) + c0]       + sat [static_cast<size_t>(r0) * (W + 1) + c0];
    }
    double boxSum2(int r0, int c0, int r1, int c1) const noexcept {
        return sat2[static_cast<size_t>(r1 + 1) * (W + 1) + (c1 + 1)] - sat2[static_cast<size_t>(r0) * (W + 1) + (c1 + 1)]
             - sat2[static_cast<size_t>(r1 + 1) * (W + 1) + c0]       + sat2[static_cast<size_t>(r0) * (W + 1) + c0];
    }
};

// ─── D  Patch selection by texture (local variance) ─────────────────────────

struct PatchCandidate {
    int   row = 0, col = 0;   // top-left pixel in the source grid
    float textureScore = 0.f;
};

static std::vector<PatchCandidate> selectPatches(
    const RasterGrid    &srcGrid,
    const IntegralImages &ii,
    int patchSize, int maxPatches)
{
    std::vector<PatchCandidate> candidates;
    int step = std::max(1, patchSize / 2);
    int W = srcGrid.width, H = srcGrid.height;
    int margin = patchSize;

    for (int r = margin; r + patchSize + margin < H; r += step) {
        for (int c = margin; c + patchSize + margin < W; c += step) {
            int n = patchSize * patchSize;
            double s  = ii.boxSum (r, c, r + patchSize - 1, c + patchSize - 1);
            double s2 = ii.boxSum2(r, c, r + patchSize - 1, c + patchSize - 1);
            double var = s2 / n - (s / n) * (s / n);
            candidates.push_back({r, c, static_cast<float>(var)});
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const PatchCandidate &a, const PatchCandidate &b) {
                  return a.textureScore > b.textureScore; });
    if (static_cast<int>(candidates.size()) > maxPatches)
        candidates.resize(static_cast<size_t>(maxPatches));
    return candidates;
}

// ─── E  2D phase correlation (self-contained complex FFT) ────────────────────
//     Coarse seed / Translation mode - no external FFT dependency.

/** In-place iterative radix-2 Cooley-Tukey FFT. a.size() must be a power of 2. */
static void fft1d(std::vector<std::complex<double>> &a, bool inverse) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        double ang = 2.0 * ddb::kPi / static_cast<double>(len) * (inverse ? 1.0 : -1.0);
        std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k) {
                std::complex<double> u = a[i + k];
                std::complex<double> v = a[i + k + len / 2] * w;
                a[i + k]           = u + v;
                a[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
    if (inverse)
        for (auto &x : a) x /= static_cast<double>(n);
}

/** In-place 2D FFT of an n×n row-major complex matrix (rows then columns). */
static void fft2d(std::vector<std::complex<double>> &m, int n, bool inverse) {
    std::vector<std::complex<double>> line(static_cast<size_t>(n));
    for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ++c) line[c] = m[static_cast<size_t>(r) * n + c];
        fft1d(line, inverse);
        for (int c = 0; c < n; ++c) m[static_cast<size_t>(r) * n + c] = line[c];
    }
    for (int c = 0; c < n; ++c) {
        for (int r = 0; r < n; ++r) line[r] = m[static_cast<size_t>(r) * n + c];
        fft1d(line, inverse);
        for (int r = 0; r < n; ++r) m[static_cast<size_t>(r) * n + c] = line[r];
    }
}

/**
 * Phase correlation between src and ref.
 * @return (dc, dr): the displacement, in common-grid pixels, that must be ADDED
 *         to a source position to reach the matching reference position
 *         (same convention as the per-patch NCC match below).
 */
static std::pair<double, double> phaseCorrelate(const RasterGrid &src,
                                                const RasterGrid &ref,
                                                double *peakStrength = nullptr)
{
    int nfft = 1;
    while (nfft < std::max({src.width, src.height, ref.width, ref.height}))
        nfft <<= 1;
    const size_t N = static_cast<size_t>(nfft) * nfft;

    auto meanOf = [](const std::vector<float> &v) -> double {
        if (v.empty()) return 0.0;
        double s = 0;
        for (float x : v) s += x;
        return s / static_cast<double>(v.size());
    };
    const double mS = meanOf(src.data);
    const double mR = meanOf(ref.data);

    // Zero-padded, mean-subtracted inputs (mean removal suppresses the DC peak)
    std::vector<std::complex<double>> FS(N, {0.0, 0.0}), FR(N, {0.0, 0.0});
    for (int r = 0; r < src.height; ++r)
        for (int c = 0; c < src.width; ++c)
            FS[static_cast<size_t>(r) * nfft + c] = std::complex<double>(src.data[static_cast<size_t>(r) * src.width + c] - mS, 0.0);
    for (int r = 0; r < ref.height; ++r)
        for (int c = 0; c < ref.width; ++c)
            FR[static_cast<size_t>(r) * nfft + c] = std::complex<double>(ref.data[static_cast<size_t>(r) * ref.width + c] - mR, 0.0);

    fft2d(FS, nfft, false);
    fft2d(FR, nfft, false);

    // Normalized cross-power spectrum: conj(FS) ⊙ FR / |conj(FS) ⊙ FR|
    std::vector<std::complex<double>> cp(N);
    for (size_t i = 0; i < N; ++i) {
        std::complex<double> v = std::conj(FS[i]) * FR[i];
        double mag = std::abs(v);
        cp[i] = (mag < 1e-12) ? std::complex<double>(0.0, 0.0) : v / mag;
    }

    fft2d(cp, nfft, true);   // full complex inverse → single sharp peak

    size_t peakIdx = 0;
    double peakVal = cp[0].real();
    for (size_t i = 1; i < N; ++i)
        if (cp[i].real() > peakVal) { peakVal = cp[i].real(); peakIdx = i; }

    int pr = static_cast<int>(peakIdx) / nfft;
    int pc = static_cast<int>(peakIdx) % nfft;

    // Peak-to-sidelobe strength: how sharply the correlation surface peaks
    // above its mean background (excluding a small neighborhood around the
    // peak). A sharp, isolated peak => confident localization; a flat surface
    // (e.g. textureless input) => low strength. Used to derive a [0,1]
    // confidence for translation-only alignment.
    if (peakStrength) {
        double sum = 0.0; size_t cnt = 0;
        const int exclude = 2;
        for (int r = 0; r < nfft; r++) {
            for (int c = 0; c < nfft; c++) {
                int ddr = std::abs(r - pr); ddr = std::min(ddr, nfft - ddr);
                int ddc = std::abs(c - pc); ddc = std::min(ddc, nfft - ddc);
                if (ddr <= exclude && ddc <= exclude) continue;
                sum += std::abs(cp[static_cast<size_t>(r) * nfft + c].real());
                cnt++;
            }
        }
        const double meanSidelobe = cnt > 0 ? sum / cnt : 0.0;
        *peakStrength = (meanSidelobe > 1e-12) ? (peakVal / meanSidelobe) : 0.0;
    }

    // Circular wrap-around: peaks past nfft/2 represent negative displacements
    double dr = pr < nfft / 2 ? pr : pr - nfft;
    double dc = pc < nfft / 2 ? pc : pc - nfft;
    return {dc, dr};
}

// ─── F  Per-patch NCC ───────────────────────────────────────────────────────

struct NccMatch {
    double dr    = 0, dc = 0;  // sub-pixel displacement (common-grid pixels)
    float  score = -1.f;       // NCC peak ∈ [-1, 1]
};

/** The patch of the source to look for: its top-left pixel and size, and how far to search. */
struct PatchQuery {
    int srcRow = 0, srcCol = 0;
    int patchSize = 0, searchRadius = 0;
};

/**
 * Searches the template srcGrid[srcRow,srcCol : srcRow+patchSize, ...]
 * in the refGrid window centered on (srcRow+seedDr, srcCol+seedDc) ± searchRadius.
 */
static NccMatch matchPatch(const RasterGrid    &srcGrid,
                           const IntegralImages &refII,
                           const RasterGrid    &refGrid,
                           const PatchQuery    &query,
                           double seedDr, double seedDc)
{
    const int srcRow = query.srcRow, srcCol = query.srcCol;
    const int patchSize = query.patchSize, searchRadius = query.searchRadius;
    int n = patchSize * patchSize;

    // Extract source patch and compute μ, σ
    std::vector<float> patch(n);
    double pSum = 0, pSum2 = 0;
    for (int r = 0; r < patchSize; r++)
        for (int c = 0; c < patchSize; c++) {
            float v = srcGrid.data[static_cast<size_t>(srcRow + r) * srcGrid.width + (srcCol + c)];
            patch[static_cast<size_t>(r) * patchSize + c] = v;
            pSum += v; pSum2 += v * static_cast<double>(v);
        }
    double pMean = pSum / n;
    double pStd  = std::sqrt(std::max(0.0, pSum2 / n - pMean * pMean));
    if (pStd < 1e-6) return {};  // uniform patch → skip

    // Search window in the reference
    int rr0 = static_cast<int>(std::round(srcRow + seedDr)) - searchRadius;
    int rc0 = static_cast<int>(std::round(srcCol + seedDc)) - searchRadius;
    int rr1 = rr0 + 2 * searchRadius;
    int rc1 = rc0 + 2 * searchRadius;
    rr0 = std::clamp(rr0, 0, refGrid.height - patchSize);
    rc0 = std::clamp(rc0, 0, refGrid.width  - patchSize);
    rr1 = std::clamp(rr1, 0, refGrid.height - patchSize);
    rc1 = std::clamp(rc1, 0, refGrid.width  - patchSize);

    int mapRows = rr1 - rr0 + 1, mapCols = rc1 - rc0 + 1;
    if (mapRows <= 0 || mapCols <= 0) return {};

    NccMatch best;
    std::vector<float> nccMap(static_cast<size_t>(mapRows) * mapCols, -2.f);

    for (int rr = rr0; rr <= rr1; rr++) {
        for (int rc = rc0; rc <= rc1; rc++) {
            double refS  = refII.boxSum (rr, rc, rr + patchSize - 1, rc + patchSize - 1);
            double refS2 = refII.boxSum2(rr, rc, rr + patchSize - 1, rc + patchSize - 1);
            double refMean = refS / n;
            double refStd  = std::sqrt(std::max(0.0, refS2 / n - refMean * refMean));
            if (refStd < 1e-6) { continue; }

            // Cross sum
            double cross = 0;
            for (int r = 0; r < patchSize; r++)
                for (int c = 0; c < patchSize; c++)
                    cross += (patch[static_cast<size_t>(r) * patchSize + c] - pMean) *
                             (refGrid.data[static_cast<size_t>(rr + r) * refGrid.width + (rc + c)] - refMean);

            float ncc = static_cast<float>(cross / (n * pStd * refStd));
            nccMap[static_cast<size_t>(rr - rr0) * mapCols + (rc - rc0)] = ncc;
            if (ncc > best.score) {
                best.score = ncc;
                best.dr = rr - srcRow;
                best.dc = rc - srcCol;
            }
        }
    }

    // Sub-pixel: 3×3 parabolic fit on the peak
    int pr = static_cast<int>(best.dr + srcRow) - rr0;
    int pc = static_cast<int>(best.dc + srcCol) - rc0;
    if (pr > 0 && pr < mapRows - 1 && pc > 0 && pc < mapCols - 1) {
        float ym1 = nccMap[static_cast<size_t>(pr - 1) * mapCols + pc], yp1 = nccMap[static_cast<size_t>(pr + 1) * mapCols + pc];
        float xm1 = nccMap[static_cast<size_t>(pr) * mapCols + (pc - 1)], xp1 = nccMap[static_cast<size_t>(pr) * mapCols + (pc + 1)];
        float c0  = nccMap[static_cast<size_t>(pr) * mapCols + pc];
        float denR = 2 * c0 - ym1 - yp1, denC = 2 * c0 - xm1 - xp1;
        if (denR > 1e-9f) best.dr += 0.5f * (ym1 - yp1) / denR;
        if (denC > 1e-9f) best.dc += 0.5f * (xm1 - xp1) / denC;
    }
    return best;
}

// ─── G  Umeyama / Procrustes similarity estimator (direct 2D closed form) ────

/**
 * Least-squares 2D similarity (scale, rotation, translation) mapping src→ref.
 * Uses the direct 2D Procrustes solution rather than a general 2×2 SVD: the
 * latter is numerically degenerate for the translation-dominated case (a
 * near-isotropic symmetric cross-covariance), where it injects a spurious
 * rotation. The direct form returns R=I exactly for pure translation.
 * Requires ≥ 2 points.
 */
static Similarity2D umeyama(const std::vector<TiePoint> &pts) {
    int N = static_cast<int>(pts.size());
    if (N < 2) throw AppException("Umeyama requires at least 2 points");

    Vec2 muP{}, muQ{};
    for (auto &tp : pts) {
        muP.x += tp.src.x; muP.y += tp.src.y;
        muQ.x += tp.ref.x; muQ.y += tp.ref.y;
    }
    muP.x /= N; muP.y /= N; muQ.x /= N; muQ.y /= N;

    double sigma2P = 0;
    double s00 = 0, s01 = 0, s10 = 0, s11 = 0;
    for (auto &tp : pts) {
        double px = tp.src.x - muP.x, py = tp.src.y - muP.y;
        double qx = tp.ref.x - muQ.x, qy = tp.ref.y - muQ.y;
        sigma2P += px * px + py * py;
        s00 += qx * px; s01 += qx * py;
        s10 += qy * px; s11 += qy * py;
    }

    // Optimal rotation:  θ = atan2(Σ(qy·px − qx·py), Σ(qx·px + qy·py))
    double a = s00 + s11;          // Σ(qx·px + qy·py)
    double b = s10 - s01;          // Σ(qy·px − qx·py)
    double theta = std::atan2(b, a);
    double denom = std::sqrt(a * a + b * b);
    double scale = (sigma2P > 1e-20) ? denom / sigma2P : 1.0;

    Similarity2D sim;
    sim.scale    = scale;
    sim.cosTheta = std::cos(theta);
    sim.sinTheta = std::sin(theta);
    sim.t.x = muQ.x - scale * (sim.cosTheta * muP.x - sim.sinTheta * muP.y);
    sim.t.y = muQ.y - scale * (sim.sinTheta * muP.x + sim.cosTheta * muP.y);
    return sim;
}

// ─── H  RANSAC ───────────────────────────────────────────────────────────────

static Similarity2D ransac(const std::vector<TiePoint> &pts,
                           double threshold, int maxIter,
                           std::vector<bool> &inlierMask)
{
    int N = static_cast<int>(pts.size());
    inlierMask.assign(N, false);
    if (N < 2) return {};

    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, N - 1);
    Similarity2D best;
    int bestInliers = 0;

    for (int it = 0; it < maxIter; it++) {
        int i = dist(rng), j;
        do { j = dist(rng); } while (j == i);

        try {
            auto sim = umeyama({pts[i], pts[j]});
            int cnt = 0;
            for (auto &tp : pts) if (sim.residual(tp) < threshold) cnt++;
            if (cnt > bestInliers) { bestInliers = cnt; best = sim; }
        } catch (...) {}
    }

    // Refit Umeyama on all inliers
    std::vector<TiePoint> inliers;
    for (int i = 0; i < N; i++) {
        if (best.residual(pts[i]) < threshold) {
            inliers.push_back(pts[i]);
            inlierMask[i] = true;
        }
    }
    if (inliers.size() >= 2) best = umeyama(inliers);
    return best;
}

// ─── I  Geotransform composition + GDAL warp ─────────────────────────────────

static void applyWarp(const std::string &sourcePath,
                      const std::string &tmpPath,
                      const Similarity2D &sim,
                      const std::string &targetCrs)
{
    // Resolve to an absolute path so the VRT SimpleSource can find the file
    // regardless of any cwd change between the VRT creation and the warp read.
    const std::string absSrcPath = io::toUtf8(fs::absolute(io::fromUtf8(sourcePath)));
    const auto src = raster::openInputRaster(absSrcPath);
    if (!src) throw GDALException("Cannot open " + sourcePath);
    GDALDatasetH hSrc = datasetHandle(src);

    double gt[6];
    requireGeoTransform(hSrc, gt, sourcePath);

    double co = sim.cosTheta, si = sim.sinTheta, sc = sim.scale;
    double ngt[6];
    ngt[0] = sc * (co * gt[0] - si * gt[3]) + sim.t.x;
    ngt[1] = sc * (co * gt[1] - si * gt[4]);
    ngt[2] = sc * (co * gt[2] - si * gt[5]);
    ngt[3] = sc * (si * gt[0] + co * gt[3]) + sim.t.y;
    ngt[4] = sc * (si * gt[1] + co * gt[4]);
    ngt[5] = sc * (si * gt[2] + co * gt[5]);

    // Wrap the source in a lightweight VRT (metadata-only) carrying the new
    // GeoTransform. This avoids copying the entire raster pixel data into a
    // MEM dataset (which peaks at O(W·H·B) RAM), letting GDALWarp read source
    // pixels on-demand block-by-block (O(block) memory, independent of raster
    // size). The VRT driver stores the source path as a SimpleSource reference;
    // hSrc is kept open until after GDALWarp so the file remains accessible.
    GDALDriverH hVrtDrv = GDALGetDriverByName("VRT");
    if (!hVrtDrv) throw GDALException("VRT driver unavailable");
    // Declared after src: closed before it
    auto vrt = adoptDataset(GDALCreateCopy(hVrtDrv, "", hSrc, FALSE,
                                           nullptr, nullptr, nullptr));
    if (!vrt) throw GDALException("Cannot create VRT wrapper for warp");
    GDALDatasetH hVrt = datasetHandle(vrt);
    if (GDALSetGeoTransform(hVrt, ngt) != CE_None || GDALSetProjection(hVrt, targetCrs.c_str()) != CE_None)
        throw GDALException("Cannot georeference the VRT wrapper for warp");

    // GDALWarp → temporary LZW GTiff
    CPLStringList args;
    args.AddString("-of");    args.AddString("GTiff");
    args.AddString("-t_srs"); args.AddString(targetCrs.c_str());
    args.AddString("-r");     args.AddString("bilinear");
    GtiffOptions().compress(GtiffOptions::Codec::Lzw).bigTiffIfSafer().appendTo(args);

    const GdalOptionsPtr<GDALWarpAppOptions> wOpts(GDALWarpAppOptionsNew(args.List(), nullptr));
    auto out = adoptDataset(
        GDALWarp(tmpPath.c_str(), nullptr, 1, &hVrt, wOpts.get(), nullptr));
    if (!out) throw GDALException("GDALWarp failed for aligned output: " + tmpPath);
    closeOutputOrThrow(std::move(out), tmpPath);
}

// ─── J  DEM Z offset ────────────────────────────────────────────────────────

static double computeZOffset(const std::string &alignedPath,
                             const std::string &referencePath)
{
    auto alignedDs = raster::openArtefactRaster(io::fromUtf8(alignedPath));
    auto refDs     = raster::openInputRaster(referencePath);
    if (!alignedDs || !refDs) return 0.0;
    GDALDatasetH hA = datasetHandle(alignedDs);
    GDALDatasetH hR = datasetHandle(refDs);

    // Both rasters share the source CRS at this point (the reference is
    // reprojected upstream when needed). Sample the *geographic* overlap on a
    // shared world-aligned grid so that the compared elevation samples are
    // co-located: comparing the top-left 512x512 pixel block of each raster by
    // index would mix non-corresponding ground points whenever the origins or
    // extents differ, biasing the median offset.
    double gtA[6], gtR[6];
    if (GDALGetGeoTransform(hA, gtA) != CE_None || GDALGetGeoTransform(hR, gtR) != CE_None) {
        return 0.0;
    }

    const Extent overlap = Extent::of(hA, gtA).overlap(Extent::of(hR, gtR));
    const double ox0 = overlap.x0, ox1 = overlap.x1;
    const double oy0 = overlap.y0, oy1 = overlap.y1;
    if (ox1 <= ox0 || oy1 <= oy0) return 0.0;

    double gsd = std::max(std::abs(gtA[1]), std::abs(gtR[1]));
    if (gsd <= 0) gsd = std::max(ox1 - ox0, oy1 - oy0) / 512.0;
    const int outW = std::clamp(static_cast<int>(std::round((ox1 - ox0) / gsd)), 1, 512);
    const int outH = std::clamp(static_cast<int>(std::round((oy1 - oy0) / gsd)), 1, 512);

    // Reads the [ox0,oy0]->[ox1,oy1] world window from hDs, resampled to the
    // shared outW x outH grid, so both buffers index the same ground points.
    auto readOverlap = [&](GDALDatasetH hDs, const double gt[6],
                           std::vector<float> &out) -> bool {
        int px0 = static_cast<int>(std::floor((ox0 - gt[0]) / gt[1]));
        int px1 = static_cast<int>(std::ceil ((ox1 - gt[0]) / gt[1]));
        int py0 = static_cast<int>(std::floor((oy1 - gt[3]) / gt[5]));  // gt[5] < 0
        int py1 = static_cast<int>(std::ceil ((oy0 - gt[3]) / gt[5]));
        px0 = std::clamp(px0, 0, GDALGetRasterXSize(hDs));
        px1 = std::clamp(px1, 0, GDALGetRasterXSize(hDs));
        py0 = std::clamp(py0, 0, GDALGetRasterYSize(hDs));
        py1 = std::clamp(py1, 0, GDALGetRasterYSize(hDs));
        if (px1 <= px0 || py1 <= py0) return false;
        out.assign(static_cast<size_t>(outW) * outH, 0.f);
        return GDALRasterIO(GDALGetRasterBand(hDs, 1), GF_Read,
                            px0, py0, px1 - px0, py1 - py0,
                            out.data(), outW, outH, GDT_Float32, 0, 0) == CE_None;
    };

    std::vector<float> bufA, bufR;
    const bool readOk = readOverlap(hA, gtA, bufA) && readOverlap(hR, gtR, bufR);
    const BandNodata ndA = readBandNodata(GDALGetRasterBand(hA, 1));
    const BandNodata ndR = readBandNodata(GDALGetRasterBand(hR, 1));
    alignedDs.reset(); refDs.reset();
    if (!readOk) return 0.0;

    std::vector<float> diffs;
    diffs.reserve(bufA.size());
    for (size_t i = 0; i < bufA.size(); i++) {
        if (std::isnan(bufA[i]) || std::isnan(bufR[i])) continue;
        if (isNodata(bufA[i], ndA) || isNodata(bufR[i], ndR)) continue;
        diffs.push_back(bufR[i] - bufA[i]);
    }
    if (diffs.empty()) return 0.0;
    std::sort(diffs.begin(), diffs.end());
    return diffs[diffs.size() / 2];
}

void applyZOffset(const std::string &path, double zOffset) {
    if (std::abs(zOffset) < 1e-6) return;
    const auto dataset = raster::openArtefactRaster(io::fromUtf8(path), raster::ArtefactAccess::Update);
    if (!dataset) throw GDALException("Cannot open " + path + " to apply the vertical offset");
    GDALDatasetH hDs = datasetHandle(dataset);
    GDALRasterBandH hBand = GDALGetRasterBand(hDs, 1);
    int W = GDALGetRasterXSize(hDs), H = GDALGetRasterYSize(hDs);
    const BandNodata nd = readBandNodata(hBand);
    std::vector<float> row(W);
    for (int r = 0; r < H; r++) {
        if (GDALRasterIO(hBand, GF_Read, 0, r, W, 1, row.data(), W, 1, GDT_Float32, 0, 0) != CE_None)
            throw GDALException("Cannot read row " + std::to_string(r) + " of " + path);
        for (float &v : row)
            if (!isNodata(v, nd))
                v += static_cast<float>(zOffset);
        if (GDALRasterIO(hBand, GF_Write, 0, r, W, 1, row.data(), W, 1, GDT_Float32, 0, 0) != CE_None)
            throw GDALException("Cannot write row " + std::to_string(r) + " of " + path);
    }
}

// ─── K  Public API ──────────────────────────────────────────────────────────

AlignValidationResult validateAlignRaster(const std::string &sourcePath,
                                          const std::string &referencePath)
{
    AlignValidationResult r;

    const auto srcDs = raster::openInputRaster(sourcePath);
    const auto refDs = raster::openInputRaster(referencePath);
    if (!srcDs) { r.errors.push_back("Cannot open source: " + sourcePath); return r; }
    if (!refDs) { r.errors.push_back("Cannot open reference: " + referencePath); return r; }
    GDALDatasetH hS = datasetHandle(srcDs);
    GDALDatasetH hR = datasetHandle(refDs);

    r.summary.sourceCrs    = GDALGetProjectionRef(hS) ? GDALGetProjectionRef(hS) : "";
    r.summary.referenceCrs = GDALGetProjectionRef(hR) ? GDALGetProjectionRef(hR) : "";
    if (r.summary.sourceCrs.empty())    r.errors.push_back("Source has no CRS");
    if (r.summary.referenceCrs.empty()) r.errors.push_back("Reference has no CRS");

    r.summary.crsMismatch = (r.summary.sourceCrs != r.summary.referenceCrs);
    if (r.summary.crsMismatch)
        r.warnings.push_back("CRS mismatch: reference will be reprojected to source CRS before alignment");

    r.summary.sourceType    = detectIsDem(hS) ? "dem" : "ortho";
    r.summary.referenceType = detectIsDem(hR) ? "dem" : "ortho";
    if (r.summary.sourceType != r.summary.referenceType)
        r.warnings.push_back("Type mismatch (source=" + r.summary.sourceType +
                             ", reference=" + r.summary.referenceType + "): alignment may be inaccurate");

    double gts[6], gtr[6];
    const bool sourceHasGeoTransform = GDALGetGeoTransform(hS, gts) == CE_None;
    const bool referenceHasGeoTransform = GDALGetGeoTransform(hR, gtr) == CE_None;
    if (!sourceHasGeoTransform) r.errors.push_back("Source has no geotransform");
    if (!referenceHasGeoTransform) r.errors.push_back("Reference has no geotransform");
    if (!sourceHasGeoTransform || !referenceHasGeoTransform) {
        r.ok = false;
        return r;
    }
    r.summary.sourceGsdM    = std::abs(gts[1]);
    r.summary.referenceGsdM = std::abs(gtr[1]);

    // bbox overlap (no reprojection for validation only)
    r.summary.overlapPercent = Extent::of(hS, gts).overlapPercent(Extent::of(hR, gtr));
    // When CRS differ the envelopes are in incompatible coordinate systems;
    // the raw overlap percentage is meaningless and must not block validation -
    // alignRaster() will reproject the reference before checking real overlap.
    if (!r.summary.crsMismatch && r.summary.overlapPercent < 5.0)
        r.errors.push_back("Insufficient overlap (" +
                           std::to_string(static_cast<int>(r.summary.overlapPercent)) + "%)");

    r.ok = r.errors.empty();
    return r;
}

namespace {

// Reprojects the reference into the CRS of the source, into tmpReproj
void reprojectReference(const std::string &referencePath, const std::string &sourceCrs,
                        const std::string &tmpReproj) {
    LOGD << "Reprojecting reference to source CRS";
    CPLStringList args;
    args.AddString("-t_srs");
    args.AddString(sourceCrs.c_str());
    const auto refSrc = raster::openInputRaster(referencePath);
    if (!refSrc) throw GDALException("Cannot open reference for reprojection");
    GDALDatasetH hR = datasetHandle(refSrc);
    const GdalOptionsPtr<GDALWarpAppOptions> wOpts(GDALWarpAppOptionsNew(args.List(), nullptr));
    auto reprojected = adoptDataset(GDALWarp(tmpReproj.c_str(), nullptr, 1, &hR, wOpts.get(), nullptr));
    if (!reprojected)
        throw GDALException("Reference reprojection failed");
    closeOutputOrThrow(std::move(reprojected), tmpReproj);
}

// What both alignment modes work from: the files, and the source and the reference read onto one
// grid of the area they share
struct AlignInputs {
    std::string sourcePath;
    std::string referencePath;   // as given
    std::string effectiveReference; // in the CRS of the source
    std::string outputPath;
    std::string sourceCrs;
    // The folder of this alignment for the rasters it writes and reads back: an artefact place
    // (artefact_cache.h), so reading them needs no copy
    fs::path scratch;
    bool isDem = false;
    double targetGsd = 0;
    RasterGrid srcGrid, refGrid;
};

AlignInputs loadInputs(const std::string &sourcePath, const std::string &referencePath,
                       const std::string &effectiveRef, const std::string &outputPath,
                       const fs::path &scratch, const AlignValidationResult &val) {
    AlignInputs in;
    in.scratch = scratch;
    in.sourcePath = sourcePath;
    in.referencePath = referencePath;
    in.effectiveReference = effectiveRef;
    in.outputPath = outputPath;
    in.sourceCrs = val.summary.sourceCrs;

    // Overlap bbox + common GSD
    // Owned: readToCommonGrid throws (empty window, failed RasterIO) while both are open
    auto srcDs = raster::openInputRaster(sourcePath);
    auto refDs = raster::openInputRaster(effectiveRef);
    if (!srcDs || !refDs)
        throw GDALException("Cannot reopen datasets for grid extraction");
    GDALDatasetH hS = datasetHandle(srcDs);
    GDALDatasetH hR = datasetHandle(refDs);
    double gts[6], gtr[6];
    requireGeoTransform(hS, gts, sourcePath);
    requireGeoTransform(hR, gtr, referencePath);

    const Extent src = Extent::of(hS, gts);
    const Extent ref = Extent::of(hR, gtr);
    const Extent ov = src.overlap(ref);

    // When the CRS differed, validateAlignRaster() could not check the real
    // overlap (the envelopes were in incompatible coordinate systems). Now that
    // the reference shares the source CRS, verify the overlap and fail fast with
    // a clear message instead of relying on a later "empty window" error.
    if (val.summary.crsMismatch) {
        const double overlapPct = src.overlapPercent(ref);
        if (overlapPct < 5.0) {
            throw AppException("Insufficient overlap after reprojection (" +
                               std::to_string(static_cast<int>(overlapPct)) + "%)");
        }
    }

    in.isDem = detectIsDem(hS);
    // Use the worst (coarsest) GSD for the common grid
    in.targetGsd = std::max(std::abs(gts[1]), std::abs(gtr[1]));

    in.srcGrid = readToCommonGrid(hS, sourcePath, ov, in.targetGsd, in.isDem);
    in.refGrid = readToCommonGrid(hR, referencePath, ov, in.targetGsd, in.isDem);
    srcDs.reset();
    refDs.reset();

    LOGD << "Common grid: " << in.srcGrid.width << "x" << in.srcGrid.height
         << " @ GSD=" << in.targetGsd << " type=" << (in.isDem ? "dem" : "ortho");
    return in;
}

// Applies the transform to the source and writes the COG: warp into a temporary file, DEM Z
// offset, then buildCog; sets result.shiftZ for a DEM
void writeAligned(const AlignInputs &in, const Similarity2D &sim, AlignResult &result) {
    const std::string tmpWarp = io::toUtf8(in.scratch / "warp.tif");
    applyWarp(in.sourcePath, tmpWarp, sim, in.sourceCrs);
    if (in.isDem) {
        result.shiftZ = computeZOffset(tmpWarp, in.effectiveReference);
        applyZOffset(tmpWarp, result.shiftZ);
    }
    buildCog(tmpWarp, in.outputPath);
}

AlignResult alignByTranslation(const AlignInputs &in, AlignResult result) {
    double peakStrength = 0.0;
    auto pc = phaseCorrelate(in.srcGrid, in.refGrid, &peakStrength);
    double dc = pc.first, dr = pc.second;
    // (dc, dr) is the src→ref displacement in common-grid pixels.
    // Map to map units through the common grid transform (same convention as
    // the similarity tie points): T.x = dc·gt[1], T.y = dr·gt[5].
    Similarity2D sim;
    sim.t.x = dc * in.srcGrid.gt[1];
    sim.t.y = dr * in.srcGrid.gt[5];
    LOGD << "Phase correlation: dc=" << dc << " dr=" << dr
         << " → tx=" << sim.t.x << " ty=" << sim.t.y << " m";

    writeAligned(in, sim, result);

    result.success    = true;
    result.txMapUnits = sim.t.x;
    result.tyMapUnits = sim.t.y;
    // Map the peak-to-sidelobe strength to a saturating [0,1] confidence
    // (PSR ~ 10 => 0.5, higher => closer to 1). Replaces the previous fixed
    // 0.8 so callers can distinguish a sharp, reliable match from a weak one.
    result.confidence = std::clamp(peakStrength / (peakStrength + 10.0), 0.0, 1.0);
    return result;
}

// The tie points of the NCC matches of the textured patches of the source
std::vector<TiePoint> matchTiePoints(const AlignInputs &in, const AlignOptions &opts,
                                     const std::vector<PatchCandidate> &patches, const IntegralImages &refII,
                                     double seedDr, double seedDc) {
    const RasterGrid &srcGrid = in.srcGrid;
    std::vector<TiePoint> tiePoints;
    tiePoints.reserve(patches.size());
    double halfPatch = opts.patchSize / 2.0;
    for (auto &p : patches) {
        auto m = matchPatch(srcGrid, refII, in.refGrid, {p.row, p.col, opts.patchSize, opts.searchRadius},
                            seedDr, seedDc);
        if (m.score < 0.3f) continue;
        Vec2 srcMap { srcGrid.gt[0] + (p.col + halfPatch) * srcGrid.gt[1],
                      srcGrid.gt[3] + (p.row + halfPatch) * srcGrid.gt[5] };
        Vec2 refMap { srcGrid.gt[0] + (p.col + halfPatch + m.dc) * srcGrid.gt[1],
                      srcGrid.gt[3] + (p.row + halfPatch + m.dr) * srcGrid.gt[5] };
        tiePoints.push_back({srcMap, refMap});
    }
    return tiePoints;
}

AlignResult alignBySimilarity(const AlignInputs &in, const AlignOptions &opts, AlignResult result) {
    const RasterGrid &srcGrid = in.srcGrid;
    const RasterGrid &refGrid = in.refGrid;
    IntegralImages srcII(srcGrid.data, srcGrid.width, srcGrid.height);
    IntegralImages refII(refGrid.data, refGrid.width, refGrid.height);

    // Phase seed
    double seedDr = 0, seedDc = 0;
    if (opts.usePhaseCorrelationSeed) {
        auto pc = phaseCorrelate(srcGrid, refGrid);
        seedDc = pc.first; seedDr = pc.second;
        LOGD << "Phase seed: dr=" << seedDr << " dc=" << seedDc << " px";
    }

    // Patch selection
    auto patches = selectPatches(srcGrid, srcII, opts.patchSize, opts.maxPatches);
    LOGD << "Candidate patches: " << patches.size();
    if (patches.empty()) {
        result.warningMessage = "No texture patches found; trying translation mode";
        AlignOptions fallback = opts;
        fallback.mode = AlignMode::Translation;
        return alignRaster(in.sourcePath, in.referencePath, in.outputPath, fallback);
    }

    // NCC matching
    const std::vector<TiePoint> tiePoints = matchTiePoints(in, opts, patches, refII, seedDr, seedDc);
    LOGD << "Tie points before RANSAC: " << tiePoints.size();

    if (static_cast<int>(tiePoints.size()) < opts.minInliers) {
        result.warningMessage = "Insufficient texture for similarity estimation; "
                                "falling back to translation-only";
        LOGW << result.warningMessage;
        AlignOptions fallback = opts;
        fallback.mode = AlignMode::Translation;
        return alignRaster(in.sourcePath, in.referencePath, in.outputPath, fallback);
    }

    // RANSAC
    std::vector<bool> inlierMask;
    Similarity2D sim = ransac(tiePoints, opts.ransacThreshold,
                              opts.ransacIterations, inlierMask);
    int inlierCount = static_cast<int>(
        std::count(inlierMask.begin(), inlierMask.end(), true));
    LOGD << "RANSAC inliers: " << inlierCount << "/" << tiePoints.size();

    if (inlierCount < opts.minInliers ||
        static_cast<double>(inlierCount) / tiePoints.size() < opts.minInlierRatio) {
        result.warningMessage =
            "Low RANSAC inlier count (" + std::to_string(inlierCount) +
            "/" + std::to_string(tiePoints.size()) + "); result may be inaccurate";
        LOGW << result.warningMessage;
    }

    // RMSE over inliers
    double rmse = 0; int nin = 0;
    for (int i = 0; i < static_cast<int>(tiePoints.size()); i++)
        if (inlierMask[i]) { rmse += std::pow(sim.residual(tiePoints[i]), 2); nin++; }
    rmse = nin > 0 ? std::sqrt(rmse / nin) : 0.0;

    double inlierRatio = static_cast<double>(inlierCount) / tiePoints.size();
    double confidence  = inlierRatio * std::exp(-rmse / (3.0 * opts.ransacThreshold));

    // Apply and write output
    writeAligned(in, sim, result);

    result.success      = true;
    result.txMapUnits   = sim.t.x;
    result.tyMapUnits   = sim.t.y;
    result.thetaDeg     = ddb::rad2deg(std::atan2(sim.sinTheta, sim.cosTheta));
    result.scale        = sim.scale;
    result.inlierCount  = inlierCount;
    result.inlierRatio  = inlierRatio;
    result.rmseMapUnits = rmse;
    result.confidence   = confidence;
    return result;
}

}  // namespace

AlignResult alignRaster(const std::string &sourcePath,
                        const std::string &referencePath,
                        const std::string &outputPath,
                        const AlignOptions &opts)
{
    AlignResult result;
    result.mode = (opts.mode == AlignMode::Similarity) ? "similarity" : "translation";

    // 1. Validation
    auto val = validateAlignRaster(sourcePath, referencePath);
    if (!val.ok) throw AppException("Alignment validation failed: " + val.errors[0]);

    // 2. Optional reprojection of the reference into the source CRS
    // The rasters written on the way (the reprojected reference, the warped source) go to a
    // folder of the artefacts of the cache, removed when the alignment ends
    const io::ScopedTempDir scratch = raster::makeArtefactRequestFolder("align-");
    std::string effectiveRef = referencePath;

    if (val.summary.crsMismatch) {
        effectiveRef = io::toUtf8(scratch.path() / "reference.tif");
        reprojectReference(referencePath, val.summary.sourceCrs, effectiveRef);
    }

    // 3. The overlap of both rasters on one grid, then the estimate
    const AlignInputs in = loadInputs(sourcePath, referencePath, effectiveRef, outputPath, scratch.path(), val);
    if (opts.mode == AlignMode::Translation)
        return alignByTranslation(in, result);
    return alignBySimilarity(in, opts, result);
}

std::string alignResultToJson(const AlignResult &r) {
    json j;
    j["success"]     = r.success;
    j["tx"]          = r.txMapUnits;
    j["ty"]          = r.tyMapUnits;
    j["thetaDeg"]    = r.thetaDeg;
    j["scale"]       = r.scale;
    j["shiftZ"]      = r.shiftZ;
    j["inlierCount"] = r.inlierCount;
    j["inlierRatio"] = r.inlierRatio;
    j["rmse"]        = r.rmseMapUnits;
    j["confidence"]  = r.confidence;
    j["mode"]        = r.mode;
    j["warning"]     = r.warningMessage;
    return j.dump(2);
}

std::string alignValidationToJson(const AlignValidationResult &r) {
    json j;
    j["ok"]       = r.ok;
    j["errors"]   = r.errors;
    j["warnings"] = r.warnings;
    j["summary"]  = {
        {"sourceCrs",      r.summary.sourceCrs},
        {"referenceCrs",   r.summary.referenceCrs},
        {"crsMismatch",    r.summary.crsMismatch},
        {"overlapPercent", r.summary.overlapPercent},
        {"sourceType",     r.summary.sourceType},
        {"referenceType",  r.summary.referenceType},
        {"sourceGsdM",     r.summary.sourceGsdM},
        {"referenceGsdM",  r.summary.referenceGsdM}
    };
    return j.dump(2);
}

}  // namespace ddb
