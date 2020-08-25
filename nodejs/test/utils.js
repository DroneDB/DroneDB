/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
const fs = require('fs');
const path = require('path');
const http = require('https');
const os = require('os');

class TestArea{
    constructor(name, recreateIfExists = false){
        if (name.indexOf("..") !== -1) throw new Error("Cannot use .. in test area");
        this.name = name;

        const root = this.getFolder();
        if (recreateIfExists){
            if (fs.existsSync(root)){
                fs.rmdirSync(root, { recursive: true });
            }
        }
    }

    getFolder(subfolder){
        const root = path.join(os.tmpdir(), `ddb-tests-${this.name}`);
        let dir;
        if (subfolder){
            dir = path.join(root, subfolder);
        }else{
            dir = root;
        }
        if (!fs.existsSync(dir)){
            fs.mkdirSync(dir, { recursive: true});
        }
        return dir;
    }

    async downloadTestAsset(url, filename, overwrite = false){
        return new Promise((resolve, reject) => {
            const dest = path.join(this.getFolder(), filename);

            if (fs.existsSync(dest)){
                if (!overwrite){
                    resolve(dest);
                    return;
                }else fs.unlinkSync(dest);
            }

            const file = fs.createWriteStream(dest);
            file.on('finish', () => {
                file.close();
                resolve(dest);
            });
            http.get(url, function(response) {
                response.pipe(file);
            }).on('error', err => {
                fs.unlinkSync(dest);
                reject(err);
            });
        });
    }
};

function isPng(file){
    return fs.readFileSync(file).compare(Uint8Array.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]), 0, 7, 0, 7) === 0;
}

module.exports = {
    TestArea,
    isPng
};