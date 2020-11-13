/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

const { DEFAULT_REGISTRY } = require('./constants');

module.exports = class Tag {
    constructor(tag) {
        const parts = tag.split("/");

        if (parts.length === 2) {
            if (typeof window === "undefined" || window.location.hostname === DEFAULT_REGISTRY) {
                this.registryUrl = ""; // default
            } else {
                const proto = window.location.protocol === "https" ?
                              "" :
                              `${window.location.protocol}//`;
                const port = (window.location.port ? ":" + window.location.port : "");
                this.registryUrl = `${proto}${window.location.hostname}${port}`;
            }
            this.org = parts[0];
            this.ds = parts[1];
        } else if (parts.length === 3) {
            this.registryUrl = parts[0];
            this.org = parts[1];
            this.ds = parts[2];
        } else {
            throw new Error("Cannot parse tag: " + tag);
        }
    }
    toString() {
        if (this.registryUrl){
            return `${this.registryUrl}/${this.org}/${this.ds}`;
        }else{
            return `${this.org}/${this.ds}`;
        }
    }
}
