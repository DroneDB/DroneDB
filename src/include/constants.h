/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CONSTANTS_H
#define CONSTANTS_H

#define DDB_DATABASE_FILE "dbase.sqlite"
#define DDB_EMPTY_DATABASE_FILE "dbase.empty.sqlite"

#define DEFAULT_REGISTRY "hub.dronedb.app"
// OpenTopography global DEM. The API key is never stored in the source: it is read from
// DSM_SERVICE_KEY_ENV when a download is needed (see buildDsmServiceUrl in dsmservice.h)
#define DEFAULT_DSM_SERVICE_URL "https://portal.opentopography.org/API/globaldem?demtype=AW3D30&west={west}&south={south}&east={east}&north={north}&outputFormat=GTiff&API_Key={key}"
#define DSM_SERVICE_KEY_ENV "DDB_OPENTOPOGRAPHY_KEY"

#endif // CONSTANTS_H
