/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <iomanip>
#include "progressbar.h"

#include <string>
#include <algorithm>

namespace cmd{

void ProgressBar::update(const std::string &label, float progress){
    ++ticks;
    unsigned int w = label.empty() ? barWidth : std::max<unsigned int>(3, barWidth - label.length() - 1);
    const unsigned int pos = static_cast<int>(w * progress / 100.0f);

    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    const auto time_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count();

    // Print label
    if (!label.empty()){
        // Check whether we need to print a new line (if label has changed)
        if (label != lastLabel){
            if (!lastLabel.empty()) std::cout << std::endl;
            lastLabel = label;
        }

        std::cout << label << " ";
    }

    std::cout << "[";

    for (unsigned int i = 0; i < w; ++i) {
        if (i < pos) std::cout << completeChar;
        else if (i == pos) std::cout << completeChar;
        else std::cout << incompleteChar;
    }

    std::cout << "] ";
    if (progress < 10) std::cout << " ";
    std::cout << std::fixed << std::setprecision(2) << progress << "% "
              << std::setprecision(2) << float(time_elapsed) / 1000.0 << "s\r";
    std::cout.flush();
}

void ProgressBar::done() const{
    std::cout << std::endl;
}

}
