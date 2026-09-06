function initialize() {
    RainCourt.build();
}
function fixedUpdate(dt, input) {
    // Publish the physical stance before a click can request a path through low clearance.
    Locomotion.prepare(dt, input);
    Controller.tick(dt, input);
    Locomotion.tick(dt, input);
    Companion.tick(dt);
    CameraRig.tick(dt, input);
    Engine.status(Locomotion.state, Controller.message);
}
