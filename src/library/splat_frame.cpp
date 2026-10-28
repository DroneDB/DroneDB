/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "splat_frame.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <numeric>
#include <random>
#include <utility>

#include "json.h"

namespace ddb
{
    namespace
    {
        using Vec3 = std::array<double, 3>;
        using Mat3 = std::array<std::array<double, 3>, 3>;

        // The estimate needs a ground to find: fewer splats than this are not a scene
        constexpr size_t kMinSplats = 16;
        // The fit looks at this many splats at most (every nth of the others): the RANSAC scores
        // every one of them for each plane it tries
        constexpr size_t kMaxSplats = 40000;
        constexpr int kRansacIterations = 300;
        constexpr uint32_t kSeed = 0x5eed5eedu;
        // A splat this transparent is not part of the scene
        constexpr double kMinOpacity = 0.05;
        // A plane is as thick as this much of the extent of the scene
        constexpr double kPlaneThickness = 0.02;
        // ... and the ground is looked for again in a slab a quarter as thick. A level ground is the
        // densest plane however thin it is taken; a slope that wins only in the thicker slab (a wooded
        // hillside behind a subject that stands on level ground) leaves the scene with two grounds.
        constexpr double kThinPlaneThickness = 0.005;
        // The planes fitted one after the other at each thickness
        constexpr int kPlaneCandidates = 6;
        // The grounds found at the two thicknesses are one plane up to the first angle, and two from the second
        constexpr double kAgreeDegrees = 8.0;
        constexpr double kDisagreeDegrees = 15.0;
        // A median this close to the ground (in standard deviations) says nothing about the side
        constexpr double kSideDeadZone = 0.05;
        // The plane through three splats is not one if they are nearly on a line
        constexpr double kMinTriangleSine = 0.1;
        // Below this share of the opacity there is no ground: the flat axis of everything is used
        constexpr double kMinGroundSupport = 0.12;
        constexpr double kPcaConfidenceCeiling = 0.8;

        double dot(const Vec3 &a, const Vec3 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
        Vec3 sub(const Vec3 &a, const Vec3 &b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
        Vec3 cross(const Vec3 &a, const Vec3 &b)
        {
            return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        }
        double length(const Vec3 &a) { return std::sqrt(dot(a, a)); }

        // 0 at `from`, 1 at `to`, linear between
        double ramp(double x, double from, double to)
        {
            return std::clamp((x - from) / (to - from), 0.0, 1.0);
        }

        struct Points
        {
            std::vector<Vec3> p;
            std::vector<double> w;
            double totalWeight = 0.0;
            double extent = 0.0; // length of the diagonal of the 1st..99th percentile box
        };

        Points gather(const std::vector<float> &positions, const std::vector<float> &opacities)
        {
            const size_t n = positions.size() / 3;
            const size_t stride = std::max<size_t>(1, (n + kMaxSplats - 1) / kMaxSplats);
            Points points;
            for (size_t i = 0; i < n; i += stride)
            {
                const Vec3 p = {positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]};
                if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]))
                    continue;
                const double w = i < opacities.size() ? static_cast<double>(opacities[i]) : 1.0;
                if (!(w >= kMinOpacity)) // also a NaN
                    continue;
                points.p.push_back(p);
                points.w.push_back(std::min(w, 1.0));
                points.totalWeight += points.w.back();
            }

            // The extent ignores the outliers a scene always has (floaters, a far background)
            double diagonal = 0.0;
            for (int axis = 0; axis < 3 && !points.p.empty(); ++axis)
            {
                std::vector<double> values;
                values.reserve(points.p.size());
                for (const Vec3 &p : points.p)
                    values.push_back(p[axis]);
                const size_t lo = values.size() / 100;
                const size_t hi = values.size() - 1 - lo;
                std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(lo), values.end());
                const double mn = values[lo];
                std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(hi), values.end());
                diagonal += (values[hi] - mn) * (values[hi] - mn);
            }
            points.extent = std::sqrt(diagonal);
            return points;
        }

        // Eigenvalues (descending) and eigenvectors (columns) of a symmetric matrix, by Jacobi
        // rotations: exact enough for 3x3, and no dependency
        void eigenSymmetric(Mat3 a, std::array<double, 3> &values, Mat3 &vectors)
        {
            vectors = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
            for (int sweep = 0; sweep < 50; ++sweep)
            {
                const double off = std::abs(a[0][1]) + std::abs(a[0][2]) + std::abs(a[1][2]);
                if (off < 1e-300)
                    break;
                for (int p = 0; p < 2; ++p)
                    for (int q = p + 1; q < 3; ++q)
                    {
                        if (std::abs(a[p][q]) < 1e-300)
                            continue;
                        const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
                        const double t = (theta >= 0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                        const double c = 1.0 / std::sqrt(t * t + 1.0);
                        const double s = t * c;
                        for (int k = 0; k < 3; ++k)
                        {
                            const double akp = a[k][p], akq = a[k][q];
                            a[k][p] = c * akp - s * akq;
                            a[k][q] = s * akp + c * akq;
                        }
                        for (int k = 0; k < 3; ++k)
                        {
                            const double apk = a[p][k], aqk = a[q][k];
                            a[p][k] = c * apk - s * aqk;
                            a[q][k] = s * apk + c * aqk;
                        }
                        for (int k = 0; k < 3; ++k)
                        {
                            const double vkp = vectors[k][p], vkq = vectors[k][q];
                            vectors[k][p] = c * vkp - s * vkq;
                            vectors[k][q] = s * vkp + c * vkq;
                        }
                    }
            }

            std::array<int, 3> order = {0, 1, 2};
            std::sort(order.begin(), order.end(), [&](int i, int j) { return a[i][i] > a[j][j]; });
            Mat3 sorted{};
            for (int c = 0; c < 3; ++c)
            {
                values[c] = std::max(a[order[c]][order[c]], 0.0);
                for (int k = 0; k < 3; ++k)
                    sorted[k][c] = vectors[k][order[c]];
            }
            vectors = sorted;
        }

        struct Spread
        {
            Vec3 mean{};
            std::array<double, 3> values{}; // descending
            Mat3 vectors{};                 // the axes, as columns
        };

        // The weighted mean and principal axes of the points `selected` names (all of them if empty)
        Spread spreadOf(const Points &points, const std::vector<size_t> *selected)
        {
            Spread spread;
            const size_t n = selected != nullptr ? selected->size() : points.p.size();
            auto at = [&](size_t i) { return selected != nullptr ? (*selected)[i] : i; };

            double total = 0.0;
            for (size_t i = 0; i < n; ++i)
            {
                const size_t k = at(i);
                total += points.w[k];
                for (int d = 0; d < 3; ++d)
                    spread.mean[d] += points.w[k] * points.p[k][d];
            }
            if (total <= 0.0)
                return spread;
            for (double &m : spread.mean)
                m /= total;

            Mat3 cov{};
            for (size_t i = 0; i < n; ++i)
            {
                const size_t k = at(i);
                const Vec3 d = sub(points.p[k], spread.mean);
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c)
                        cov[r][c] += points.w[k] * d[r] * d[c] / total;
            }
            eigenSymmetric(cov, spread.values, spread.vectors);
            return spread;
        }

        Vec3 axisOf(const Spread &spread, int column)
        {
            return {spread.vectors[0][column], spread.vectors[1][column], spread.vectors[2][column]};
        }

        struct Plane
        {
            Vec3 normal{0, 0, 1};
            double offset = 0.0; // normal . p + offset == 0 on the plane
            double distance(const Vec3 &p) const { return dot(normal, p) + offset; }
        };

        // The opacity within `thickness` of the plane, over the points `among` names (all of them when
        // null), and which points those are (in ascending order)
        double supportOf(const Points &points, const Plane &plane, double thickness, const std::vector<size_t> *among,
                         std::vector<size_t> *inliers)
        {
            double sum = 0.0;
            if (inliers != nullptr)
                inliers->clear();
            const size_t n = among != nullptr ? among->size() : points.p.size();
            for (size_t j = 0; j < n; ++j)
            {
                const size_t i = among != nullptr ? (*among)[j] : j;
                if (std::abs(plane.distance(points.p[i])) <= thickness)
                {
                    sum += points.w[i];
                    if (inliers != nullptr)
                        inliers->push_back(i);
                }
            }
            return sum;
        }

        // The plane through three of the `among` splats that most of their opacity lies on; false when
        // no three of them make a plane
        bool ransacPlane(const Points &points, const std::vector<size_t> &among, double thickness, std::mt19937 &rng,
                         Plane &best)
        {
            const size_t n = among.size();
            double bestSupport = -1.0;
            for (int it = 0; it < kRansacIterations; ++it)
            {
                const size_t a = among[rng() % n], b = among[rng() % n], c = among[rng() % n];
                if (a == b || b == c || a == c)
                    continue;
                const Vec3 u = sub(points.p[b], points.p[a]);
                const Vec3 v = sub(points.p[c], points.p[a]);
                const Vec3 normal = cross(u, v);
                const double len = length(normal);
                if (len <= kMinTriangleSine * length(u) * length(v) || len <= 0.0)
                    continue;
                Plane plane;
                plane.normal = {normal[0] / len, normal[1] / len, normal[2] / len};
                plane.offset = -dot(plane.normal, points.p[a]);
                const double support = supportOf(points, plane, thickness, &among, nullptr);
                if (support > bestSupport)
                {
                    bestSupport = support;
                    best = plane;
                }
            }
            return bestSupport >= 0.0;
        }

        // A plane the splats lie on, and how much of them it holds
        struct PlaneCandidate
        {
            Plane plane;
            double share = 0.0;    // of all the opacity, within the thickness of the plane
            double ownShare = 0.0; // of the opacity that no plane found before it holds
            double inPlane = 0.0;  // sqrt(l2 / l1) of the two spreads of its splats: a line is not a ground
            bool found = false;
        };

        // Refits `plane` to all the splats on it (twice: the first fit moves it, and the splats with it)
        void refit(const Points &points, Plane plane, double thickness, PlaneCandidate &candidate)
        {
            std::vector<size_t> inliers;
            for (int pass = 0; pass < 2; ++pass)
            {
                supportOf(points, plane, thickness, nullptr, &inliers);
                if (inliers.size() < 3)
                    break;
                const Spread spread = spreadOf(points, &inliers);
                plane.normal = axisOf(spread, 2);
                plane.offset = -dot(plane.normal, spread.mean);
            }
            candidate.plane = plane;
            candidate.share = supportOf(points, plane, thickness, nullptr, &inliers) / points.totalWeight;
            if (inliers.size() >= 3)
            {
                const Spread spread = spreadOf(points, &inliers);
                candidate.inPlane = spread.values[0] > 0.0 ? std::sqrt(spread.values[1] / spread.values[0]) : 0.0;
            }
        }

        // The plane the scene stands on, among planes `thickness` thick. Several are found, one after the
        // other, each among the splats the planes before it do not hold (a room has a floor, a ceiling and
        // walls; a street has the road and the roofs), and the one kept holds the most opacity of its own
        // weighted by |cos|^4 of its angle to the flattest axis of the scene: a wall can hold more splats
        // than the floor, but a room is lower than it is wide.
        PlaneCandidate dominantPlane(const Points &points, double thickness, const Vec3 &flattest)
        {
            std::mt19937 rng(kSeed);
            std::vector<size_t> remaining(points.p.size());
            std::iota(remaining.begin(), remaining.end(), size_t{0});
            std::vector<size_t> inliers, rest;
            PlaneCandidate best;
            double bestScore = -1.0;
            for (int k = 0; k < kPlaneCandidates && remaining.size() >= kMinSplats; ++k)
            {
                Plane plane;
                if (!ransacPlane(points, remaining, thickness, rng, plane))
                    break;
                PlaneCandidate candidate;
                refit(points, plane, thickness, candidate);
                candidate.ownShare = supportOf(points, candidate.plane, thickness, &remaining, nullptr) / points.totalWeight;
                candidate.found = true;
                const double score = candidate.ownShare * std::pow(dot(candidate.plane.normal, flattest), 4);
                if (score > bestScore)
                {
                    bestScore = score;
                    best = candidate;
                }

                supportOf(points, candidate.plane, thickness, nullptr, &inliers);
                rest.clear();
                std::set_difference(remaining.begin(), remaining.end(), inliers.begin(), inliers.end(),
                                    std::back_inserter(rest));
                remaining.swap(rest);
            }
            return best;
        }

        // Which side of the ground along `normal` the scene is on. The ground is the densest layer of
        // splats, `thickness` thick, between the 1st and 99th percentile of the heights; `sideBias` is
        // (above - below) / (above + below) of the opacity off it, and `medianOffset` how far the
        // weighted median height is above it (0 within it), in standard deviations of the heights. Both
        // are positive when the scene stands on the layer.
        struct Side
        {
            double sideBias = 0.0;
            double medianOffset = 0.0;
        };

        Side sideOf(const Points &points, const Vec3 &normal, double thickness)
        {
            std::vector<std::pair<double, double>> heights; // height, weight
            heights.reserve(points.p.size());
            for (size_t i = 0; i < points.p.size(); ++i)
                heights.emplace_back(dot(points.p[i], normal), points.w[i]);
            std::sort(heights.begin(), heights.end());
            const size_t lo = heights.size() / 100;
            const size_t hi = heights.size() - 1 - lo;
            const double from = heights[lo].first, to = heights[hi].first;

            Side side;
            if (!(to > from) || !(thickness > 0.0))
                return side;
            const size_t bins = std::clamp<size_t>(static_cast<size_t>((to - from) / thickness), 8, 4096);
            std::vector<double> histogram(bins, 0.0);
            double total = 0.0, mean = 0.0;
            for (size_t i = lo; i <= hi; ++i)
            {
                const auto bin = std::min(bins - 1, static_cast<size_t>((heights[i].first - from) / (to - from) * static_cast<double>(bins)));
                histogram[bin] += heights[i].second;
                total += heights[i].second;
                mean += heights[i].second * heights[i].first;
            }
            mean /= total;
            double variance = 0.0, median = from, cumulative = 0.0;
            bool medianFound = false;
            for (size_t i = lo; i <= hi; ++i)
            {
                variance += heights[i].second * (heights[i].first - mean) * (heights[i].first - mean) / total;
                cumulative += heights[i].second;
                if (!medianFound && cumulative >= total / 2.0)
                {
                    median = heights[i].first;
                    medianFound = true;
                }
            }
            const size_t densest = static_cast<size_t>(std::max_element(histogram.begin(), histogram.end()) - histogram.begin());
            const double ground = from + (static_cast<double>(densest) + 0.5) * (to - from) / static_cast<double>(bins);

            double above = 0.0, below = 0.0;
            for (const auto &[h, w] : heights)
            {
                if (h - ground > thickness)
                    above += w;
                else if (h - ground < -thickness)
                    below += w;
            }
            side.sideBias = above + below > 0.0 ? (above - below) / (above + below) : 0.0;
            // A median within the layer is on neither side of it
            const double off = median - ground;
            const double beyond = std::abs(off) <= thickness ? 0.0 : off - std::copysign(thickness, off);
            side.medianOffset = variance > 0.0 ? beyond / std::sqrt(variance) : 0.0;
            return side;
        }

        double degreesBetweenAxes(const Vec3 &a, const Vec3 &b)
        {
            return std::acos(std::min(1.0, std::abs(dot(a, b)))) * 180.0 / 3.14159265358979323846;
        }

        SplatUp groundPlaneUp(const Points &points, const PlaneCandidate &thick, const PlaneCandidate &thin,
                              double thickness, double flatness)
        {
            // The thin plane places the ground more precisely; the side is read at the usual thickness
            Vec3 normal = thin.found ? thin.plane.normal : thick.plane.normal;
            Side side = sideOf(points, normal, thickness);
            if (side.sideBias < 0.0)
            {
                for (double &c : normal)
                    c = -c;
                side.sideBias = -side.sideBias;
                side.medianOffset = -side.medianOffset;
            }
            // The two readings of the side must not contradict each other; either is enough when they do not
            const double sideClear = side.medianOffset <= -kSideDeadZone
                                         ? 0.0
                                         : std::max(ramp(side.sideBias, 0.1, 0.5), ramp(side.medianOffset, kSideDeadZone, 0.25));
            const double tilt = thin.found ? degreesBetweenAxes(thick.plane.normal, thin.plane.normal) : 90.0;

            SplatUp up;
            up.up = normal;
            up.source = SplatUpSource::GroundPlane;
            up.confidence = ramp(thick.share, 0.08, 0.30) * ramp(thick.inPlane, 0.05, 0.25) * ramp(flatness, 0.3, 0.6) *
                            sideClear * (1.0 - ramp(tilt, kAgreeDegrees, kDisagreeDegrees));
            return up;
        }

        SplatUp pcaUp(const Points &points, const Spread &spread, double flatness)
        {
            Vec3 normal = axisOf(spread, 2);

            // The skew of the splats along the flat axis: a floor with things on it has its tail up
            double m2 = 0.0, m3 = 0.0, total = 0.0;
            for (size_t i = 0; i < points.p.size(); ++i)
            {
                const double d = dot(sub(points.p[i], spread.mean), normal);
                m2 += points.w[i] * d * d;
                m3 += points.w[i] * d * d * d;
                total += points.w[i];
            }
            m2 /= total;
            m3 /= total;
            const double skew = m2 > 0.0 ? m3 / std::pow(m2, 1.5) : 0.0;
            if (skew < 0.0)
                for (double &c : normal)
                    c = -c;

            SplatUp up;
            up.up = normal;
            up.source = SplatUpSource::Pca;
            up.confidence = kPcaConfidenceCeiling * ramp(flatness, 0.3, 0.8) * ramp(std::abs(skew), 0.2, 1.0);
            return up;
        }
    } // namespace

    const char *splatUpSourceName(SplatUpSource source)
    {
        switch (source)
        {
            case SplatUpSource::Georef: return "georef";
            case SplatUpSource::GroundPlane: return "ground-plane";
            case SplatUpSource::Pca: return "pca";
        }
        return "pca";
    }

    void to_json(json &j, const SplatUp &up)
    {
        // Rounded: the vector is an estimate, and the file is read by people
        const auto round6 = [](double v) { return std::round(v * 1e6) / 1e6; };
        j = json::object();
        j["up"] = {round6(up.up[0]), round6(up.up[1]), round6(up.up[2])};
        j["upConfidence"] = round6(up.confidence);
        j["upSource"] = splatUpSourceName(up.source);
    }

    SplatUp estimateSplatUp(const std::vector<float> &positions, const std::vector<float> &opacities,
                            bool georeferenced)
    {
        SplatUp result;
        if (georeferenced)
        {
            result.confidence = 1.0;
            result.source = SplatUpSource::Georef;
            return result;
        }

        const Points points = gather(positions, opacities);
        if (points.p.size() < kMinSplats || !(points.extent > 0.0) || !(points.totalWeight > 0.0))
            return result;

        // The flattest axis of everything, and how flat the scene is along it: 1 - sqrt(l3 / l2)
        const Spread all = spreadOf(points, nullptr);
        const Vec3 flattest = axisOf(all, 2);
        const double flatness = all.values[1] > 0.0 ? 1.0 - std::sqrt(all.values[2] / all.values[1]) : 0.0;

        const double thickness = kPlaneThickness * points.extent;
        const PlaneCandidate thick = dominantPlane(points, thickness, flattest);
        if (!thick.found || thick.share < kMinGroundSupport)
            return pcaUp(points, all, flatness);
        const PlaneCandidate thin = dominantPlane(points, kThinPlaneThickness * points.extent, flattest);
        return groundPlaneUp(points, thick, thin, thickness, flatness);
    }

    SplatUp estimateSplatUp(const RadCoarseSplats &splats, bool georeferenced)
    {
        return estimateSplatUp(splats.positions, splats.opacities, georeferenced);
    }

} // namespace ddb
