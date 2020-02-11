const n = require('bindings')('node-ddb.node');

module.exports = {
    getVersion: n.getVersion,
    entryType: require('./entryType'),

    parseFiles: async (files, options = {}) => {
        return new Promise((resolve, reject) => {
            if (typeof files === "string") files = [files];
            n.parseFiles(files, !!options.withHash, false, (err, result) => {
                if (err) reject(err);
                else resolve(result);
            });
        });
    }
};
