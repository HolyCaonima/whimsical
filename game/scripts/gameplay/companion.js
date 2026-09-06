/* A navigation-driven companion. Animation only receives intent; it does not own following policy. */
var Companion = {
    id : 0,
    target : 0,
    path : [],
    repath : 0,
    yaw : 0,
    speed : 0,
    lastGoal : null,
    init : function(id, target) {
        this.id = id;
        this.target = target;
        this.path = [];
        this.repath = 0;
        this.speed = 0;
    },
    tick : function(dt) {
        var p = Engine.position(this.id), leader = Engine.position(this.target);
        var dx = leader.x - p.x, dz = leader.z - p.z, distance = Math.sqrt(dx * dx + dz * dz);
        this.repath -= dt;
        // A trailing point leaves room for both collision capsules, even when the leader stops.
        var goal = {
            x : leader.x - Math.sin(leader.yaw) * 1.55,
            y : 0,
            z : leader.z - Math.cos(leader.yaw) * 1.55
        };
        if (distance < 1.3)
            this.path = [];
        else if (this.repath <= 0) {
            this.repath = .45;
            var moved = !this.lastGoal ||
                        Math.abs(goal.x - this.lastGoal.x) + Math.abs(goal.z - this.lastGoal.z) > .4;
            if (moved || !this.path.length) {
                var path = Engine.findPath(this.id, goal);
                if (path.length) {
                    this.path = path;
                    this.lastGoal = goal;
                }
            }
        }
        while (this.path.length) {
            dx = this.path[0].x - p.x;
            dz = this.path[0].z - p.z;
            if (dx * dx + dz * dz > .04)
                break;
            this.path.shift();
        }
        var desired = 0;
        if (this.path.length && distance > 1.3) {
            var length = Math.sqrt(dx * dx + dz * dz);
            dx /= length;
            dz /= length;
            var turn =
                Math.atan2(Math.sin(Math.atan2(dx, dz) - this.yaw), Math.cos(Math.atan2(dx, dz) - this.yaw));
            this.yaw += Math.max(-7 * dt, Math.min(7 * dt, turn));
            desired = Math.min(4.5, Math.max(.5, (distance - 1.1) * 1.5));
            if (this.path.length === 1)
                desired = Math.min(desired, length * 3);
            desired *= Math.max(0, Math.cos(turn));
        } else {
            dx = 0;
            dz = 0;
        }
        this.speed += Math.max(-9 * dt, Math.min(7 * dt, desired - this.speed));
        var next = Engine.move(this.id, dx * this.speed * dt, dz * this.speed * dt);
        var vx = (next.x - p.x) / dt, vz = (next.z - p.z) / dt, actual = Math.sqrt(vx * vx + vz * vz);
        Engine.pose(this.id, next.x, next.y, next.z, this.yaw, 1);
        Engine.animationInput(this.id, {
            action : actual < .06   ? 'Idle'
                     : actual < 1   ? 'Walk'
                     : actual < 2.6 ? 'Trot'
                                    : 'Canter',
            velocity : {x : vx, y : 0, z : vz},
            facing : {x : Math.sin(this.yaw), y : 0, z : Math.cos(this.yaw)}
        });
    }
};
