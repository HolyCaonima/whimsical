/* Gameplay owns intent/state; native movement supplies capsule collision and navigation. ES5. */
var Locomotion = {
    id : 0,
    marker : 0,
    facing : false,
    blockedTime : 0,
    velocity : {x : 0, z : 0},
    path : [],
    state : 'Idle',
    phase : 0,
    yaw : 0,
    interactionTime : 0,
    init : function(id) {
        this.id = id;
        this.settings = Engine.readJson('animations/locomotion.json');
    },
    command : function(target) {
        var route = Engine.findPath(this.id, target);
        if (!route.length) {
            Controller.message = 'Destination is blocked';
            return false;
        }
        this.path = route;
        Engine.showPath(route);
        this.interactionTime = 0;
        return true;
    },
    stop : function() {
        this.path = [];
        Engine.showPath([]);
        this.interactionTime = 0;
        this.facing = false;
    },
    prepare : function(input) {
        this.bodyHeight = Engine.characterHeight(this.id, input.keys[17] ? this.settings.crouchHeight : this.settings.standingHeight);
    },
    present : function(next, height, bob) {
        Engine.pose(this.id, next.x, next.y, next.z, this.yaw, 1);
        Engine.visualPose(this.id, {x : 0, y : bob, z : 0}, {x : 1, y : height, z : 1});
        if (this.marker)
            Engine.pose(this.marker, next.x + Math.sin(this.yaw) * .42, next.y + height * .36 + bob,
                        next.z + Math.cos(this.yaw) * .42, this.yaw, .14);
    },
    tick : function(dt, input) {
        var bodyHeight = this.bodyHeight;
        var crouched = bodyHeight < this.settings.standingHeight - .05;
        var p = Engine.position(this.id), s = this.settings, dx = 0, dz = 0, distance = 0, manual = false;
        if (this.interactionTime > 0) {
            this.interactionTime -= dt;
            this.velocity = {x : 0, z : 0};
            this.state = 'Interacting';
            this.present(p, bodyHeight * .5, 0);
            return;
        }
        if (Controller.selected && input.focused) {
            var x = (input.keys[68] ? 1 : 0) - (input.keys[65] ? 1 : 0),
                z = (input.keys[83] ? 1 : 0) - (input.keys[87] ? 1 : 0);
            if (x || z) {
                manual = true;
                this.path = [];
                Controller.pending = 0;
                Engine.showPath([]);
                dx = x * Math.cos(CameraRig.yaw) + z * Math.sin(CameraRig.yaw);
                dz = -x * Math.sin(CameraRig.yaw) + z * Math.cos(CameraRig.yaw);
            }
        }
        if (!manual && this.path.length) {
            dx = this.path[0].x - p.x;
            dz = this.path[0].z - p.z;
            distance = Math.sqrt(dx * dx + dz * dz);
            if (distance < s.arrivalRadius) {
                this.path.shift();
                Engine.showPath(this.path);
                if (this.path.length) {
                    dx = this.path[0].x - p.x;
                    dz = this.path[0].z - p.z;
                    distance = Math.sqrt(dx * dx + dz * dz);
                } else {
                    dx = 0;
                    dz = 0;
                }
            }
        }
        var length = Math.sqrt(dx * dx + dz * dz), speed = crouched   ? s.crouchSpeed
                                                           : input.keys[16] ? s.runSpeed
                                                                            : s.walkSpeed;
        if (!manual && this.path.length === 1)
            speed = Math.min(speed, Math.sqrt(2 * s.braking * Math.max(0, distance - .04)));
        var turn = 0;
        if (length > .001) {
            dx /= length;
            dz /= length;
            var desired = Math.atan2(dx, dz);
            turn = Math.atan2(Math.sin(desired - this.yaw), Math.cos(desired - this.yaw));
            this.yaw += Math.max(-s.turnRate * dt, Math.min(s.turnRate * dt, turn));
            speed *= Math.max(0, Math.cos(turn));
        } else
            speed = 0;
        var tx = dx * speed, tz = dz * speed, v = this.velocity,
            acc = (speed > Math.sqrt(v.x * v.x + v.z * v.z) ? s.acceleration : s.braking) * dt;
        var vx = tx - v.x, vz = tz - v.z, change = Math.sqrt(vx * vx + vz * vz);
        if (change > acc) {
            vx *= acc / change;
            vz *= acc / change;
        }
        v.x += vx;
        v.z += vz;
        var next = Engine.move(this.id, v.x * dt, v.z * dt);
        v.x = (next.x - p.x) / dt;
        v.z = (next.z - p.z) / dt;
        var actual = Math.sqrt(v.x * v.x + v.z * v.z);
        this.phase += actual * dt * 5;
        if (this.path.length && speed > .3 && actual < .05) {
            this.blockedTime += dt;
            if (this.blockedTime > .6) {
                var goal = this.path[this.path.length - 1];
                this.blockedTime = 0;
                if (!this.command(goal)) {
                    this.stop();
                    Controller.pending = 0;
                    Controller.message = 'Route is no longer reachable';
                }
            }
        } else
            this.blockedTime = 0;
        this.state = this.facing || (length > .01 && Math.abs(turn) > 1.0 && actual < .2) ? 'TurnInPlace'
                     : actual < .04                                                       ? (crouched ? 'Crouching' : 'Idle')
                     : speed < .05                                                        ? 'Stopping'
                     : crouched                                                           ? 'Crouching'
                     : actual < .6                                                        ? 'Starting'
                     : input.keys[16]                                                     ? 'Running'
                                                                                          : 'Walking';
        var height = bodyHeight * .5;
        var bob = actual > .1 ? Math.sin(this.phase) * .025 : 0;
        this.present(next, height, bob);
    }
};
