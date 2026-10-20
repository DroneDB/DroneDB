/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef MATHUTIL_H
#define MATHUTIL_H

#include <type_traits>

namespace ddb {

// The one definition of pi (std::numbers::pi needs C++20). M_PI is not standard C++
// (MSVC hides it behind _USE_MATH_DEFINES), so it must not be used or redefined.
constexpr double kPi = 3.14159265358979323846;

// True when value is a positive integral power of two (1, 2, 4, ...).
template <typename T>
constexpr bool isPowerOfTwo(T value) {
    static_assert(std::is_integral_v<T>, "isPowerOfTwo needs an integral type");
    return value > 0 && (value & (value - 1)) == 0;
}

}  // namespace ddb

#endif  // MATHUTIL_H
