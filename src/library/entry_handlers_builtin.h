/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef ENTRY_HANDLERS_BUILTIN_H
#define ENTRY_HANDLERS_BUILTIN_H

// Private to the library: the factories of the built-in entry handlers, one per
// entry_handler_<type>.cpp, called once by registerBuiltinEntryHandlers()
// (entry_handler_registry.cpp). Not installed, not part of the API.

#include <memory>

#include "entry_handler.h"

namespace ddb {

// Image, GeoImage, Panorama, GeoPanorama, Video, GeoVideo (one EXIF parser). The Image
// handler classifies the EXIF/GDAL-sniffed extensions (jpg, tif, mp4, ...), GeoRaster included.
std::unique_ptr<EntryHandler> makeImageEntryHandler(EntryType type);
std::unique_ptr<EntryHandler> makeGeoRasterEntryHandler();
std::unique_ptr<EntryHandler> makePointCloudEntryHandler();
std::unique_ptr<EntryHandler> makeModelEntryHandler();
std::unique_ptr<EntryHandler> makeVectorEntryHandler();
std::unique_ptr<EntryHandler> makeGaussianSplatEntryHandler();
std::unique_ptr<EntryHandler> makeTiles3DEntryHandler();

}  // namespace ddb

#endif  // ENTRY_HANDLERS_BUILTIN_H
