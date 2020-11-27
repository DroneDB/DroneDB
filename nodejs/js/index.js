/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
const Tag = require('./tag');
const Dataset = require('./dataset');
const Registry = require('./registry');
const entry = require('./entry');
const thumbs = require('./thumbs');
const fetchEntries = require('./fetchEntries');
const utils = require('./utils');
const pathutils = require('./pathutils');

const ddb = {
    Tag, Dataset, Registry,
    entry, utils, pathutils,
    fetchEntries,
    thumbs,

    tile: {},

    registerNativeBindings: function(n){
        this.getVersion = n.getVersion;
        this.getDefaultRegistry = n.getDefaultRegistry;

        this.thumbs.getFromUserCache = async function(imagePath, options = {}) {
            return new Promise((resolve, reject) => {
                n._thumbs_getFromUserCache(imagePath, options, (err, result) => {
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

        this.chattr = async function(ddbPath, attrs = {}){
            return new Promise((resolve, reject) => {
                n.chattr(ddbPath, attrs, (err, attrs) => {
                    if (err) reject(err);
                    else resolve(attrs);
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