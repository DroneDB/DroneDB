/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

module.exports = {
    type: {
        UNDEFINED: 0,
        DIRECTORY: 1,
        GENERIC: 2,
        GEOIMAGE: 3,
        GEORASTER: 4,
        POINTCLOUD: 5,
        IMAGE: 6,
        DRONEDB: 7,
        MARKDOWN: 8,
        VIDEO: 9,
        GEOVIDEO: 10
    },

    typeToHuman: function(t){
        switch(t){
            case this.type.UNDEFINED:
                return "Undefined";
            case this.type.DIRECTORY:
                return "Directory";
            case this.type.GENERIC:
                return "Generic";
            case this.type.GEOIMAGE:
                return "GeoImage";
            case this.type.GEORASTER:
                return "GeoRaster";
            case this.type.POINTCLOUD:
                return "PointCloud";
            case this.type.IMAGE:
                return "Image";
            case this.type.DRONEDB:
                return "DroneDB";
            case this.type.MARKDOWN:
                return "Markdown";
            case this.type.VIDEO:
                return "Video";
            case this.type.GEOVIDEO:
                return "GeoVideo";
            default:
                return "?";
        }
    },

    hasGeometry: function(entry) {
        if (!entry) return false;
        return !!entry.point_geom || !!entry.polygon_geom;
    },
    
    isDirectory: function(entry) {
        if (!entry) return false;
        return entry.type === this.type.DIRECTORY ||
            entry.type === this.type.DRONEDB;
    }
};
