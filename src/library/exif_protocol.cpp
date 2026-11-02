/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "exif_protocol.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <system_error>

#include "../../vendor/base64/base64.h"
#include "json.h"
#include "mio.h"

namespace ddb
{
    namespace exif
    {
        namespace protocol
        {

            namespace
            {
                // Well-formed UTF-8 as JSON needs it: no overlong form, no surrogate, nothing
                // above U+10FFFF
                bool isValidUtf8(std::string_view text)
                {
                    size_t i = 0;
                    const size_t n = text.size();
                    while (i < n)
                    {
                        const auto c = static_cast<unsigned char>(text[i]);
                        size_t length;
                        uint32_t code;
                        if (c < 0x80)
                        {
                            i++;
                            continue;
                        }
                        if (c >= 0xC2 && c <= 0xDF)
                        {
                            length = 2;
                            code = c & 0x1F;
                        }
                        else if (c >= 0xE0 && c <= 0xEF)
                        {
                            length = 3;
                            code = c & 0x0F;
                        }
                        else if (c >= 0xF0 && c <= 0xF4)
                        {
                            length = 4;
                            code = c & 0x07;
                        }
                        else
                            return false;
                        if (i + length > n)
                            return false;
                        for (size_t k = 1; k < length; k++)
                        {
                            const auto next = static_cast<unsigned char>(text[i + k]);
                            if ((next & 0xC0) != 0x80)
                                return false;
                            code = (code << 6) | (next & 0x3F);
                        }
                        if ((length == 3 && code < 0x800) || (length == 4 && code < 0x10000) || code > 0x10FFFF ||
                            (code >= 0xD800 && code <= 0xDFFF))
                            return false;
                        i += length;
                    }
                    return true;
                }

                // Sets `name` to the text when it is UTF-8, else `name`64 to its base64
                void setText(json &object, const char *name, std::string text)
                {
                    if (isValidUtf8(text))
                        object[name] = std::move(text);
                    else
                        object[std::string(name) + "64"] = Base64::encode(text);
                }

                // The path member of a request. A helper is a long-lived process that resolves a
                // relative path against its own working directory, the one the host had when it
                // started the helper; the caller means its working directory at the time of the
                // request, and a host may change it between reads (ddb sync, the bindings). So
                // the path goes out absolute, made so in the caller's process.
                void setPath(json &request, const fs::path &file)
                {
                    fs::path absolute = file;
                    if (!file.empty() && !file.is_absolute())
                    {
                        std::error_code ec;
                        absolute = fs::absolute(file, ec);
                        if (ec)
                            throw ProtocolError("Cannot make " + io::toUtf8(file) + " an absolute path: " + ec.message());
                    }
                    setText(request, "path", io::toUtf8(absolute));
                }

                bool isBase64(const std::string &text)
                {
                    if (text.size() % 4 != 0)
                        return false;
                    for (size_t i = 0; i < text.size(); i++)
                    {
                        const char c = text[i];
                        const bool digit = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                           (c >= '0' && c <= '9') || c == '+' || c == '/';
                        const bool padding = c == '=' && i + 2 >= text.size();
                        if (!digit && !padding)
                            return false;
                    }
                    return true;
                }

                // Builds a Response from the events of nlohmann's SAX parser, without a document
                // in between: a response of a photo has about 85 tags with 3 or more components
                // each, and the nodes of a DOM cost more than reading the text does (measured on
                // 18 KB responses: 165 us to build the document alone, 100 us to build the tags
                // directly).
                //
                // The parser is strict (text after the value, a control character, an invalid
                // escape, a lone surrogate or invalid UTF-8 in a string, a leading zero and a
                // number beyond the range of a double are errors) and iterative, so a deep
                // nesting costs no stack; the reader bounds it at kMaxDepth. It reads an integer
                // beyond 64 bits as a double and a number too small for a double as zero with
                // its sign.
                //
                // The reader keeps a stack of the containers it is in (the response, a list of
                // tags, a tag, the list of components, a component, a rational) and the name of
                // the current member of the object it is in, and slot() tells what the value
                // that arrives is for. Whatever the protocol does not define is skipped (a later
                // minor version may add members); whatever it defines and arrives in the wrong
                // shape fails the response.
                class ResponseReader final : public nlohmann::json_sax<json>
                {
                  public:
                    explicit ResponseReader(Operation operation) : operation_(operation) { stack_.reserve(kMaxDepth); }

                    // Why the response is not valid, once parsing returned false
                    const std::string &problem() const { return problem_; }

                    // The response, once parsing succeeded
                    // @throws ProtocolError when a member the response needs is missing
                    Response finish()
                    {
                        if (!haveId_)
                            invalid("no id");
                        if (!haveOk_)
                            invalid("no ok");
                        if (!response_.ok)
                        {
                            if (!haveError_)
                                invalid("a failure without an error");
                        }
                        else if (operation_ == Operation::Read)
                        {
                            if (!haveWidth_ || !haveHeight_)
                                invalid("a read response without width and height");
                            if (!haveExif_ || !haveXmp_)
                                invalid("a read response without exif and xmp");
                            response_.metadata = ImageMetadata(width_, height_, std::move(exif_), std::move(xmp_));
                        }
                        else
                        {
                            if (!haveTags_)
                                invalid("a write response without tags");
                            response_.tags = std::move(tags_);
                        }
                        return std::move(response_);
                    }

                    bool null() override
                    {
                        switch (slot())
                        {
                        case Slot::Id:
                            haveId_ = true;
                            return done();
                        case Slot::Ignore:
                            return done();
                        case Slot::Component:
                            tag_.components.push_back(std::nullopt);
                            return done();
                        default:
                            return fail("null where a value is expected");
                        }
                    }

                    bool boolean(bool value) override
                    {
                        switch (slot())
                        {
                        case Slot::Ok:
                            response_.ok = value;
                            haveOk_ = true;
                            return done();
                        case Slot::Truncated:
                            tag_.truncated = value;
                            return done();
                        case Slot::ComponentOk:
                            component_.ok = value;
                            return done();
                        case Slot::Ignore:
                            return done();
                        default:
                            return fail("a boolean where it does not belong");
                        }
                    }

                    bool number_integer(number_integer_t value) override { return integer(value); }

                    bool number_unsigned(number_unsigned_t value) override
                    {
                        // A member this client does not know takes any JSON value: the range
                        // matters only where the protocol defines an integer
                        if (slot() == Slot::Ignore)
                            return done();
                        if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                            return fail("an integer is out of range");
                        return integer(static_cast<int64_t>(value));
                    }

                    bool number_float(number_float_t value, const string_t & /*text*/) override
                    {
                        switch (slot())
                        {
                        case Slot::ComponentFloat:
                            component_.real = static_cast<float>(value);
                            return done();
                        case Slot::Ignore:
                            return done();
                        default:
                            return fail("a number with a fraction where it does not belong");
                        }
                    }

                    bool string(string_t &value) override
                    {
                        switch (slot())
                        {
                        case Slot::Error:
                            response_.error = std::move(value);
                            haveError_ = true;
                            return done();
                        case Slot::Message:
                            response_.message = std::move(value);
                            return done();
                        case Slot::Key:
                            tag_.key = std::move(value);
                            tag_.haveKey = true;
                            return done();
                        case Slot::Key64:
                            return decodeBase64(value, tag_.key, tag_.haveKey);
                        case Slot::Type:
                            tag_.type = std::move(value);
                            tag_.haveType = true;
                            return done();
                        case Slot::Text:
                            tag_.text = std::move(value);
                            tag_.haveText = true;
                            return done();
                        case Slot::Text64:
                            return decodeBase64(value, tag_.text, tag_.haveText);
                        case Slot::ComponentFloat:
                            return nonFinite(value);
                        case Slot::Ignore:
                            return done();
                        default:
                            return fail("a string where it does not belong");
                        }
                    }

                    // Only the binary formats nlohmann reads (CBOR, MessagePack...) have binary values
                    bool binary(binary_t & /*value*/) override { return fail("a binary value is not JSON"); }

                    bool start_object(std::size_t /*elements*/) override
                    {
                        if (stack_.empty())
                            return enter(Container::Root);
                        switch (slot())
                        {
                        case Slot::Tag:
                            tag_ = TagData();
                            return enter(Container::Tag);
                        case Slot::Ignore:
                            return enter(Container::Skipped);
                        default:
                            return fail("an object where it does not belong");
                        }
                    }

                    bool key(string_t &name) override
                    {
                        switch (stack_.back().container)
                        {
                        case Container::Root:
                            member_ = rootMember(name);
                            break;
                        case Container::Tag:
                            member_ = tagMember(name);
                            break;
                        default:
                            break;
                        }
                        return true;
                    }

                    bool end_object() override
                    {
                        const Container container = stack_.back().container;
                        stack_.pop_back();
                        if (container == Container::Tag && !finishTag())
                            return false;
                        return done();
                    }

                    bool start_array(std::size_t /*elements*/) override
                    {
                        if (stack_.empty())
                            return fail("not a JSON object");
                        switch (slot())
                        {
                        case Slot::ExifList:
                            tagsOut_ = &exif_;
                            haveExif_ = true;
                            // A photo has some 60 Exif and 25 XMP tags: room for them up front
                            // spares the moves of a growing list
                            exif_.reserve(64);
                            return enter(Container::Tags);
                        case Slot::XmpList:
                            tagsOut_ = &xmp_;
                            haveXmp_ = true;
                            xmp_.reserve(32);
                            return enter(Container::Tags);
                        case Slot::WriteTags:
                            tagsOut_ = &tags_;
                            haveTags_ = true;
                            return enter(Container::Tags);
                        case Slot::Components:
                            tag_.haveComponents = true;
                            // The helper sends max(min(n, 64), 3) entries (n comes first)
                            tag_.components.reserve(std::max<size_t>(3, std::min<size_t>(tag_.count, 64)));
                            return enter(Container::Components);
                        case Slot::Component:
                            component_ = Tag::Component();
                            return enter(Container::Component);
                        case Slot::ComponentRational:
                            return enter(Container::Rational);
                        case Slot::Ignore:
                            return enter(Container::Skipped);
                        default:
                            return fail("a list where it does not belong");
                        }
                    }

                    bool end_array() override
                    {
                        const Frame frame = stack_.back();
                        if (frame.container == Container::Component)
                        {
                            if (frame.position != 4)
                                return fail("a component is not [integer, float, [numerator, denominator], ok]");
                            tag_.components.push_back(component_);
                        }
                        else if (frame.container == Container::Rational && frame.position != 2)
                            return fail("a rational is not [numerator, denominator]");
                        stack_.pop_back();
                        return done();
                    }

                    // The text is not JSON. A reader that stopped the parse is not told.
                    bool parse_error(std::size_t /*position*/, const std::string & /*lastToken*/,
                                     const json::exception &error) override
                    {
                        return fail(std::string("not JSON: ") + error.what());
                    }

                  private:
                    // The responses nest six deep; more is a broken helper. The parser keeps its
                    // nesting on the heap, a bit a level; this bound stops a broken helper from
                    // growing the reader's stack without limit.
                    static constexpr size_t kMaxDepth = 64;

                    enum class Container
                    {
                        Root,       // the object of the response
                        Tags,       // a list of tags: exif, xmp or tags
                        Tag,        // a tag
                        Components, // the list v of a tag
                        Component,  // [integer, float, [numerator, denominator], ok]
                        Rational,   // [numerator, denominator]
                        Skipped     // something the protocol does not define
                    };

                    // The member the parser is at, in the response or in a tag
                    enum class Member
                    {
                        Other,
                        Id,
                        Ok,
                        Width,
                        Height,
                        Exif,
                        Xmp,
                        Tags,
                        Error,
                        Message,
                        Key,
                        Key64,
                        Type,
                        Count,
                        Text,
                        Text64,
                        Truncated,
                        Components
                    };

                    // What the value that arrives is for
                    enum class Slot
                    {
                        Invalid,
                        Ignore,
                        Id,
                        Ok,
                        Width,
                        Height,
                        ExifList,
                        XmpList,
                        WriteTags,
                        Error,
                        Message,
                        Tag,
                        Key,
                        Key64,
                        Type,
                        Count,
                        Text,
                        Text64,
                        Truncated,
                        Components,
                        Component,
                        ComponentInteger,
                        ComponentFloat,
                        ComponentRational,
                        ComponentOk,
                        Numerator,
                        Denominator
                    };

                    struct Frame
                    {
                        Container container;
                        // In a component or a rational: the values complete so far
                        size_t position = 0;
                    };

                    struct TagData
                    {
                        std::string key;
                        std::string type;
                        std::string text;
                        size_t count = 0;
                        bool truncated = false;
                        bool haveKey = false;
                        bool haveType = false;
                        bool haveCount = false;
                        bool haveText = false;
                        bool haveComponents = false;
                        Tag::Components components;
                    };

                    [[noreturn]] static void invalid(const char *what) { throw ProtocolError(what); }

                    static Member rootMember(std::string_view name)
                    {
                        if (name == "id")
                            return Member::Id;
                        if (name == "ok")
                            return Member::Ok;
                        if (name == "width")
                            return Member::Width;
                        if (name == "height")
                            return Member::Height;
                        if (name == "exif")
                            return Member::Exif;
                        if (name == "xmp")
                            return Member::Xmp;
                        if (name == "tags")
                            return Member::Tags;
                        if (name == "error")
                            return Member::Error;
                        if (name == "message")
                            return Member::Message;
                        return Member::Other;
                    }

                    static Member tagMember(std::string_view name)
                    {
                        if (name == "k")
                            return Member::Key;
                        if (name == "k64")
                            return Member::Key64;
                        if (name == "t")
                            return Member::Type;
                        if (name == "n")
                            return Member::Count;
                        if (name == "s")
                            return Member::Text;
                        if (name == "s64")
                            return Member::Text64;
                        if (name == "trunc")
                            return Member::Truncated;
                        if (name == "v")
                            return Member::Components;
                        return Member::Other;
                    }

                    Slot slot() const
                    {
                        if (stack_.empty())
                            return Slot::Invalid;
                        const Frame &frame = stack_.back();
                        switch (frame.container)
                        {
                        case Container::Root:
                            switch (member_)
                            {
                            case Member::Id:
                                return Slot::Id;
                            case Member::Ok:
                                return Slot::Ok;
                            case Member::Width:
                                return Slot::Width;
                            case Member::Height:
                                return Slot::Height;
                            case Member::Exif:
                                return Slot::ExifList;
                            case Member::Xmp:
                                return Slot::XmpList;
                            case Member::Tags:
                                return operation_ == Operation::Write ? Slot::WriteTags : Slot::Ignore;
                            case Member::Error:
                                return Slot::Error;
                            case Member::Message:
                                return Slot::Message;
                            default:
                                return Slot::Ignore;
                            }
                        case Container::Tags:
                            return Slot::Tag;
                        case Container::Tag:
                            switch (member_)
                            {
                            case Member::Key:
                                return Slot::Key;
                            case Member::Key64:
                                return Slot::Key64;
                            case Member::Type:
                                return Slot::Type;
                            case Member::Count:
                                return Slot::Count;
                            case Member::Text:
                                return Slot::Text;
                            case Member::Text64:
                                return Slot::Text64;
                            case Member::Truncated:
                                return Slot::Truncated;
                            case Member::Components:
                                // The tags of a write response have no components
                                return operation_ == Operation::Write ? Slot::Ignore : Slot::Components;
                            default:
                                return Slot::Ignore;
                            }
                        case Container::Components:
                            // The components of a type whose values ignore the index all answer
                            // like the first: that one is all that is kept
                            return tag_.haveType && Tag::ignoresIndex(tag_.type) && !tag_.components.empty()
                                       ? Slot::Ignore
                                       : Slot::Component;
                        case Container::Component:
                            switch (frame.position)
                            {
                            case 0:
                                return Slot::ComponentInteger;
                            case 1:
                                return Slot::ComponentFloat;
                            case 2:
                                return Slot::ComponentRational;
                            case 3:
                                return Slot::ComponentOk;
                            default:
                                return Slot::Invalid;
                            }
                        case Container::Rational:
                            if (frame.position == 0)
                                return Slot::Numerator;
                            return frame.position == 1 ? Slot::Denominator : Slot::Invalid;
                        case Container::Skipped:
                            return Slot::Ignore;
                        }
                        return Slot::Invalid;
                    }

                    bool enter(Container container)
                    {
                        if (stack_.size() >= kMaxDepth)
                            return fail("nested too deep");
                        stack_.push_back({container, 0});
                        return true;
                    }

                    // A value is complete: a component or a rational goes on to its next one
                    bool done()
                    {
                        if (!stack_.empty())
                        {
                            Frame &frame = stack_.back();
                            if (frame.container == Container::Component || frame.container == Container::Rational)
                                frame.position++;
                        }
                        return true;
                    }

                    bool integer(int64_t value)
                    {
                        switch (slot())
                        {
                        case Slot::Id:
                            response_.id = value;
                            haveId_ = true;
                            return done();
                        case Slot::Width:
                            return dimension(value, width_, haveWidth_);
                        case Slot::Height:
                            return dimension(value, height_, haveHeight_);
                        case Slot::Count:
                            if (value < 0)
                                return fail("a negative count");
                            tag_.count = static_cast<size_t>(value);
                            tag_.haveCount = true;
                            return done();
                        case Slot::ComponentInteger:
                            component_.integer = value;
                            return done();
                        case Slot::ComponentFloat:
                            component_.real = static_cast<float>(value);
                            return done();
                        case Slot::Numerator:
                            return rational(value, component_.rational.first);
                        case Slot::Denominator:
                            return rational(value, component_.rational.second);
                        case Slot::Ignore:
                            return done();
                        default:
                            return fail("an integer where it does not belong");
                        }
                    }

                    bool dimension(int64_t value, int &target, bool &have)
                    {
                        if (value < 0 || value > std::numeric_limits<int32_t>::max())
                            return fail("a width or height is out of range");
                        target = static_cast<int>(value);
                        have = true;
                        return done();
                    }

                    bool rational(int64_t value, int32_t &target)
                    {
                        if (value < std::numeric_limits<int32_t>::min() || value > std::numeric_limits<int32_t>::max())
                            return fail("a numerator or denominator is out of range");
                        target = static_cast<int32_t>(value);
                        return done();
                    }

                    // The strings that stand for the floats JSON has no number for
                    bool nonFinite(const std::string &text)
                    {
                        if (text == "NaN")
                            component_.real = std::numeric_limits<float>::quiet_NaN();
                        else if (text == "Infinity")
                            component_.real = std::numeric_limits<float>::infinity();
                        else if (text == "-Infinity")
                            component_.real = -std::numeric_limits<float>::infinity();
                        else
                            return fail("a component's float is not a number");
                        return done();
                    }

                    bool decodeBase64(const std::string &encoded, std::string &target, bool &have)
                    {
                        if (!isBase64(encoded))
                            return fail("a member that should be base64 is not");
                        target = Base64::decode(encoded);
                        have = true;
                        return done();
                    }

                    bool finishTag()
                    {
                        if (!tag_.haveKey)
                            return fail("a tag has no k");
                        if (!tag_.haveType)
                            return fail("a tag has no t");
                        if (!tag_.haveCount)
                            return fail("a tag has no n");
                        if (!tag_.truncated)
                        {
                            if (!tag_.haveText)
                                return fail("a tag has no s");
                            if (operation_ == Operation::Read && !tag_.haveComponents)
                                return fail("a tag has no v");
                        }
                        tagsOut_->emplace_back(std::move(tag_.key), std::move(tag_.type), tag_.count,
                                               std::move(tag_.text), tag_.truncated, std::move(tag_.components));
                        return true;
                    }

                    bool fail(std::string what)
                    {
                        if (problem_.empty())
                            problem_ = std::move(what);
                        return false;
                    }

                    const Operation operation_;
                    std::string problem_;
                    std::vector<Frame> stack_;
                    Member member_ = Member::Other;

                    Response response_;
                    bool haveId_ = false;
                    bool haveOk_ = false;
                    bool haveError_ = false;
                    bool haveWidth_ = false;
                    bool haveHeight_ = false;
                    bool haveExif_ = false;
                    bool haveXmp_ = false;
                    bool haveTags_ = false;
                    int width_ = 0;
                    int height_ = 0;
                    std::vector<Tag> exif_;
                    std::vector<Tag> xmp_;
                    std::vector<Tag> tags_;

                    // The list the tag being read goes to, the tag and its component
                    std::vector<Tag> *tagsOut_ = nullptr;
                    TagData tag_;
                    Tag::Component component_;
                };
            } // namespace

            std::optional<Version> parseVersionOutput(std::string_view output)
            {
                // The last line that has text
                while (!output.empty() && (output.back() == '\n' || output.back() == '\r' || output.back() == ' ' ||
                                           output.back() == '\t'))
                    output.remove_suffix(1);
                if (const size_t lineStart = output.find_last_of('\n'); lineStart != std::string_view::npos)
                    output.remove_prefix(lineStart + 1);

                constexpr std::string_view marker = "protocol ";
                const size_t at = output.rfind(marker);
                if (at == std::string_view::npos)
                    return std::nullopt;
                const std::string_view numbers = output.substr(at + marker.size());
                const size_t dot = numbers.find('.');
                if (dot == std::string_view::npos || dot == 0 || dot + 1 >= numbers.size())
                    return std::nullopt;

                const auto number = [](std::string_view digits) -> std::optional<int> {
                    if (digits.empty() || digits.size() > 6)
                        return std::nullopt;
                    int value = 0;
                    for (const char c : digits)
                    {
                        if (c < '0' || c > '9')
                            return std::nullopt;
                        value = value * 10 + (c - '0');
                    }
                    return value;
                };
                const auto major = number(numbers.substr(0, dot));
                const auto minor = number(numbers.substr(dot + 1));
                if (!major || !minor)
                    return std::nullopt;
                return Version{*major, *minor};
            }

            bool isFileError(std::string_view error)
            {
                return error == "open" || error == "exiv2";
            }

            bool isAccepted(const Version &helper)
            {
                return helper.major == kMajorVersion && helper.minor >= kMinorVersion;
            }

            std::string toString(const Version &version)
            {
                return std::to_string(version.major) + "." + std::to_string(version.minor);
            }

            std::string readRequest(int64_t id, const fs::path &file)
            {
                json request = {{"id", id}, {"op", "read"}};
                setPath(request, file);
                return request.dump();
            }

            std::string writeRequest(int64_t id, const fs::path &file, const TagAssignments &set)
            {
                json request = {{"id", id}, {"op", "write"}};
                setPath(request, file);
                json assignments = json::array();
                for (const auto &[key, value] : set)
                {
                    if (!isValidUtf8(key))
                        throw ProtocolError("The key " + Base64::encode(key) + " (base64) is not valid UTF-8");
                    json assignment = {{"key", key}};
                    setText(assignment, "value", value);
                    assignments.push_back(std::move(assignment));
                }
                request["set"] = std::move(assignments);
                return request.dump();
            }

            Response parseResponse(std::string_view line, Operation operation)
            {
                ResponseReader reader(operation);
                // Strict: nothing but white space after the value; no comments
                if (!json::sax_parse(line.data(), line.data() + line.size(), &reader, json::input_format_t::json,
                                     /*strict=*/true, /*ignore_comments=*/false))
                    throw ProtocolError(reader.problem().empty() ? std::string("not JSON") : reader.problem());
                return reader.finish();
            }

        } // namespace protocol
    } // namespace exif
} // namespace ddb
