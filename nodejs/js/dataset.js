/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

const Tag = require('./tag');
const { DEFAULT_REGISTRY } = require('./constants');

 module.exports = class Dataset{
    constructor(tag){
        this.tag = new Tag(tag);
    }

    remoteUri(path){
        let remote = DEFAULT_REGISTRY;
        let secure = true;
        const { registryUrl, org, ds } = this.tag;

        if (registryUrl){
            if (registryUrl.startsWith("http://")) secure = false;
            remote = registryUrl.replace(/^https?:\/\//, "");
        }

        const proto = secure ? "ddb" : "ddb+unsafe";
        const p = path ? `/${path}` : "";
        
        return `${proto}://${remote}/${org}/${ds}${p}`;
    }
 };
