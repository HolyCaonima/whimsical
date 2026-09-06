var CameraRig = {
    yaw : .72,
    pitch : .86,
    distance : 30,
    targetDistance : 30,
    x : 0,
    z : 0,
    follow : true,
    init : function(config) {
        this.yaw = config.yaw;
        this.pitch = config.pitch;
        this.distance = this.targetDistance = config.distance;
    },
    tick : function(dt, input) {
        if (input.pressed[70] || input.pressed[32])
            this.follow = true;
        if (input.keys[81])
            this.yaw += dt * 1.3;
        if (input.keys[82])
            this.yaw -= dt * 1.3;
        if (input.middle) {
            this.yaw -= input.dx * .006;
            this.pitch = Math.max(.48, Math.min(1.22, this.pitch + input.dy * .004));
        }
        this.targetDistance = Math.max(10, Math.min(44, this.targetDistance - input.wheel * 2.2));
        this.distance += (this.targetDistance - this.distance) * (1 - Math.exp(-dt * 12));
        var px = (input.keys[39] ? 1 : 0) - (input.keys[37] ? 1 : 0),
            pz = (input.keys[40] ? 1 : 0) - (input.keys[38] ? 1 : 0);
        // Edge pan is opt-in (hold Alt), avoiding accidental pan during character interactions.
        if (input.keys[18] && input.focused && !input.pointerCaptured) {
            px += (input.x > input.width - 15 ? 1 : 0) - (input.x < 15 ? 1 : 0);
            pz += (input.y > input.height - 15 ? 1 : 0) - (input.y < 15 ? 1 : 0);
        }
        if (px || pz) {
            this.follow = false;
            this.x += (px * Math.cos(this.yaw) + pz * Math.sin(this.yaw)) * dt * this.distance * .42;
            this.z += (-px * Math.sin(this.yaw) + pz * Math.cos(this.yaw)) * dt * this.distance * .42;
        }
        if (this.follow) {
            var p = Engine.position(Locomotion.id), k = 1 - Math.exp(-dt * 4);
            this.x += (p.x - this.x) * k;
            this.z += (p.z - this.z) * k;
        }
        this.x = Math.max(-11, Math.min(11, this.x));
        this.z = Math.max(-9, Math.min(9, this.z));
        Engine.camera(this.x, .5, this.z, this.yaw, this.pitch, this.distance);
    }
};
