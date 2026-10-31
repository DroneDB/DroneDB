/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef EXIFEDITOR_H
#define EXIFEDITOR_H

#include <ostream>
#include <string>
#include <vector>
#include "exif_metadata.h"
#include "fs.h"

namespace ddb
{

  /// A file whose EXIF could not be written
  struct ExifWriteFailure
  {
    std::string file;    // UTF-8
    std::string message; // why
  };

  class ExifEditor
  {
    std::vector<fs::path> files;

  public:
    ExifEditor(const std::string &file);
    ExifEditor(std::vector<std::string> &files);

    /// True when every file can be edited; writes one line to `rejected` for each file that cannot
    /// ("<file>: <reason>")
    bool canEdit(std::ostream &rejected);

    /// The Set* methods write each file once, in the order given, and go on with the next file
    /// when one fails. They return the files that could not be written; the editor never prints.
    /// A file that does not exist throws FSException, and so does a reader that cannot work
    /// (MetadataHelperException).
    [[nodiscard]] std::vector<ExifWriteFailure> SetGPSAltitude(double altitude);
    [[nodiscard]] std::vector<ExifWriteFailure> SetGPSLatitude(double latitude);
    [[nodiscard]] std::vector<ExifWriteFailure> SetGPSLongitude(double longitude);
    [[nodiscard]] std::vector<ExifWriteFailure> SetGPS(double latitude, double longitude, double altitude);

  protected:
    // Sends one write request per file with the tags to set
    std::vector<ExifWriteFailure> writeTags(const exif::TagAssignments &set);

    const std::string doubleToDMS(double d);
    const std::string doubleToFraction(double d, int precision);
  };

}
#endif // EXIFEDITOR_H
