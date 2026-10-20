/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef RANDOMUTIL_H
#define RANDOMUTIL_H

#include <random>
#include <type_traits>

namespace ddb {

// The one random source for non-cryptographic uses (jitter, sampling, unique names).
// std::rand() shares global state across threads and is not safe to call concurrently;
// each thread gets its own engine, seeded once from std::random_device.
inline std::mt19937 &threadRandomEngine() {
    thread_local std::mt19937 engine{std::random_device{}()};
    return engine;
}

// Uniform integer in [lo, hi] (both inclusive) from the calling thread's engine.
template <typename T>
T randomInt(T lo, T hi) {
    static_assert(std::is_integral_v<T>, "randomInt needs an integral type");
    return std::uniform_int_distribution<T>(lo, hi)(threadRandomEngine());
}

// True with probability 1/n (n > 0): for occasional housekeeping such as cache cleanup.
inline bool oneIn(unsigned n) {
    return n <= 1 || randomInt<unsigned>(0, n - 1) == 0;
}

}  // namespace ddb

#endif  // RANDOMUTIL_H
