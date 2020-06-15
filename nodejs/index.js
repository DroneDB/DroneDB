/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
const n = require('bindings')('node-ddb.node');

const ddb = {
    getVersion: n.getVersion,

    thumbs:{
        supportedForType: function(entryType){
            entryType= parseInt(entryType);
            return entryType === ddb.entry.type.GEOIMAGE ||
                    entryType === ddb.entry.type.GEORASTER ||
                    entryType === ddb.entry.type.IMAGE; 
        },

        getFromUserCache: async function(imagePath, modifiedTime, options = {}){
            return new Promise((resolve, reject) => {
                n._thumbs_getFromUserCache(imagePath, modifiedTime, options, (err, result) => {
                    if (err) reject(err);
                    else resolve(result);
                });
            });
        }
    },

    entry: {
        type: require('./entryType'),
        typeToHuman: n.typeToHuman,
        hasGeometry: function(entry){
            if (!entry) return false;
            return !!entry.point_geom || !!entry.polygon_geom;
        },
        isDirectory: function(entry){
            if (!entry) return false;
            return entry.type === this.type.DIRECTORY ||
                   entry.type === this.type.DRONEDB;
        }
    },

    parseFiles: async function(files, options = {}){
        return new Promise((resolve, reject) => {
            if (typeof files === "string") files = [files];

            n.parseFiles(files, options, (err, result) => {
                if (err) reject(err);
                else resolve(result);
            });
        });
    },

    parseFile: async function(file, options = {}){
        const result = await this.parseFiles(file, options);
        if (result.length) return result[0];
    },
};

module.exports = ddb;