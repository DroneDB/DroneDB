const protoRegex = /^(file:\/\/|ddb:\/\/|ddb+unsafe:\/\/)/i;

module.exports = {
    basename: function(path){
        const pathWithoutProto = path.replace(protoRegex, "");
        const name = pathWithoutProto.split(/[\\/]/).pop();
        if (name) return name;
        else return pathWithoutProto;
    },

    join: function(...paths){
        return paths.map(p => {
            if (p[p.length - 1] === "/") return p.slice(0, p.length - 1);
            else return p;
        }).join("/");
    },

    getParentFolder: function(path) {
        if (typeof path === 'undefined' || path == null) 
            throw "Path is required";

        var idx = path.lastIndexOf('/');
        if (idx == -1) return null;

        return path.substr(0, idx);
    },

    getTree: function(path) {
        var folders = [];
        var f = path;
        do {
            folders.push(f);
            f = this.getParentFolder(f);
        } while(f != null);

        return folders.reverse();
    }
}