/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
const entry = require('./entry');

const ddb = {
    entry,

    thumbs: {
        supportedForType: function(entryType) {
            entryType = parseInt(entryType);
            return entryType === ddb.entry.type.GEOIMAGE ||
                entryType === ddb.entry.type.GEORASTER ||
                entryType === ddb.entry.type.IMAGE;
        }
    },

    tile: {}
};

module.exports = ddb;