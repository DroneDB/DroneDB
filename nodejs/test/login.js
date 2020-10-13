/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
const ddb = require('../');
const assert = require('assert');

describe('login', function() {
    it('should export a login method', async function() {
        assert.ok(ddb.login !== undefined);
    });

    // TODO:
    // testing share is a bit tricky, as you need to launch
    // MiniReg, run share, tear-down MiniReg
});