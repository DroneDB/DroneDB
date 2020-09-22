/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef PROGRESSBAR_H
#define PROGRESSBAR_H

#include <iostream>
#include <chrono>

namespace cmd{

// TODO: support for multiple bars/tracks
// TODO: read console width to adjust bar width
class ProgressBar {
private:
    unsigned int ticks = 0;

    const unsigned int barWidth;
    const char completeChar = '#';
    const char incompleteChar = '-';
    const std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();

    std::string lastLabel = "";
public:
    ProgressBar() : barWidth(40) {}
    ProgressBar(unsigned int width) :
                barWidth(width) {}

    void update(const std::string &label, float progress);
    void done() const;
};

}

#endif // PROGRESSBAR_H
