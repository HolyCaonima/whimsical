function initialize() {
    var data = Engine.sceneData();
    Track.initialize(data);
    Race.initialize(data);
    Effects.initialize(data);
    CameraRig.reset(Race.cars[0]);
    Hud.initialize();
    Engine.log('MAPLE_READY: four cars, ' + Track.length.toFixed(1) + ' m circuit, 3 laps; opaque smoke.');
}

function fixedUpdate(dt, input) {
    if (!input.keyboardCaptured) {
        if (Race.ready && (input.keys[87] || input.pressed[13])) Race.ready = false;
        if (!Race.ready && !Race.finished && (input.pressed[27] || input.pressed[80])) Race.paused = !Race.paused;
        if (input.pressed[86]) { Race.autopilot = !Race.autopilot; Race.ready = false; }
        if (input.pressed[82] && !Race.paused && !Race.finished) Vehicle.recover(Race.cars[0]);
        if (input.pressed[13] && Race.finished) Race.reset();
    }
    // Preserve handling integration at 60 Hz even when a developer uses t.TimeScale.
    var steps = Math.ceil(dt * 60), step = dt / steps;
    for (var i = 0; i < steps; i++) Race.update(step, input);
    CameraRig.update(Race.cars[0], Race.paused ? 0 : dt);
}

function updateUI(dt) { Hud.update(dt); }
