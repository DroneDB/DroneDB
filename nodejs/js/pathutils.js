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
    }
}