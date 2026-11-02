/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "exif_metadata.h"

#include <atomic>
#include <stdexcept>

#include "exif_helper_pool.h"
#include "gdal_open_policy.h"

namespace ddb
{
    namespace exif
    {

        bool Tag::ignoresIndex(std::string_view typeName)
        {
            return typeName == "XmpText" || typeName == "LangAlt" || typeName == "Date" || typeName == "Time";
        }

        Tag::Tag(std::string key, std::string typeName, size_t count, std::string value, bool truncated,
                 Components components)
            : key_(std::move(key)),
              typeName_(std::move(typeName)),
              count_(count),
              value_(std::move(value)),
              truncated_(truncated),
              ignoresIndex_(ignoresIndex(typeName_)),
              components_(std::move(components))
        {
            if (ignoresIndex_ && components_.size() > 1)
                components_.resize(1);
        }

        const Tag::Component &Tag::component(size_t index) const
        {
            if (ignoresIndex_)
                index = 0;
            if (index >= components_.size() || !components_[index])
                throw std::out_of_range("Tag " + key_ + " has no component " + std::to_string(index));
            return *components_[index];
        }

        ImageMetadata::ImageMetadata(int width, int height, std::vector<Tag> exif, std::vector<Tag> xmp)
            : width_(width), height_(height), exif_(std::move(exif)), xmp_(std::move(xmp))
        {
        }

        const Tag *ImageMetadata::find(const std::vector<Tag> &tags, std::string_view key)
        {
            for (const Tag &tag : tags)
                if (tag.key() == key)
                    return &tag;
            return nullptr;
        }

        ReadResult PendingRead::get()
        {
            if (!request_)
                throw std::logic_error("No metadata read is pending");
            // Taken out first: the read is over whatever get() does, a failure included
            const std::unique_ptr<Request> request = std::move(request_);
            return request->get();
        }

        namespace
        {
            // A read that only happens when its result is asked for
            class DeferredRead final : public PendingRead::Request
            {
              public:
                DeferredRead(IMetadataReader &reader, fs::path file) : reader_(reader), file_(std::move(file)) {}
                ReadResult get() override { return reader_.read(file_); }

              private:
                IMetadataReader &reader_;
                fs::path file_;
            };
        } // namespace

        PendingRead IMetadataReader::begin(const fs::path &file)
        {
            return PendingRead(std::make_unique<DeferredRead>(*this, file));
        }

        namespace
        {
            // The reader a ScopedMetadataReader installed, nullptr for the default
            std::atomic<IMetadataReader *> g_override{nullptr};
        } // namespace

        IMetadataReader &metadataReader()
        {
            // The tags are read by the host: the raster helper reads the file of its request with
            // GDAL only, gets what the tags say in the request, and its workers can start no
            // program (metadata-util included)
            if (gdalopen::userFileOpensEnabled())
                throw std::logic_error("metadataReader runs in the host: the raster helper reads no tags, the host "
                                       "reads them with metadata-util and sends what a request needs");
            if (IMetadataReader *reader = g_override.load(std::memory_order_acquire))
                return *reader;
            return HelperMetadataReader::instance();
        }

        ScopedMetadataReader::ScopedMetadataReader(IMetadataReader &reader)
            : previous_(g_override.exchange(&reader, std::memory_order_acq_rel))
        {
        }

        ScopedMetadataReader::~ScopedMetadataReader()
        {
            g_override.store(previous_, std::memory_order_release);
        }

        size_t closeMetadataHelpers()
        {
            return HelperMetadataReader::shutdownInstance();
        }

    } // namespace exif
} // namespace ddb
