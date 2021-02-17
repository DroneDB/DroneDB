/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

const localStorage = require('../polyfills/node/localStorage');
const fetch = require('../polyfills/node/fetch');
const FormData = require('../polyfills/node/FormData');
const Organization = require('./organization');
const { DEFAULT_REGISTRY } = require('./constants');

let refreshTimers = {};

module.exports = class Registry{
    constructor(url = "https://" + DEFAULT_REGISTRY){
        this.url = url;
        this.eventListeners = {};
    }
    
    get remote(){
        return this.url.replace(/^https?:\/\//, "");
    }

    get tagUrl(){
        // Drop the https prefix if it's secure (it's the default)
        return this.secure ? this.remote : this.url;
    }

    get secure(){
        return this.url.startsWith("https://");
    }

    // Login 
    async login(username, password, xAuthToken = null){
        const formData = new FormData();
        if (username) formData.append("username", username);
        if (password) formData.append("password", password);
        if (xAuthToken) formData.append("token", xAuthToken);

        try{
            const res = await fetch(`${this.url}/users/authenticate`, {
                method: 'POST',
                body: formData
            }).then(r => r.json());
            
            if (res.token){
                this.setCredentials(res.username, res.token, res.expires);
                this.setAutoRefreshToken();
                this.emit("login", res.username);

                return res;
            }else{
                throw new Error(res.error || `Cannot login: ${JSON.stringify(res)}`);
            }
        }catch(e){
            throw new Error(`Cannot login: ${e.message}`);
        }
    }

    async refreshToken(){
        if (this.isLoggedIn()){
            const res = await fetch(`${this.url}/users/authenticate/refresh`, {
                method: 'POST',
                headers: {
                    Authorization: `Bearer ${this.getAuthToken()}`
                }
            }).then(r => r.json());
            
            if (res.token){
                this.setCredentials(this.getUsername(), res.token, res.expires);
            }else{
                throw new Error(res.error || `Cannot refresh token: ${JSON.stringify(res)}`);
            }
        }else{
            throw new Error("logged out");
        }
    }

    setAutoRefreshToken(seconds = 3600){
        if (refreshTimers[this.url]){
            clearTimeout(refreshTimers[this.url]);
            delete refreshTimers[this.url];
        }

        setTimeout(async () => {
            try{
                await this.refreshToken();
                this.setAutoRefreshToken(seconds);
            }catch(e){
                console.error(e);
                
                // Try again later, unless we're logged out
                if (e.message !== "logged out"){
                    this.setAutoRefreshToken(seconds);
                }
            }
        }, seconds * 1000);
    }

    logout(){
        this.clearCredentials();
        this.emit("logout");
    }

    setCredentials(username, token, expires){
        localStorage.setItem(`${this.url}_username`, username);
        localStorage.setItem(`${this.url}_jwt_token`, token);
        localStorage.setItem(`${this.url}_jwt_token_expires`, expires);

        // Set cookie if the URL matches the current window
        if (typeof window !== "undefined"){
            if (window.location.origin === this.url){
                document.cookie = `jwtToken=${token};${expires*1000};path=/`;
            }
        }
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

         // Clear cookie if the needed
         if (typeof window !== "undefined"){
            if (window.location.origin === this.url){
                document.cookie = `jwtToken=;-1;path=/`;
            }
        }
    }

    isLoggedIn(){
        const loggedIn = this.getAuthToken() !== null && this.getAuthTokenExpiration() > new Date();
        if (!loggedIn) this.clearCredentials();
        return loggedIn;
    }

    async makeRequest(endpoint, method="GET", body = null){
        const headers = {};
        const authToken = this.getAuthToken();
        if (authToken) headers.Authorization = `Bearer ${authToken}`;
        const options = { 
            method,
            headers
        };

        if (body){
            const formData = new FormData();
            for(let k in body){
                if (Array.isArray(body[k])){
                    body[k].forEach(v => formData.append(k, v));
                }else{
                    formData.append(k, body[k]);
                }
            }
            options.body = formData;
        }

        const response = await fetch(`${this.url}${endpoint}`, options);
        if (response.status === 204) return true;
        else if (response.status === 401) throw new Error("Unauthorized");
        else{
            const contentType = response.headers.get("Content-Type");
            if (contentType && contentType.indexOf("application/json") !== -1){
                let json = await response.json();
                if (json.error) throw new Error(json.error);

                if (response.status === 200) return json;
                else throw new Error(`Server responded with: ${JSON.stringify(json)}`);
            }else if (contentType && contentType.indexOf("text/") !== -1){
                let text = await response.text();
                if (response.status === 200) return text;
                else throw new Error(`Server responded with: ${text}`);
            }else{
                throw new Error(`Server responded with: ${await response.text()}`);
            }
        }
    }

    async getRequest(endpoint){
        return this.makeRequest(endpoint, "GET");
    }

    async postRequest(endpoint, body = {}){
        return this.makeRequest(endpoint, "POST", body);
    }

    async deleteRequest(endpoint){
        return this.makeRequest(endpoint, "DELETE");
    }

    Organization(name){
        return new Organization(this, name);
    }

    addEventListener(event, cb){
        this.eventListeners[event] = this.eventListeners[event] || [];
        if (!this.eventListeners[event].find(e => e === cb)){
            this.eventListeners[event].push(cb);
        }
    }

    removeEventListener(event, cb){
        this.eventListeners[event] = this.eventListeners[event] || [];
        this.eventListeners[event] = this.eventListeners[event].filter(e => e !== cb);
    }

    emit(event, ...params){
        if (this.eventListeners[event]){
            this.eventListeners[event].forEach(listener => {
                listener(...params);
            });
        }
    }
}