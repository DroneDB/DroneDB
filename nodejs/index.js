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

    tile: {
        getFromUserCache: async function(geotiffPath, tz, tx, ty, options = {}){
            return new Promise((resolve, reject) => {
                n._tile_getFromUserCache(geotiffPath, tz, tx, ty, options, (err, result) => {
                    if (err) reject(err);
                    else resolve(result);
                });
            });
        }
    },


    info: async function(files, options = {}){
        return new Promise((resolve, reject) => {
            const isSingle = typeof files === "string"; 
            if (isSingle) files = [files];

            n.info(files, options, (err, result) => {
                if (err) reject(err);
                else{
                    // Return single item
                    if (isSingle) resolve(result[0]);

                    // Return entire array
                    else resolve(result);
                } 
            });
        });
    },

    init: async function(directory){
        return new Promise((resolve, reject) => {
            n.init(directory, (err, result) => {
                if (err) reject(err);
                else resolve(result);
            })
        });
    },

    add: async function(ddbPath, paths, options = {}){
        return new Promise((resolve, reject) => {
            if (typeof paths === "string") paths = [paths];
            n.add(ddbPath, paths, options, err => {
                if (err) reject(err);
                else resolve(true);                
            });
        });
    },

    remove: async function(ddbPath, paths, options = {}){
        return new Promise((resolve, reject) => {
            if (typeof paths === "string") paths = [paths];
            n.remove(ddbPath, paths, options, err => {
                if (err) reject(err);
                else resolve(true);     
            });
        });
    }
};

module.exports = ddb;