/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "request.h"

#include <fstream>

#include "exceptions.h"
#include "logger.h"
#include "mio.h"
#include "version.h"

namespace ddb::net {

Request::Request(const std::string &url, ReqType reqType)
    : url(url),
      reqType(reqType),
      curl(nullptr),
      headers(nullptr),
      form(nullptr),
      mime_data_carrier(nullptr),
      cb(nullptr) {
    try {
        curl = curl_easy_init();
        if (!curl) throw NetException("Cannot initialize CURL");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

        if (is_logger_verbose()) {
            curl_easy_setopt(curl, CURLOPT_VERBOSE, true);
        }

        if (reqType == HTTP_POST) {
            curl_easy_setopt(curl, CURLOPT_POST, true);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
        }

        curl_easy_setopt(curl, CURLOPT_USERAGENT, "dronedb-agent/" APP_VERSION);
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorMsg);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, true);

        fs::path caBundlePath = io::getDataPath("curl-ca-bundle.crt");
        if (!caBundlePath.empty()) {
            LOGD << "CA Bundle: " << caBundlePath.string();
            curl_easy_setopt(curl, CURLOPT_CAINFO,
                             caBundlePath.string().c_str());
        }
    } catch (AppException &) {
        if (curl) curl_easy_cleanup(curl);
        throw;
    }
}

Request::~Request() {
    if (curl) curl_easy_cleanup(curl);
    curl = nullptr;
    if (headers) curl_slist_free_all(headers);
    headers = nullptr;
    if (form) curl_mime_free(form);
    form = nullptr;
    if (mime_data_carrier) delete mime_data_carrier;
    mime_data_carrier = nullptr;
}

Request &Request::verifySSL(bool flag) {
    // VERIFYPEER basically makes sure the certificate itself is valid (i.e.,
    // signed by a trusted CA, the certificate chain is complete, etc).
    // VERIFYHOST checks that the host you're talking to is the host named in
    //	the certificate.
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, flag);

    return *this;
}

Request &Request::authToken(const std::string &token) {
    header("Authorization", "Bearer " + token);
    return *this;
}

Request &Request::authCookie(const std::string &token) {
    header("Cookie", "jwtToken=" + token);
    return *this;
}

Request &Request::progressCb(const RequestCallback &cb) {
    this->cb = cb;
    return *this;
}

Request &Request::maximumUploadSpeed(unsigned long bytesPerSec) {
    curl_easy_setopt(curl, CURLOPT_MAX_SEND_SPEED_LARGE,
                     static_cast<curl_off_t>(bytesPerSec));
    return *this;
}

std::string Request::urlEncode(const std::string &str) {
    char *encoded =
        curl_easy_escape(curl, str.c_str(), static_cast<int>(str.length()));
    if (!encoded) throw NetException("Cannot url encode " + str);
    std::string s(encoded);
    curl_free(encoded);
    return s;
}

Response Request::send() {
    Response res;

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Response::WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, static_cast<void *>(&res));

    perform(res);

    return res;
}

Request &Request::formData(std::vector<std::string> params) {
    if (params.size() % 2 != 0)
        throw NetException("Invalid number of formData parameters");

    std::stringstream ss;
    for (unsigned long i = 0; i < params.size(); i += 2) {
        ss << urlEncode(params[i]) << "=" << urlEncode(params[i + 1]);
        if (i + 1 < params.size()) ss << "&";
    }
    curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, ss.str().c_str());

    return *this;
}

Request &Request::multiPartFormData(std::vector<std::string> files,
                                    std::vector<std::string> params) {
    if (files.size() % 2 != 0)
        throw NetException("Invalid number of multiPartFormData files");
    if (params.size() % 2 != 0)
        throw NetException("Invalid number of multiPartFormData parameters");

    if (!form) form = curl_mime_init(curl);
    curl_mimepart *field = nullptr;

    // Expect: 100-continue is not wanted
    header("Expect:");

    // Add files
    for (unsigned long i = 0; i < files.size(); i += 2) {
        field = curl_mime_addpart(form);
        curl_mime_name(field, files[i].c_str());
        curl_mime_filedata(field, files[i + 1].c_str());
    }

    // Add parameters
    for (unsigned long i = 0; i < params.size(); i += 2) {
        field = curl_mime_addpart(form);
        curl_mime_name(field, params[i].c_str());
        curl_mime_data(field, params[i + 1].c_str(), CURL_ZERO_TERMINATED);
    }

    return *this;
}

DDB_DLL Request &Request::multiPartFormData(const std::string &filename,
                                            std::istream *stream, size_t offset,
                                            size_t size,
                                            std::vector<std::string> params) {
    if (params.size() % 2 != 0)
        throw NetException("Invalid number of multiPartFormData parameters");

    if (!form) form = curl_mime_init(curl);
    curl_mimepart *field = nullptr;

    // Expect: 100-continue is not wanted
    header("Expect:");

    field = curl_mime_addpart(form);

    LOGD << "curl_mime_addpart ok";

    curl_mime_filename(field, filename.c_str());
    curl_mime_name(field, filename.c_str());

    LOGD << "curl_mime_name ok";

    const curl_read_callback rcb = [](char *buffer, size_t size, size_t nitems,
                                      void *arg) {
        auto *p = static_cast<struct ctl *>(arg);

        size_t sz = p->size - p->position;

        LOGD << "curl_read_callback(" << size << ", " << nitems
             << ") cur = " << p->stream->tellg() << " reading " << sz
             << " data";

        nitems *= size;
        if (sz > nitems) sz = nitems;
        if (sz) {
            p->stream->seekg(p->position + p->offset, std::ios::beg);
            if (!p->stream->read(buffer, sz)) {
                throw FSException("Cannot read from stream");
            }
        }

        p->position += sz;

        return sz;
    };
    const curl_seek_callback scb = [](void *arg, curl_off_t offset,
                                      int origin) {
        struct ctl *p = (struct ctl *)arg;

        switch (origin) {
            case SEEK_END:
                offset += p->size;
                break;
            case SEEK_CUR:
                offset += p->position;
                break;
        }

        if (offset < 0) return CURL_SEEKFUNC_FAIL;
        p->position = offset;
        return CURL_SEEKFUNC_OK;
    };

    this->mime_data_carrier = new ctl;
    this->mime_data_carrier->size = size;
    this->mime_data_carrier->position = 0;
    this->mime_data_carrier->offset = offset;
    this->mime_data_carrier->stream = stream;

    LOGD << "Starting upload with total size " << size;

    curl_mime_data_cb(field, size, rcb, scb, nullptr, this->mime_data_carrier);

    LOGD << "curl_mime_data_cb ok";

    // Add parameters
    for (unsigned long i = 0; i < params.size(); i += 2) {
        field = curl_mime_addpart(form);
        curl_mime_name(field, params[i].c_str());
        curl_mime_data(field, params[i + 1].c_str(), CURL_ZERO_TERMINATED);
    }

    return *this;
}

Request &Request::header(const std::string &header) {
    headers = curl_slist_append(headers, header.c_str());
    return *this;
}

Request &Request::header(const std::string &name, const std::string &value) {
    header(name + ": " + value);
    return *this;
}

size_t write_data(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    const size_t written = fwrite(ptr, size, nmemb, stream);
    return written;
}

Response Request::downloadToFile(const std::string &outFile) {
    FILE *f = nullptr;

    f = fopen(outFile.c_str(), "wb");
    if (!f) throw FSException("Cannot open " + outFile + " for writing");

    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, false);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);

    Response res;
    perform(res);
    fclose(f);

    return res;
}

static int xferinfo(void *p, curl_off_t dltotal, curl_off_t dlnow,
                    curl_off_t ultotal, curl_off_t ulnow) {
    const auto progress = static_cast<struct RequestProgress *>(p);

    //  CURL *curl = static_cast<CURL *>(progress->curl);

    //  CURLINFO_TOTAL_TIME_T is Unavailable in old cURL versions
    //  curl_off_t curTime = 0;
    //  curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME_T, &curTime);

    //  if((curTime - progress->lastRuntime) >= 10000) {
    //    progress->lastRuntime = curTime;
    const size_t totalBytes = dltotal + ultotal;
    const size_t txBytes = dlnow + ulnow;

    // if (totalBytes > 0) {
    if (!(*progress->cb)(txBytes, totalBytes)) {
        return 1;
    }
    //}

    //}

    return 0;
}

void Request::perform(Response &res) {
    if (headers != nullptr) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    if (form != nullptr) {
        curl_easy_setopt(curl, CURLOPT_MIMEPOST, form);
    }

    if (cb != nullptr) {
        struct RequestProgress prog;
        prog.lastRuntime = 0;
        prog.curl = curl;
        prog.cb = &cb;
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferinfo);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &prog);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, false);
    }

    const CURLcode ret = curl_easy_perform(curl);
    if (ret != CURLE_OK) {
        const std::string err(curl_easy_strerror(ret));
        throw NetException(err + ": " + errorMsg);
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res.statusCode);
}

}  // namespace ddb::net
