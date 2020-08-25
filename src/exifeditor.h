/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef EXIFEDITOR_H
#define EXIFEDITOR_H

#include <vector>
#include "fs.h"
#include "ddb_export.h"

namespace ddb{

class ExifEditor {
  std::vector<fs::path> files;

  public:
    DDB_DLL ExifEditor(const std::string &file);
    DDB_DLL ExifEditor(std::vector<std::string> &files);

    DDB_DLL bool canEdit();

    DDB_DLL void SetGPSAltitude(double altitude);
    DDB_DLL void SetGPSLatitude(double latitude);
    DDB_DLL void SetGPSLongitude(double longitude);
    DDB_DLL void SetGPS(double latitude, double longitude, double altitude);
  protected:
    template<typename Func>
    void eachFile(Func f);

    const std::string doubleToDMS(double d);
    const std::string doubleToFraction(double d, int precision);
};

}
#endif // EXIFEDITOR_H
