/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
const ddb = require('../');
const assert = require('assert');
const { TestArea, isPng } = require('./utils');
const fs = require('fs');

describe('functions', function() {
  it('should export a getVersion() method', function() {
    assert.equal(typeof ddb.getVersion(), "string");
    assert(ddb.getVersion().length > 0);
  });

  it('should export a info() method', function() {
    assert.equal(typeof ddb.info, "function");
  });

  it('should be able to call info without hash', async function(){
    const res = await ddb.info(__filename);
    assert.ok(res.path.length > 0);
    assert.equal(typeof res.hash, "undefined")
  });

  it('should be able to call info with hash', async function(){
    const res = await ddb.info([__filename, __dirname], {withHash: true});
    assert.equal(res.length, 2);

    // Files have hash calculated
    assert.equal(typeof res[0].hash, "string");
    assert(res[0].hash.length > 0);

    // Directories do not
    assert.equal(typeof res[1].hash, "undefined");
  });

  it('should fail when info is called on bad files', async function(){
    await assert.rejects(ddb.info("404", {withHash: true}));
  });

  it('should export a thumbs.getFromUserCache() method', function() {
    assert.equal(typeof ddb.thumbs.getFromUserCache, "function");
  });

  it('should fail when thumbs.getFromUserCache() is called on bad files', async function(){
    await assert.rejects(ddb.thumbs.getFromUserCache("nonexistant.jpg", 0, {thumbSize: 200}));
  });

  it('should be able to generate thumbnails', async function(){
    this.timeout(4000); 
    const t = new TestArea("thumbs");
    const geotiffPath = await t.downloadTestAsset("https://raw.githubusercontent.com/DroneDB/test_data/master/brighton/odm_orthophoto.tif", 
                              "ortho.tif");
    const thumb = await ddb.thumbs.getFromUserCache(geotiffPath, 0, {thumbSize: 256, forceRecreate: true});
    assert.ok(fs.existsSync(thumb));
  });

  it('should be able to generate tiles', async function(){
    this.timeout(4000);
    const t = new TestArea("tile");
    const geotiffPath = await t.downloadTestAsset("https://raw.githubusercontent.com/DroneDB/test_data/master/brighton/odm_orthophoto.tif", 
                              "ortho.tif");
    const tile = await ddb.tile.getFromUserCache(geotiffPath, 19, 128168, 339545, { forceRecreate: true });
    assert.ok(fs.existsSync(tile));
    assert.ok(isPng(tile));
  });

  it('should fail grecefully when tile.getFromUserCache() is called on invalid file', async function(){
    await assert.rejects(ddb.tile.getFromUserCache("nonexistant", 19, 128168, 339545));
  });
  
});
