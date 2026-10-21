/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Image, GeoImage, Panorama, GeoPanorama, Video and GeoVideo entries: photos and videos read
// through their EXIF/XMP tags.

#include "entry.h"
#include "entry_handlers_builtin.h"

namespace ddb {

namespace {

class ImageEntryHandler : public EntryHandler {
  public:
    ImageEntryHandler(EntryType type, std::string name) : EntryHandler(type, std::move(name)) {}

    // Panoramas and videos have no thumbnail of their own
    ThumbnailSource thumbnailSource() const override {
        return type() == EntryType::Image || type() == EntryType::GeoImage ? ThumbnailSource::File
                                                                          : ThumbnailSource::None;
    }
};

std::string imageTypeName(EntryType type) {
    switch (type) {
        case EntryType::Image: return "Image";
        case EntryType::GeoImage: return "GeoImage";
        case EntryType::Panorama: return "Panorama";
        case EntryType::GeoPanorama: return "GeoPanorama";
        case EntryType::Video: return "Video";
        case EntryType::GeoVideo: return "GeoVideo";
        default: throw InvalidArgsException("Not an image or video type: " + std::to_string(type));
    }
}

}  // namespace

std::unique_ptr<EntryHandler> makeImageEntryHandler(EntryType type) {
    return std::make_unique<ImageEntryHandler>(type, imageTypeName(type));
}

}  // namespace ddb
