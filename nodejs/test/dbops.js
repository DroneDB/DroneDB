/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
const ddb = require('../');
const assert = require('assert');
const { TestArea } = require('./utils');
const fs = require('fs');

describe('ddbops', function() {
    it('should be able to call init(), add() and remove()', async function() {
        this.timeout(4000);
        const t = new TestArea("init", true);
        const f = t.getFolder(".");
        const ddbPath = await ddb.init(f);
        assert.ok(fs.existsSync(ddbPath));
        assert.ok(ddbPath.indexOf(".ddb") !== -1);

        const imagePath = await t.downloadTestAsset("https://raw.githubusercontent.com/DroneDB/test_data/master/test-datasets/drone_dataset_brighton_beach/DJI_0018.JPG",
            "DJI_0018.JPG");

        const addRes = (await ddb.add(f, imagePath))[0];
        assert.ok(typeof addRes.hash === "string");
        assert.ok(typeof addRes.path === "string");
        assert.ok(typeof addRes.size === "number");

        assert.ok(await ddb.remove(f, imagePath));
    });
});