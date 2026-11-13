/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "cpu_requirements.h"

#include <string>

#include "exceptions.h"

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <immintrin.h>
#include <intrin.h>
#endif

namespace ddb
{
    namespace
    {
        // The features missing from this CPU, comma-separated; empty when it has them all
        std::string missingCpuFeatures()
        {
            std::string missing;
            const auto need = [&missing](bool has, const char *name) {
                if (!has)
                    missing += (missing.empty() ? "" : ", ") + std::string(name);
            };
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
            __builtin_cpu_init();
            need(__builtin_cpu_supports("avx2"), "AVX2");
            need(__builtin_cpu_supports("fma"), "FMA");
            need(__builtin_cpu_supports("bmi2"), "BMI2");
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
            int regs[4] = {};
            __cpuid(regs, 1);
            const bool fma = (regs[2] & (1 << 12)) != 0;
            const bool osxsave = (regs[2] & (1 << 27)) != 0;
            const bool avx = (regs[2] & (1 << 28)) != 0;
            // The operating system saves the SSE and AVX registers (XCR0 bits 1 and 2)
            const bool avxState = osxsave && avx && (_xgetbv(0) & 0x6) == 0x6;
            __cpuidex(regs, 7, 0);
            need(avxState && (regs[1] & (1 << 5)) != 0, "AVX2");
            need(fma, "FMA");
            need((regs[1] & (1 << 8)) != 0, "BMI2");
#else
            (void)need;
#endif
            return missing;
        }
    }

    void requireCpuFeatures()
    {
        const std::string missing = missingCpuFeatures();
        if (!missing.empty())
            throw AppException("This build of DroneDB needs an x86-64-v3 CPU (AVX2, FMA, BMI2); this CPU lacks " +
                               missing);
    }
}
