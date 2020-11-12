/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
const { parseUri } = require('../js/utils');
const assert = require('assert');

describe('utils', function() {
    it('should be able to parse ddb:// URIs', function() {
        assert.deepEqual(parseUri("ddb://hub.dronedb.app/test/abc/A.JPG"), {
            registryUrl: "https://hub.dronedb.app",
            org: "test",
            ds: "abc",
            path: "A.JPG"
        });

        assert.deepEqual(parseUri("ddb+unsafe://hub.dronedb.app/test/abc/A.JPG"), {
            registryUrl: "http://hub.dronedb.app",
            org: "test",
            ds: "abc",
            path: "A.JPG"
        });

        assert.deepEqual(parseUri("ddb://localhost/test/abc"), {
            registryUrl: "https://localhost",
            org: "test",
            ds: "abc",
            path: ""
        });

        assert.throws(() => parseUri("ddb://test/abc"));
        assert.throws(() => parseUri("ddb://test"));
    });
});