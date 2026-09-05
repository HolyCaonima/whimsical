var Controller = {
    selected : true,
    pending : 0,
    message : 'Click to walk. Explore the court.',
    tick : function(dt, input) {
        Locomotion.facing = false;
        if (input.pressed[27]) {
            Locomotion.stop();
            this.pending = 0;
            this.message = 'Order cancelled';
        }
        if (input.pressed[9]) {
            this.selected = !this.selected;
            Engine.select(this.selected ? Locomotion.id : 0);
            if (!this.selected)
                Locomotion.stop();
        }
        if (input.leftPressed || input.rightPressed) {
            if (input.picked === Locomotion.id) {
                this.selected = true;
                Engine.select(Locomotion.id);
            } else if (this.selected) {
                this.pending = 0;
                if (input.picked && Interactions.objects[input.picked])
                    this.interact(input.picked);
                else if (input.groundValid)
                    Locomotion.command({x : input.groundX, y : input.groundY, z : input.groundZ});
            }
        }
        if (input.pressed[69] && this.selected) {
            var id = Interactions.nearest(Engine.position(Locomotion.id));
            if (id)
                this.interact(id);
        }
        if (this.pending && Locomotion.path.length === 0) {
            var p = Engine.position(Locomotion.id), item = Interactions.objects[this.pending];
            if (Interactions.distance(p, item.approach) < .65) {
                var target = Engine.position(this.pending),
                    desired = Math.atan2(target.x - p.x, target.z - p.z),
                    angle =
                        Math.atan2(Math.sin(desired - Locomotion.yaw), Math.cos(desired - Locomotion.yaw));
                if (Math.abs(angle) > .12) {
                    Locomotion.facing = true;
                    Locomotion.yaw += Math.max(-6 * dt, Math.min(6 * dt, angle));
                } else {
                    Interactions.execute(this.pending);
                    Locomotion.interactionTime = .75;
                    this.pending = 0;
                }
            }
        }
        Engine.status(Locomotion.state, this.message);
    },
    interact : function(id) {
        var item = Interactions.objects[id];
        if (!item)
            return;
        if (Locomotion.command(item.approach)) {
            this.pending = id;
            this.message = 'Approaching ' + item.name;
        }
    }
};
