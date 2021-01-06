/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
const entry = require('./entry');
const { parseUri, isDDBUri } = require('./utils');
const Registry = require('./registry');

module.exports = {
    supportedForType: function(entryType) {
        entryType = parseInt(entryType);
        return entryType === entry.type.GEOIMAGE ||
            entryType === entry.type.GEORASTER ||
            entryType === entry.type.IMAGE;
    },

    fetch: function(uri, thumbSize = 256){
        if (isDDBUri(uri)){
            const { registryUrl, org, ds, path } = parseUri(uri);
            const dataset = new Registry(registryUrl).Organization(org).Dataset(ds);
            return dataset.thumbUrl(path, thumbSize);
        }else if (uri.startsWith("file://")){
            // Local file, use getFromUserCache (if available)
            if (this.getFromUserCache){
                return this.getFromUserCache(uri.substring("file://".length), { thumbSize });
            }else{
                throw new Error("ddb.thumbs.getFromUserCache is only available in NodeJS. Did you call registerNativeBindings?");
            }
        }else{
            throw new Error(`Unsupported URI: ${uri}`);
        }
    }
}
