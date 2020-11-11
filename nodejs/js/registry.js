/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

const localStorage = require('../polyfills/node/localStorage');
const fetch = require('../polyfills/node/fetch');

module.exports = class Registry{
    constructor(url){
        this.url = url;
    }

    // TODO: move login here
    // TODO: add refresh token endpoint

    setCredentials(username, token, expires){
        localStorage.setItem(`${this.url}_username`, username);
        localStorage.setItem(`${this.url}_jwt_token`, token);
        localStorage.setItem(`${this.url}_jwt_token_expires`, expires);
    }

    getAuthToken(){
        if (this.getAuthTokenExpiration() > new Date()){
            return localStorage.getItem(`${this.url}_jwt_token`);
        }
    }

    getUsername(){
        if (this.isLoggedIn()){
            return localStorage.getItem(`${this.url}_username`);
        }
    }

    getAuthTokenExpiration(){
        const expires = localStorage.getItem(`${this.url}_jwt_token_expires`);
        if (expires){
            return new Date(expires * 1000);
        }
    }

    clearCredentials(){
        localStorage.removeItem(`${this.url}_jwt_token`);
        localStorage.removeItem(`${this.url}_jwt_token_expires`);
        localStorage.removeItem(`${this.url}_username`);
    }

    isLoggedIn(){
        return this.getAuthToken() !== null && this.getAuthTokenExpiration() > new Date();
    }

    async list(organization, dataset, path){
        const headers = {};
        const authToken = this.getAuthToken();
        if (authToken) headers.Authorization = `Bearer ${authToken}`;

        const response = await fetch(`${this.url}/orgs/${organization}/ds/${dataset}/list`, { 
            method: "POST",
            body: { path },
            headers
        });
        if (response.status == 200) return response.json();
        else if (response.status == 401) throw new Error("Unauthorized");
        else throw new Error(`Server responded with: ${response.text()}`);
    }
}