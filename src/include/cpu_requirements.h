/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CPU_REQUIREMENTS_H
#define CPU_REQUIREMENTS_H

namespace ddb
{
    // Throws AppException when the CPU lacks the instructions the library is compiled for
    // (x86-64-v3: AVX2, FMA, BMI2 and the AVX state saved by the operating system), so a host on
    // an older CPU gets an error at start-up instead of an illegal instruction later. Its
    // translation unit must stay compiled for plain x86-64. No check on other architectures.
    void requireCpuFeatures();
}

#endif // CPU_REQUIREMENTS_H
