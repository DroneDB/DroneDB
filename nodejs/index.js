const n = require('bindings')('node-ddb.node');

const ddb = {
    getVersion: n.getVersion,
    entryType: require('./entryType'),

    thumbs:{
        supportedForType: function(entryType){
            entryType = parseInt(entryType);
            return entryType === ddb.entryType.GEOIMAGE ||
                   entryType === ddb.entryType.GEORASTER ||
                   entryType === ddb.entryType.IMAGE; 
        },

        getFromUserCache: async (imagePath, modifiedTime, options = {}) => {
            return new Promise((resolve, reject) => {
                n._thumbs_getFromUserCache(imagePath, modifiedTime, options, (err, result) => {
                    if (err) reject(err);
                    else resolve(result);
                });
            });
        }
    },

    entry: {
        hasGeometry: function(entry){
            if (!entry) return false;
            return !!entry.point_geom || !!entry.polygon_geom;
        }
    },

    parseFiles: async (files, options = {}) => {
        return new Promise((resolve, reject) => {
            if (typeof files === "string") files = [files];

            n.parseFiles(files, options, (err, result) => {
                if (err) reject(err);
                else resolve(result);
            });
        });
    },
};

module.exports = ddb;