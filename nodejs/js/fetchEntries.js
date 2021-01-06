/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
const { parseUri, isDDBUri } = require('./utils');
const Registry = require('./registry');

// Retrieves entry information from 
// local or remote sources
async function fetchEntries(uri, options = {}){
    if (isDDBUri(uri)){
        const { registryUrl, org, ds, path } = parseUri(uri);
        const dataset = new Registry(registryUrl).Organization(org).Dataset(ds);
        return dataset.list(path);
    }else if (uri.startsWith("file://")){
        // Local file, use ddb.info (if available)
        if (this.info){
            return this.info(uri.substring("file://".length), options);
        }else{
            throw new Error("ddb.info is only available in NodeJS. Did you call registerNativeBindings?");
        }
    }else{
        throw new Error(`Unsupported URI: ${uri}`);
    }
};

module.exports = fetchEntries;