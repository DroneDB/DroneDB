/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

module.exports = class Dataset{
    constructor(registry, org, ds){
        this.registry = registry;
        this.org = org;
        this.ds = ds;
    }

    remoteUri(path){
        const { remote, secure } = this.registry;

        const proto = secure ? "ddb" : "ddb+unsafe";
        const p = (path && path !== ".") ? `/${path}` : "";
        
        return `${proto}://${remote}/${this.org}/${this.ds}${p}`;
    }

    
    get baseApi(){
        return `/orgs/${this.org}/ds/${this.ds}`;
    }

    downloadUrl(paths){
        let url = `${this.baseApi}/download`;
        if (paths) url += `?path=${paths.join(",")}`;
        return url;
    }

    thumbUrl(path, size){
        let url = `${this.baseApi}/thumb?path=${path}`;
        if (size) url += `&size=${size}`;
        return url;
    }

    async download(paths){
        return this.registry.postRequest(`${this.baseApi}/download`, { path: paths });
    }
    
    async info(){
        return this.registry.getRequest(`${this.baseApi}`);
    }

    async list(path){
        return this.registry.postRequest(`${this.baseApi}/list`, { path });
    }

    async delete(){
        return this.registry.deleteRequest(`${this.baseApi}`);
    }

    async rename(slug){
        if (typeof slug !== "string") throw new Error(`Invalid slug ${slug}`);
        return this.registry.postRequest(`${this.baseApi}/rename`, { slug });
    }

    async setPublic(flag){
        return this.registry.postRequest(`${this.baseApi}/chattr`, { public: flag });
    }
 };
