/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "plane_fit.h"

#include <cmath>
#include <utility>

namespace ddb {

// A 3x3 Gaussian elimination: good enough for the tiny matrices built here (and avoids pulling
// Eigen into DroneDB for one call).
bool solvePlane(const PlaneSums &s, Plane &out) {
    double m[3][4] = {
        {s.xx, s.xy, s.x, s.xz},
        {s.xy, s.yy, s.y, s.yz},
        {s.x,  s.y,  s.n, s.z}
    };
    for (int i = 0; i < 3; i++) {
        int piv = i;
        for (int k = i + 1; k < 3; k++)
            if (std::fabs(m[k][i]) > std::fabs(m[piv][i])) piv = k;
        if (std::fabs(m[piv][i]) < 1e-12) return false;
        if (piv != i) std::swap(m[i], m[piv]);
        for (int k = i + 1; k < 3; k++) {
            const double f = m[k][i] / m[i][i];
            for (int j = i; j < 4; j++) m[k][j] -= f * m[i][j];
        }
    }
    const double c = m[2][3] / m[2][2];
    const double b = (m[1][3] - m[1][2] * c) / m[1][1];
    const double a = (m[0][3] - m[0][1] * b - m[0][2] * c) / m[0][0];
    out.a = a; out.b = b; out.c = c;
    return true;
}

Plane bestFitPlane(const PlanePoints &points) {
    Plane basePlane;
    const size_t n = points.size();
    double cx = 0, cy = 0;
    for (size_t i = 0; i < n; i++) { cx += points.x[i]; cy += points.y[i]; }
    cx /= static_cast<double>(n); cy /= static_cast<double>(n);

    PlaneSums sums;
    for (size_t i = 0; i < n; i++)
        sums.add(points.x[i] - cx, points.y[i] - cy, points.z[i]);
    Plane local{};
    if (!solvePlane(sums, local)) {
        basePlane.c = sums.z / sums.n;
    } else {
        // Back to the coordinates of the points: z = a(X-cx) + b(Y-cy) + c
        //                                          = aX + bY + (c - a*cx - b*cy)
        basePlane.a = local.a;
        basePlane.b = local.b;
        basePlane.c = local.c - local.a * cx - local.b * cy;
    }
    return basePlane;
}

namespace {

double rmsResidual(const PlanePoints &points, const Plane &plane) {
    double s = 0.0;
    for (size_t i = 0; i < points.size(); i++) {
        const double r = points.z[i] - plane.at(points.x[i], points.y[i]);
        s += r * r;
    }
    return std::sqrt(s / static_cast<double>(points.size()));
}

} // namespace

RobustPlane robustPlaneFit(const PlanePoints &points, int iterations, double keep) {
    RobustPlane fit;
    fit.plane = bestFitPlane(points);
    fit.rms = rmsResidual(points, fit.plane);
    fit.inliers = points.size();
    for (int it = 0; it < iterations && fit.rms > 0.0; it++) {
        const double limit = keep * fit.rms;
        PlanePoints kept;
        for (size_t i = 0; i < points.size(); i++) {
            if (std::fabs(points.z[i] - fit.plane.at(points.x[i], points.y[i])) <= limit)
                kept.add(points.x[i], points.y[i], points.z[i]);
        }
        // Too few points left to say more than the plane of all of them
        if (kept.size() < 3 || kept.size() == fit.inliers) break;
        fit.plane = bestFitPlane(kept);
        fit.rms = rmsResidual(kept, fit.plane);
        fit.inliers = kept.size();
    }
    return fit;
}

} // namespace ddb
