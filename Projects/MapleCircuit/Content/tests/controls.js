// Native engine diagnostic map only: real project inputs, PhysicsScene, UI and opaque effects.
var ControlsVerification = {time: 0, pausedX: 0, pausedZ: 0, phase: 0, showcase: false};
var initializeGame = initialize;
initialize = function () {
    initializeGame(); Race.ready = false; Race.countdown = 0;
    ControlsVerification.showcase = !!Engine.sceneData().showcase;
    if (ControlsVerification.showcase) Race.autopilot = true;
    Engine.log('MAPLE_CONTROLS_BEGIN');
};
var updateGame = fixedUpdate;
var driveAI = Vehicle.ai;
Vehicle.ai = function (car, cars) {
    var control = driveAI(car, cars);
    if (ControlsVerification.showcase && car.index === 0 && ControlsVerification.time > 5.0) control.drift = true;
    return control;
};
fixedUpdate = function (dt, ignored) {
    var v = ControlsVerification, car = Race.cars[0];
    if (v.showcase && v.time >= 5.8) return;
    v.time += dt;
    if (v.showcase) { updateGame(dt, {keys:[],pressed:[],keyboardCaptured:false}); return; }
    var keys = [], pressed = [], t = v.time;
    if (t < 3.4) keys[87] = true;
    if (t > 1.65 && t < 3.4) { keys[32] = true; if (t < 2.35) keys[65] = true; }
    if (t >= 3.4 && v.phase === 0) {
        if (car.maxSpeed < 15 || Effects.emitted === 0 || !Effects.smoke.some(function(p) { return p.life > 0; }))
            throw new Error('Native acceleration / opaque drift smoke failed');
        v.pausedX = car.x; v.pausedZ = car.z; pressed[27] = true; v.phase = 1;
        Engine.log('MAPLE_CONTROLS_DRIFT_PASS ' + JSON.stringify({maxKmh:car.maxSpeed*3.6, smoke:Effects.emitted}));
    }
    if (t >= 4.3 && v.phase === 1) {
        if (car.x !== v.pausedX || car.z !== v.pausedZ || !Race.paused) throw new Error('Native pause moved the car');
        pressed[27] = true; pressed[82] = true; v.phase = 2;
    }
    if (t >= 4.5 && v.phase === 2) {
        if (car.track.distance > 2 || car.speed > 1) throw new Error('Native recovery failed');
        Race.reset(); v.phase = 3;
        if (Race.elapsed !== 0 || Race.cars[0].completed !== 0 || Effects.smoke.some(function(p){return p.life > 0;}))
            throw new Error('Native restart failed');
        Engine.log('MAPLE_NATIVE_CONTROLS_PASS');
    }
    updateGame(dt, {keys:keys,pressed:pressed,keyboardCaptured:false});
};
