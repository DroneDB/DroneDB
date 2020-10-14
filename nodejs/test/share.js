/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
const ddb = require('../');
const assert = require('assert');

describe('share', function() {
    it('should export a share method', async function() {
        assert.ok(ddb.share !== undefined);
    });

    // TODO:
    // testing share is a bit tricky, as you need to launch
    // MiniReg, run share, tear-down MiniReg
});