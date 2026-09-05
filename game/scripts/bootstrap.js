function initialize() {
    RainCourt.build();
}
function fixedUpdate(dt, input) {
    // Publish the physical stance before a click can request a path through low clearance.
    Locomotion.prepare(input);
    Controller.tick(dt, input);
    Locomotion.tick(dt, input);
    CameraRig.tick(dt, input);
    Engine.status(Locomotion.state, Controller.message);
}
