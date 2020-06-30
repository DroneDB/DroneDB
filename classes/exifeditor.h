/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef EXIFEDITOR_H
#define EXIFEDITOR_H

#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

namespace ddb{

class ExifEditor {
  std::vector<fs::path> files;

  public:
    ExifEditor(const std::string &file);
    ExifEditor(std::vector<std::string> &files);

    bool canEdit();

    void SetGPSAltitude(double altitude);
    void SetGPSLatitude(double latitude);
    void SetGPSLongitude(double longitude);
    void SetGPS(double latitude, double longitude, double altitude);
  protected:
    template<typename Func>
    void eachFile(Func f);

    const std::string doubleToDMS(double d);
    const std::string doubleToFraction(double d, int precision);
};

}
#endif // EXIFEDITOR_H
