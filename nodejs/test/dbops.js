/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
const ddb = require('../');
const assert = require('assert');
const { TestArea, isPng } = require('./utils');
const fs = require('fs');

describe('ddbops', function() {
  it('should be able to call init()', async function() {
    const t = new TestArea("init", true);
    const f = t.getFolder(".");
    const ddbPath = await ddb.init(f);
    assert.ok(fs.existsSync(ddbPath));
    assert.ok(ddbPath.indexOf(".ddb") !== -1);
  });  
});
