/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
const entry = require('./entry');
const Tag = require('./tag');
const Dataset = require('./dataset');
const { parseUri } = require('./utils');

const ddb = {
    Tag, Dataset,
    entry,

    thumbs: {
        supportedForType: function(entryType) {
            entryType = parseInt(entryType);
            return entryType === ddb.entry.type.GEOIMAGE ||
                entryType === ddb.entry.type.GEORASTER ||
                entryType === ddb.entry.type.IMAGE;
        }
    },

    // Retrieves entry information from 
    // local or remote sources
    fetchEntries: async function(uri, options = {}){
        if (uri.startsWith("ddb://") || uri.startsWith("ddb+unsafe://")){
            const { registryUrl, organization, dataset, path } = parseUri(uri);
            
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

        this.info = async function(paths, options = {}) {
            return new Promise((resolve, reject) => {
                if (typeof paths === "string") paths = [paths];
        
                n.info(paths, options, (err, result) => {
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
                });
            });
        };

        this.add = async function(ddbPath, paths, options = {}) {
            return new Promise((resolve, reject) => {
                if (typeof paths === "string") paths = [paths];

                n.add(ddbPath, this._resolvePaths(ddbPath, paths), options, (err, entries) => {
                    if (err) reject(err);
                    else return resolve(entries);
                });
            });
        };

        this.list = async function(ddbPath, paths = ".", options = {}) {
            return new Promise((resolve, reject) => {
                const isSingle = typeof paths === "string";
                if (isSingle) paths = [paths];
        
                n.list(ddbPath, this._resolvePaths(ddbPath, paths), options, (err, result) => {
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
                n.remove(ddbPath, this._resolvePaths(ddbPath, paths), options, err => {
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

        // Guarantees that paths are expressed with
        // a ddbPath root or are absolute paths
        this._resolvePaths = function(ddbPath, paths){
            const path = require('path');
            
            return paths.map(p => {
                if (path.isAbsolute(p)) return p;

                const relative = path.relative(ddbPath, p);

                // Is it relative? Good
                if (relative && !relative.startsWith("..") && !path.isAbsolute(relative)) return p;
                
                // Combine
                else return path.join(ddbPath, p);
            });
        }
    }
};

module.exports = ddb;