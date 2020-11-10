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

    tile: {},

    registerNativeBindings: function(n){
        this.getVersion = n.getVersion;
        this.getDefaultRegistry = n.getDefaultRegistry;

        this.thumbs.getFromUserCache = async function(imagePath, modifiedTime, options = {}) {
            return new Promise((resolve, reject) => {
                n._thumbs_getFromUserCache(imagePath, modifiedTime, options, (err, result) => {
                    if (err) reject(err);
                    else resolve(result);
                });
            });
        };

        this.tile.getFromUserCache = async function(geotiffPath, tz, tx, ty, options = {}) {
            return new Promise((resolve, reject) => {
                n._tile_getFromUserCache(geotiffPath, tz, tx, ty, options, (err, result) => {
                    if (err) reject(err);
                    else resolve(result);
                });
            });
        };

        this.info = async function(files, options = {}) {
            return new Promise((resolve, reject) => {
                if (typeof files === "string") files = [files];
        
                n.info(files, options, (err, result) => {
                    if (err) reject(err);
                    else resolve(result);
                });
            });
        };

        this.init = async function(directory) {
            return new Promise((resolve, reject) => {
                n.init(directory, (err, result) => {
                    if (err) reject(err);
                    else resolve(result);
                })
            });
        };

        this.add = async function(ddbPath, paths, options = {}) {
            return new Promise((resolve, reject) => {
                if (typeof paths === "string") paths = [paths];
        
                n.add(ddbPath, paths, options, (err, entries) => {
                    if (err) reject(err);
                    else return resolve(entries);
                });
            });
        };

        this.list = async function(ddbPath, files = ".", options = {}) {
            const path = require('path');
            
            return new Promise((resolve, reject) => {
                const isSingle = typeof files === "string";
                if (isSingle) files = [files];
        
                n.list(ddbPath, files.map(f => path.join(ddbPath, f)), options, (err, result) => {
                    if (err) reject(err);
                    else {
                        resolve(result);
                    }
                });
            });
        };

        this.remove = async function(ddbPath, paths, options = {}) {
            return new Promise((resolve, reject) => {
                if (typeof paths === "string") paths = [paths];
                n.remove(ddbPath, paths, options, err => {
                    if (err) reject(err);
                    else resolve(true);
                });
            });
        };

        this.share = async function(paths, tag, options = {}, progress = () => true){
            return new Promise((resolve, reject) => {
                if (typeof paths === "string") paths = [paths];
                n.share(paths, tag, options, progress, (err, url) => {
                    if (err) reject(err);
                    else resolve(url);
                });
            });
        };

        this.login = async function(username, password, server = ""){
            return new Promise((resolve, reject) => {
                n.login(username, password, server, (err, token) => {
                    if (err) reject(err);
                    else resolve(token);
                });
            });
        };
    }
};

module.exports = ddb;