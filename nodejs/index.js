const n = require('bindings')('node-ddb.node');

module.exports = {
    getVersion: n.getVersion,
    entryType: require('./entryType'),

    parseFiles: async (files, options = {}) => {
        return new Promise((resolve, reject) => {
            if (typeof files === "string") files = [files];

            n.parseFiles(files, options, (err, result) => {
                if (err) reject(err);
                else resolve(result);
            });
        });
    },

    getThumbFromUserCache: async (imagePath, modifiedTime, options = {}) => {
        return new Promise((resolve, reject) => {
            n.getThumbFromUserCache(imagePath, modifiedTime, options, (err, result) => {
                if (err) reject(err);
                else resolve(result);
            });
        });
    }
};
