/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
const path = require("path");

const bindings = (mod) => {
    let paths = [
        path.join(__dirname, '..', 'build'),
        path.join(__dirname, '..', 'build', 'Release'),
        path.join(__dirname, '..', '..', '..', 'build'),
    ];
    const tries = [];

    for (let i = 0; i < paths.length; i++){
        const attempt = path.join(paths[i], mod);

        try{
            return require(attempt);
        }catch(e){
            console.log(e);
            tries.push(attempt);
        }
    }

    throw new Error(`Cannot import ${mod}. Tried: ${JSON.stringify(tries)}`);
};
const n = bindings('node-ddb.node');
const ddb = require('./js/index');

ddb.registerNativeBindings(n);

module.exports = ddb;