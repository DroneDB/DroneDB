/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
 const LocalStorage = require('node-localstorage').LocalStorage;
 const home = require('os').homedir();
 const path = require('path');

module.exports = new LocalStorage(path.join(home, '.ddb', 'localStorage'));