var Interactions = {
    objects : {},
    register : function(id, name, approach, action) {
        this.objects[id] = {name : name, approach : approach, action : action};
    },
    distance : function(a, b) {
        var x = a.x - b.x, z = a.z - b.z;
        return Math.sqrt(x * x + z * z);
    },
    nearest : function(p) {
        var best = 4, id = 0;
        for (var key in this.objects) {
            var d = this.distance(p, this.objects[key].approach);
            if (d < best) {
                best = d;
                id = Number(key);
            }
        }
        return id;
    },
    execute : function(id) {
        var item = this.objects[id];
        if (item) {
            item.action();
            Controller.message = item.name + ' activated';
        }
    }
};
