var Race = {
    initialize: function (data) {
        this.laps = data.laps; this.verification = !!data.verification;
        this.cars = []; this.finishOrder = [];
        for (var i = 0; i < 4; i++) this.cars.push(Vehicle.create(i));
        this.reset(!this.verification);
    },
    reset: function (ready) {
        this.ready = !!ready;
        this.elapsed = 0; this.countdown = 3; this.paused = false; this.finished = false;
        this.autopilot = this.verification; this.finishOrder = []; this.verified = false;
        this.message = ''; this.messageTime = 0;
        for (var i = 0; i < this.cars.length; i++) Vehicle.reset(this.cars[i]);
        if (Effects.smoke) Effects.reset();
        if (CameraRig.ready) CameraRig.reset(this.cars[0]);
    },
    checkpoint: function (car, previous) {
        if (car.finish || car.track.distance > Track.width + 1.5) return;
        var gate = car.nextGate * Track.length / 16;
        if (!Track.crossed(previous, car.track.s, gate)) return;
        car.gatesPassed++;
        if (car.nextGate === 0) {
            if (car.started) {
                car.completed++;
                var lapTime = this.elapsed - car.lapStart;
                car.lastLap = lapTime;
                if (!car.best || lapTime < car.best) car.best = lapTime;
                if (car.index === 0) { this.message = 'LAP ' + car.completed + '  /  ' + Hud.time(lapTime); this.messageTime = 2.5; }
                if (car.completed === this.laps) {
                    car.finish = this.elapsed; this.finishOrder.push(car.index);
                    Engine.log('MAPLE_FINISH ' + JSON.stringify({car: car.index, time: car.finish, gates: car.gatesPassed,
                        maxKmh: car.maxSpeed * 3.6, distance: car.distanceDriven, contacts: car.contacts}));
                    if (car.index === 0) this.finished = true;
                } else car.lap++;
            } else car.started = true;
            car.lapStart = this.elapsed;
        }
        car.nextGate = (car.nextGate + 1) % 16;
    },
    order: function () {
        var cars = this.cars.slice(0);
        cars.sort(function (a, b) {
            if (a.finish && b.finish) return a.finish - b.finish;
            if (a.finish) return -1;
            if (b.finish) return 1;
            // Only validated sectors contribute to ranking; shortcuts cannot jump progress.
            function progress(c) {
                var gate = (c.nextGate + 15) % 16;
                return c.gatesPassed * Track.length / 16 + Math.min(Track.length / 16,
                    MathUtil.wrap(c.track.s - gate * Track.length / 16, Track.length));
            }
            return progress(b) - progress(a);
        });
        return cars;
    },
    update: function (dt, input) {
        if (this.paused || this.ready) return;
        if (this.countdown > 0) { this.countdown = Math.max(0, this.countdown - dt); return; }
        this.elapsed += dt; this.messageTime -= dt;
        for (var i = 0; i < this.cars.length; i++) {
            var car = this.cars[i], control, previous = car.track.s;
            if (i > 0 || this.autopilot || car.finish) control = Vehicle.ai(car, this.cars);
            else {
                var keys = input.keyboardCaptured ? [] : input.keys;
                var backward = keys[83] || keys[40];
                var forward = car.vx * Math.sin(car.yaw) + car.vz * Math.cos(car.yaw);
                control = {throttle: (keys[87] || keys[38]) ? 1 : backward && forward < .7 ? -1 : 0,
                    brake: backward && forward >= .7 ? 1 : 0,
                    steer: (keys[65] || keys[37] ? 1 : 0) - (keys[68] || keys[39] ? 1 : 0), drift: !!keys[32]};
                // With local forward +Z, positive yaw is screen-left in the chase camera.
            }
            // Finishers keep circulating at cooldown speed so they do not block the finish line.
            if (car.finish && car.speed > 16) { control.throttle = 0; control.brake = .5; }
            Vehicle.step(car, control, dt);
            this.checkpoint(car, previous);
        }
        Effects.update(this.cars[0], dt);
        if (this.verification && !this.verified) {
            if (this.finishOrder.length === 4) {
                this.verified = true;
                for (var j = 0; j < 4; j++) {
                    var c = this.cars[j];
                    if (c.gatesPassed !== 49 || c.distanceDriven < Track.length * 2.85 || c.maxSpeed < 25)
                        throw new Error('Native race verification failed for car ' + j);
                }
                Engine.log('MAPLE_NATIVE_RACE_PASS ' + JSON.stringify({elapsed: this.elapsed, order: this.finishOrder}));
            }
            if (this.elapsed > 150 && !this.verified) throw new Error('Native race timed out: ' + JSON.stringify(this.cars.map(function(c) {
                return {index:c.index, completed:c.completed, gate:c.nextGate, s:c.track.s, distance:c.track.distance, speed:c.speed};
            })));
        }
    }
};

var CameraRig = {
    ready: false,
    reset: function (car) {
        this.x = car.x; this.z = car.z; this.yaw = car.yaw + Math.PI;
        this.distance = 10; this.sky = Engine.sceneObject('sky'); this.ready = true;
        this.update(car, 1);
    },
    update: function (car, dt) {
        var t = 1 - Math.exp(-8 * dt), r = 1 - Math.exp(-4.5 * dt);
        this.x += (car.x + Math.sin(car.yaw) * 2 - this.x) * t;
        this.z += (car.z + Math.cos(car.yaw) * 2 - this.z) * t;
        this.yaw += MathUtil.angle(car.yaw + Math.PI - this.yaw) * r;
        this.distance += (10 + car.speed * .095 - this.distance) * r;
        var pitch = .27;
        Engine.camera(this.x, 1.05, this.z, this.yaw, pitch, this.distance);
        Engine.transform(this.sky, {position: {
            x: this.x + Math.sin(this.yaw) * Math.cos(pitch) * this.distance,
            y: 1.05 + Math.sin(pitch) * this.distance,
            z: this.z + Math.cos(this.yaw) * Math.cos(pitch) * this.distance}, rotation: MathUtil.rotation(0)});
    }
};
