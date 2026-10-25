/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef EXIF_METADATA_H
#define EXIF_METADATA_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fs.h"

namespace ddb
{
    // The image metadata libddb works with: EXIF and XMP tags, as plain data. They are read and
    // written by the metadata-util program (vendor/metadata-util, protocol in its PROTOCOL.md), which
    // libddb talks to through IMetadataReader; no library that parses image metadata is linked
    // into libddb. The tag model answers the questions the metadata libraries it replaces
    // answered, with the same results: the text of a tag, and the numeric value of each of its
    // components as an integer, a float or a rational.
    namespace exif
    {
        // numerator, denominator
        using Rational = std::pair<int32_t, int32_t>;

        // One tag, as the helper reported it.
        //
        // The value accessors take the index of a component (the 3 numbers of a GPS latitude)
        // and throw std::out_of_range when the tag has none at that index, the same way the
        // library behind the helper does. The helper reports the first components of a tag (at
        // least 3 and at most 64, PROTOCOL.md section 6), so a tag with more has none beyond. Text,
        // date, time and language-alternative values ignore the index: every index answers like
        // index 0.
        class Tag
        {
        public:
            // The numeric views of one component
            struct Component
            {
                int64_t integer = 0;
                float real = 0.0F;
                Rational rational{0, 1};
                // False when a conversion failed (a text that is not a number); the values are
                // then what the library behind the helper returned for it
                bool ok = false;
            };

            // A component the tag does not have is empty
            using Components = std::vector<std::optional<Component>>;

            // Whether the values of this type (the type name of the tag) answer the same for
            // every index: XMP text, language alternatives, dates and times
            static bool ignoresIndex(std::string_view typeName);

            Tag() = default;
            // `value` is the text of the tag (empty when `truncated`). `components` as the helper
            // sent them; of a tag whose components all answer like the first, only that one is
            // kept.
            Tag(std::string key, std::string typeName, size_t count, std::string value, bool truncated,
                Components components);

            // For example "Exif.GPSInfo.GPSLatitude" or "Xmp.drone-dji.RelativeAltitude"
            const std::string &key() const { return key_; }
            // For example "Rational", "Ascii", "XmpText"
            const std::string &typeName() const { return typeName_; }
            // The number of components
            size_t count() const { return count_; }

            // The text of the tag. Empty when truncated(): the helper does not send values longer
            // than 65 536 bytes.
            const std::string &toString() const { return value_; }
            bool truncated() const { return truncated_; }

            int64_t toInt64(size_t index = 0) const { return component(index).integer; }
            float toFloat(size_t index = 0) const { return component(index).real; }
            Rational toRational(size_t index = 0) const { return component(index).rational; }
            // Whether the conversions of component `index` succeeded
            bool componentOk(size_t index = 0) const { return component(index).ok; }

        private:
            // @throws std::out_of_range
            const Component &component(size_t index) const;

            std::string key_;
            std::string typeName_;
            size_t count_ = 0;
            std::string value_;
            bool truncated_ = false;
            // Every index answers like index 0 (the components of a text value)
            bool ignoresIndex_ = false;
            Components components_;
        };

        // What one read of a file returns. Lookups return the first tag with the key, in the order
        // the file holds them, or nullptr.
        class ImageMetadata
        {
        public:
            ImageMetadata() = default;
            ImageMetadata(int width, int height, std::vector<Tag> exif, std::vector<Tag> xmp);

            // The pixel size the image format declares; 0 where it does not say
            int width() const { return width_; }
            int height() const { return height_; }

            const std::vector<Tag> &exif() const { return exif_; }
            const std::vector<Tag> &xmp() const { return xmp_; }
            bool hasExif() const { return !exif_.empty(); }
            bool hasXmp() const { return !xmp_.empty(); }

            const Tag *findExif(std::string_view key) const { return find(exif_, key); }
            const Tag *findXmp(std::string_view key) const { return find(xmp_, key); }

        private:
            // A scan: a file has about a hundred tags, and a lookup compares lengths first
            static const Tag *find(const std::vector<Tag> &tags, std::string_view key);

            int width_ = 0;
            int height_ = 0;
            std::vector<Tag> exif_;
            std::vector<Tag> xmp_;
        };

        struct ReadResult
        {
            // Empty when the file has no metadata the reader can give: it is not an image, it
            // cannot be opened, its metadata is damaged. That is not an error of the index: the
            // entry is indexed without EXIF.
            std::optional<ImageMetadata> metadata;
            // Why there is no metadata, for the DEBUG log; empty otherwise
            std::string message;
        };

        // A read that was started (IMetadataReader::begin) and whose result was not taken yet,
        // so the caller can work on the file while the reader reads it. Move-only.
        //
        // A PendingRead dropped without get() (the work in between failed or was canceled)
        // abandons the read: the reader leaves nothing of it behind. In particular the helper
        // that serves it never goes back to the pool with its response unread, where the next
        // caller would take that response for its own.
        class PendingRead
        {
          public:
            // What a reader keeps of a read it started
            class Request
            {
              public:
                virtual ~Request() = default;
                // The result; called at most once
                virtual ReadResult get() = 0;
            };

            PendingRead() = default;
            explicit PendingRead(std::unique_ptr<Request> request) : request_(std::move(request)) {}
            PendingRead(PendingRead &&) noexcept = default;
            // Abandons the read this one held
            PendingRead &operator=(PendingRead &&) noexcept = default;
            PendingRead(const PendingRead &) = delete;
            PendingRead &operator=(const PendingRead &) = delete;

            // A read was started and its result not taken
            bool valid() const { return request_ != nullptr; }

            // What read() of the file returns, and throws what read() throws. Once: the
            // PendingRead is empty afterwards, whatever happened.
            // @throws std::logic_error when it is empty
            ReadResult get();

          private:
            std::unique_ptr<Request> request_;
        };

        // The EXIF tags to set, in order: (key, value), the value in the text form of the tag
        // ("46/1 4/1 12000/10000" for a rational, "N" for a reference)
        using TagAssignments = std::vector<std::pair<std::string, std::string>>;

        struct WriteResult
        {
            bool ok = false;
            // Why the write failed; empty when it succeeded
            std::string message;
            // The tags that were set, as the file now holds them (no components)
            std::vector<Tag> tags;
        };

        // Reads and writes the metadata of image files. Thread-safe.
        //
        // Both throw MetadataHelperException when the reader cannot work at all (metadata-util is
        // missing, does not start or speaks another protocol): that is not a problem of the file,
        // and a batch that meets it stops. read() throws IndexException when the helper breaks
        // while it serves the file (it dies, hangs, answers nonsense): an error of that item
        // only, the next call gets a fresh helper. Both throw CanceledException when the work of
        // the calling thread is canceled.
        class IMetadataReader
        {
        public:
            virtual ~IMetadataReader() = default;

            virtual ReadResult read(const fs::path &file) = 0;

            // Starts reading `file` and returns without waiting for the result, which get() of
            // the PendingRead gives (it is what read() gives), nor for the reader: a read that
            // cannot start at once (every helper is busy) is made in get(). It does not throw
            // what read() throws: get() does, so what the caller does in between fails first, as
            // it would before a read(). One pending read per thread: a thread that holds one makes no
            // other read until it took or dropped it, since a read may hold the last helper.
            // The default starts nothing and reads in get(), for readers that have nothing to
            // overlap (the substitutes of the tests).
            virtual PendingRead begin(const fs::path &file);

            // Sets the tags in order and writes the file once. Failure of the file or of a value
            // (including a helper that broke while it served the request) is a result, not an
            // exception: the caller decides how to report it.
            virtual WriteResult write(const fs::path &file, const TagAssignments &set) = 0;
        };

        // The reader of the process: the one every metadata read of libddb goes through. By
        // default a pool of metadata-util processes (exif_helper_pool.h).
        IMetadataReader &metadataReader();

        // Replaces the reader of the process while it lives, then puts the previous one back (the
        // seam of the tests, which substitute a reader of their own). Not for use while other
        // threads are reading: they would get either reader.
        class ScopedMetadataReader
        {
        public:
            explicit ScopedMetadataReader(IMetadataReader &reader);
            ~ScopedMetadataReader();
            ScopedMetadataReader(const ScopedMetadataReader &) = delete;
            ScopedMetadataReader &operator=(const ScopedMetadataReader &) = delete;

        private:
            IMetadataReader *previous_;
        };

        // Stops the metadata-util processes of the default reader that are idle, if it was ever used:
        // close their input, wait at most 2 s together, then kill the rest. The reader works
        // again afterwards (it starts new helpers on demand). Helpers that are serving a request
        // finish it and are closed instead of kept. Returns the number of helpers closed.
        size_t closeMetadataHelpers();

    } // namespace exif
} // namespace ddb

#endif // EXIF_METADATA_H
