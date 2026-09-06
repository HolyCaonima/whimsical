/* A navigation-driven companion. Animation only receives intent; it does not own following policy. */
var Companion = {
    id : 0,
    target : 0,
    path : [],
    repath : 0,
    speed : 0,
    lastGoal : null,
    init : function(id, target) {
        this.id = id;
        this.target = target;
        this.path = [];
        this.repath = 0;
        this.speed = 0;
        this.lastGoal = null;
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
            desired = Math.min(4.5, Math.max(.5, (distance - 1.1) * 1.5));
            if (this.path.length === 1)
                desired = Math.min(desired, length * 3);
        } else {
            dx = 0;
            dz = 0;
        }
        this.speed += Math.max(-9 * dt, Math.min(7 * dt, desired - this.speed));
        // The solver predicts the turn and its footwork together. Moving/rotating the
        // capsule here would discard that root motion and rebase the pose at each repath.
        var moving = this.path.length && distance > 1.3;
        var speed = moving ? this.speed : 0;
        Engine.animationInput(this.id, {
            action : speed < .1    ? 'Idle'
                     : speed < 1.2 ? 'Walk'
                     : speed < 2   ? 'Pace'
                     : speed < 4   ? 'Trot'
                                   : 'Canter',
            velocity : {x : dx * speed, y : 0, z : dz * speed},
            facing : moving ? {x : dx, y : 0, z : dz} : {x : 0, y : 0, z : 0}
        });
    }
};
