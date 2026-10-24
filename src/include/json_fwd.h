/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef JSON_FWD_H
#define JSON_FWD_H

// The name of the JSON type, without its definition (DQ-05): enough for a header that declares
// functions taking or returning a json, or holding a reference or a pointer to one. A header
// that needs the complete type (a json member, an inline body) and every .cpp include json.h.

#include <nlohmann/json_fwd.hpp>

using json = nlohmann::json;

#endif  // JSON_FWD_H
