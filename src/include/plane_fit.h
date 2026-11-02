/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef PLANE_FIT_H
#define PLANE_FIT_H

#include <cstddef>
#include <vector>

namespace ddb {

// The plane z = a x + b y + c
struct Plane {
    double a = 0.0, b = 0.0, c = 0.0;
    double at(double x, double y) const { return a * x + b * y + c; }
};

// The sums of the normal equations of a weighted least-squares plane z = a x + b y + c
struct PlaneSums {
    double xx = 0, xy = 0, x = 0, yy = 0, y = 0, xz = 0, yz = 0, z = 0;
    double n = 0; // the sum of the weights: the number of points when they all weigh 1

    void add(double px, double py, double pz, double w = 1.0) {
        xx += w * px * px; xy += w * px * py; x += w * px;
        yy += w * py * py; y += w * py;
        xz += w * px * pz; yz += w * py * pz; z += w * pz;
        n += w;
    }
};

// Solves the normal equations of the sums (3x3 Gaussian elimination with partial pivoting).
// False when they are degenerate: fewer than three points, or all of them on a line.
bool solvePlane(const PlaneSums &s, Plane &out);

// Points (x, y, z), each coordinate in a vector of its own
struct PlanePoints {
    std::vector<double> x, y, z;

    void add(double px, double py, double pz) { x.push_back(px); y.push_back(py); z.push_back(pz); }
    std::size_t size() const { return z.size(); }
    bool empty() const { return z.empty(); }
};

// The least-squares plane through the points, in their coordinates. The points are centred on
// their centroid for the solution, which keeps it well conditioned with large map coordinates.
// Points on a line (or fewer than three) give the horizontal plane at their mean elevation.
// The points must not be empty.
Plane bestFitPlane(const PlanePoints &points);

// A least-squares plane that ignores the points far from it: the plane of all the points, then
// again `iterations` times the plane of the points within `keep` times the RMS residual of the
// previous one. Something that is not ground (a vehicle, another pile, a ditch) in a ring of
// ground is left out that way.
struct RobustPlane {
    Plane plane;
    double rms = 0.0;   // the RMS residual of the points the plane was fitted to
    std::size_t inliers = 0; // how many those are
};
RobustPlane robustPlaneFit(const PlanePoints &points, int iterations = 2, double keep = 2.0);

} // namespace ddb

#endif // PLANE_FIT_H
