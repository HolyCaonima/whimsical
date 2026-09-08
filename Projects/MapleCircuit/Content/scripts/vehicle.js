// Project-owned arcade handling. All accepted movement and contact normals come from PhysicsScene.
var Vehicle = {
    create: function (index) {
        var car = {index: index, id: Engine.sceneObject('car' + index), wheels: [], lane: index % 2 ? 2.2 : -2.2};
        for (var w = 0; w < 4; w++) car.wheels.push(Engine.sceneObject('car' + index + '_wheel' + w));
        this.reset(car);
        return car;
    },
    reset: function (car) {
        var p = Track.at(Track.length - (2 + Math.floor(car.index / 2) * 4) * Track.step, car.lane);
        car.x = p.x; car.z = p.z; car.yaw = p.yaw;
        car.vx = 0; car.vz = 0; car.speed = 0; car.steer = 0; car.spin = 0;
        car.drifting = false; car.stuck = 0; car.contacts = 0; car.maxSpeed = 0; car.distanceDriven = 0;
        car.lap = 1; car.completed = 0; car.nextGate = 0; car.started = false;
        car.lapStart = 0; car.best = 0; car.lastLap = 0; car.finish = 0; car.gatesPassed = 0;
        car.track = Track.project(car.x, car.z);
        Engine.transform(car.id, {position: {x: car.x, y: .72, z: car.z}, rotation: MathUtil.rotation(car.yaw)});
        this.visuals(car);
    },
    recover: function (car) {
        var p = Track.at(car.track.s, car.lane * .5);
        car.x = p.x; car.z = p.z; car.yaw = p.yaw; car.vx = 0; car.vz = 0; car.speed = 0; car.stuck = 0;
        Engine.transform(car.id, {position: {x: car.x, y: .72, z: car.z}, rotation: MathUtil.rotation(car.yaw)});
        car.track = Track.project(car.x, car.z);
    },
    step: function (car, control, dt) {
        var speed = Math.sqrt(car.vx * car.vx + car.vz * car.vz);
        var fx = Math.sin(car.yaw), fz = Math.cos(car.yaw);
        var forward = car.vx * fx + car.vz * fz;
        car.steer += (control.steer - car.steer) * (1 - Math.exp(-10 * dt));
        car.drifting = control.drift && speed > 7;
        var steeringAngle = car.steer * .55 / (1 + speed * .038);
        var turn = forward / 2.65 * Math.tan(steeringAngle) * (car.drifting ? 1.32 : 1);
        var yaw = car.yaw + turn * dt;
        var nx = Math.sin(yaw), nz = Math.cos(yaw);
        var longitudinal = car.vx * nx + car.vz * nz;
        var lateral = car.vx * nz - car.vz * nx;
        var offroad = car.track.distance > Track.width;
        var acceleration = control.throttle * 15.5 - longitudinal * Math.abs(longitudinal) * .009;
        if (control.brake && longitudinal > 0) acceleration -= 30 * control.brake;
        if (Math.abs(longitudinal) > .1) acceleration -= (longitudinal > 0 ? 1 : -1) * (offroad ? 8 : .9);
        if (car.drifting) acceleration -= Math.max(longitudinal, 0) * .075;
        longitudinal = MathUtil.clamp(longitudinal + acceleration * dt, -9, offroad ? 18 : 38.1);
        if (control.brake && forward > 0 && longitudinal < 0) longitudinal = 0;
        lateral *= Math.exp(-(car.drifting ? 2.2 : offroad ? 5 : 11) * dt);
        car.vx = nx * longitudinal + nz * lateral;
        car.vz = nz * longitudinal - nx * lateral;
        var moved = Engine.moveBody(car.id, {x: car.vx * dt, y: 0, z: car.vz * dt}, MathUtil.rotation(yaw), 3);
        car.x = moved.position.x; car.z = moved.position.z;
        // Read the accepted quaternion: rotation may itself have been blocked by a wall.
        var q = moved.rotation;
        car.yaw = Math.atan2(2 * (q.w * q.y + q.x * q.z), 1 - 2 * (q.y * q.y + q.z * q.z));
        if (moved.blocked) {
            car.contacts += moved.contacts.length;
            for (var i = 0; i < moved.contacts.length; i++) {
                var n = moved.contacts[i].normal, into = car.vx * n.x + car.vz * n.z;
                if (into < 0) { car.vx -= into * n.x; car.vz -= into * n.z; }
            }
        }
        car.speed = Math.sqrt(car.vx * car.vx + car.vz * car.vz);
        car.maxSpeed = Math.max(car.maxSpeed, car.speed);
        car.distanceDriven += Math.sqrt(moved.applied.x * moved.applied.x + moved.applied.z * moved.applied.z);
        car.spin += longitudinal * dt / .43;
        car.track = Track.project(car.x, car.z);
        car.stuck = car.speed < 1 && control.throttle > .5 ? car.stuck + dt : 0;
        this.visuals(car);
    },
    visuals: function (car) {
        for (var w = 0; w < 4; w++) {
            var x = w % 2 ? 1.03 : -1.03, z = w < 2 ? 1.27 : -1.3;
            var a = w < 2 ? car.steer * .32 : 0, half = a / 2, spin = car.spin / 2;
            var rotation = {x: Math.cos(half) * Math.sin(spin), y: Math.sin(half) * Math.cos(spin),
                            z: -Math.sin(half) * Math.sin(spin), w: Math.cos(half) * Math.cos(spin)};
            Engine.localTransform(car.wheels[w], {position: {x:x, y:-.26, z:z}, rotation: rotation});
        }
    },
    ai: function (car, cars) {
        var look = 5.5 + car.speed * .43, lane = car.lane * .65;
        var throttle = 1, brake = 0;
        for (var i = 0; i < cars.length; i++) {
            if (cars[i] === car) continue;
            var other = cars[i], dx = other.x - car.x, dz = other.z - car.z;
            var ahead = dx * Math.sin(car.yaw) + dz * Math.cos(car.yaw);
            var side = dx * Math.cos(car.yaw) - dz * Math.sin(car.yaw);
            if (ahead > 0 && ahead < 12 && Math.abs(side) < 2.4) {
                lane = car.track.lane + (side >= 0 ? -2.4 : 2.4);
                lane = MathUtil.clamp(lane, -3.5, 3.5);
                if (ahead < 5.5 && car.speed > other.speed) { throttle = 0; brake = .5; }
            }
        }
        var target = Track.at(car.track.s + look, lane);
        var angle = MathUtil.angle(Math.atan2(target.x - car.x, target.z - car.z) - car.yaw);
        var steeringLimit = .55 / (1 + car.speed * .038);
        var steer = MathUtil.clamp(Math.atan(2 * 2.65 * Math.sin(angle) / look) / steeringLimit, -1, 1);
        var upcoming = Track.at(car.track.s + 20 + car.speed * .3, 0);
        var curvature = Math.abs(MathUtil.angle(upcoming.yaw - car.track.yaw));
        var desired = MathUtil.clamp(38 - curvature * 15, 16, 38) - car.index * .55;
        if (car.speed > desired) { throttle = 0; brake = Math.max(brake, MathUtil.clamp((car.speed - desired) / 7, 0, 1)); }
        if (car.stuck > 2.5 || car.track.distance > 21) Vehicle.recover(car);
        return {steer: steer, throttle: throttle, brake: brake, drift: false};
    }
};

var Effects = {
    initialize: function (data) {
        this.smoke = []; this.skids = []; this.nextSmoke = 0; this.nextSkid = 0; this.clock = 0; this.emitted = 0;
        for (var i = 0; i < data.smokeCount; i++) this.smoke.push({id: Engine.sceneObject('smoke' + i), life: 0});
        for (var j = 0; j < data.skidCount; j++) this.skids.push(Engine.sceneObject('skid' + j));
    },
    reset: function () {
        var i;
        for (i = 0; i < this.smoke.length; i++) { Engine.enabled(this.smoke[i].id, false); this.smoke[i].life = 0; }
        for (i = 0; i < this.skids.length; i++) Engine.enabled(this.skids[i], false);
        this.clock = 0;
    },
    update: function (car, dt) {
        this.clock -= dt;
        if (car.drifting && this.clock <= 0) {
            this.clock = .065;
            for (var side = -1; side <= 1; side += 2) {
                var x = car.x + Math.cos(car.yaw) * side * .86 - Math.sin(car.yaw) * 1.5;
                var z = car.z - Math.sin(car.yaw) * side * .86 - Math.cos(car.yaw) * 1.5;
                var p = this.smoke[this.nextSmoke++ % this.smoke.length];
                p.x = x; p.z = z; p.life = .9;
                Engine.enabled(p.id, true); this.emitted++;
                var skid = this.skids[this.nextSkid++ % this.skids.length];
                Engine.enabled(skid, true);
                Engine.transform(skid, {position: {x: x, y: .054, z: z}, rotation: MathUtil.rotation(Math.atan2(car.vx, car.vz))});
            }
        }
        for (var i = 0; i < this.smoke.length; i++) {
            var smoke = this.smoke[i];
            if (smoke.life <= 0) continue;
            smoke.life -= dt;
            if (smoke.life <= 0) { Engine.enabled(smoke.id, false); continue; }
            var age = .9 - smoke.life;
            var scale = (.25 + age * .85) * Math.min(1, smoke.life / .23);
            Engine.transform(smoke.id, {position: {x: smoke.x + age * .24, y: .3 + age * .7, z: smoke.z}, rotation: MathUtil.rotation(age)});
            Engine.scale(smoke.id, {x: scale, y: scale, z: scale});
        }
    }
};
