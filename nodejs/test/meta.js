/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
const ddb = require('../');
const assert = require('assert');
const { TestArea } = require('./helpers');
const fs = require('fs');
const path = require('path');

describe('meta', function() {
    it('should be able to call meta functions', async function() {
        this.timeout(8000);
        const t = new TestArea("meta", true);
        const f = t.getFolder(".");
        const ddbPath = await ddb.init(f);
        assert.ok(fs.existsSync(ddbPath));

        assert.ok(await ddb.meta.add(ddbPath, "", "pilots", {name: "test"}));
        const meta = await ddb.meta.get(ddbPath, "", "pilots");
        assert.ok(Array.isArray(meta));
        assert.ok(await ddb.meta.set(ddbPath, "", "test", true));
        assert.ok(await ddb.meta.unset(ddbPath, "", "test"));
        assert.ok(await ddb.meta.list(ddbPath, ""));
        assert.ok(await ddb.meta.remove(ddbPath, meta[0].id));
    });
});