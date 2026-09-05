/* Level-specific placement and triggers. Reusable interaction rules live in gameplay/. */
var RainCourt = {
    build : function() {
        var collision = Engine.readJson('physics/profiles.json');
        var config = Engine.readJson('levels/rain_court.json');
        Engine.navigation({min : {x : config.bounds.min[0], y : config.bounds.min[1], z : config.bounds.min[2]},
                           max : {x : config.bounds.max[0], y : config.bounds.max[1], z : config.bounds.max[2]},
                           cellSize : config.navigationCellSize});
        var definitions = Engine.readJson('materials/blockout.json'), m = {};
        for (var key in definitions) {
            var d = definitions[key], e = d.emission || [ 0, 0, 0 ];
            m[key] = Engine.material(d.color[0], d.color[1], d.color[2], d.roughness, e[0], e[1], e[2],
                                     d.metallic);
        }
        function box(name, x, y, z, sx, sy, sz, mat, solid, interact, profile) {
            var id = Engine.spawn(name, false, x, y, z, sx, sy, sz, m[mat], !!solid, !!interact);
            Engine.collider(id, collision[profile || (solid ? 'obstacle' : 'decoration')]);
            return id;
        }
        // Paving top stays at the navigation plane (Y=0). The foundation meets its underside at Y=-.07.
        // Putting both top faces at Y=0 causes depth fighting and unstable material/entity history.
        box('Foundation', 0, -.37, 0, 27, .6, 23, 'stone', false, false, 'ground');
        box('Court paving', 0, -.035, 0, 22, .07, 18, 'tile', false, false, 'ground');
        // Fill only the exposed rim at Y=0, keeping the walls and outer walkable area grounded.
        box('North foundation rim', 0, -.035, -10.25, 27, .07, 2.5, 'stone', false, false, 'ground');
        box('South foundation rim', 0, -.035, 10.25, 27, .07, 2.5, 'stone', false, false, 'ground');
        box('West foundation rim', -12.25, -.035, 0, 2.5, .07, 18, 'stone', false, false, 'ground');
        box('East foundation rim', 12.25, -.035, 0, 2.5, .07, 18, 'stone', false, false, 'ground');
        // Join the walls at Z=-9.4; their exposed top faces must not overlap at the corner.
        box('West parapet', -12, 1.8, .55, 1.2, 3.6, 19.9, 'dark', true);
        box('North parapet', 0, 1.8, -10, 25.2, 3.6, 1.2, 'dark', true);
        box('East low wall', 12, .7, .55, 1.2, 1.4, 19.9, 'dark', true);
        for (var i = 0; i < 9; i++) {
            box('Coping', -12, 3.7, -9 + i * 2.2, 1.5, .25, 1.8, 'stone', false);
            box('Coping', -10 + i * 2.7, 3.7, -10, 2.25, .25, 1.5, 'stone', false);
        }
        box('Service building', 7, 2.5, -7, 7, 5, 5, 'stone', true);
        box('Roof', 7, 5.15, -7, 7.5, .3, 5.5, 'dark', false, false, 'obstacle');
        box('Door frame', 5.2, 1.45, -4.42, 2.8, 2.9, .18, 'brass', true);
        var door = box('Service door', 5.2, 1.3, -4.2, 2.2, 2.6, .18, 'dark', true);
        box('Door light', 5.2, 3, -4.02, 2.4, .16, .14, 'warm', false);
        Engine.light(5.2, 3.2, -3.4, .7, 1, .57, .24, 30);
        var beacon = box('Power console', -6, .7, -3, 1.4, 1.4, 1.1, 'brass', true, true);
        box('Console display', -6, 1.45, -3, 1.1, .10, .75, 'cyan', false);
        var cyanLight = Engine.light(-6, 2.1, -2.7, .45, .23, .8, .72, 22);
        box('Cargo A', -4, .65, 4, 2.2, 1.3, 1.5, 'crate', true);
        box('Cargo B', -5, 1.65, 4.2, 1, 0.7, 1.2, 'dark', true);
        box('Cargo C', 7, .65, 3, 2, 1.3, 2, 'crate', true);
        var cache = box('Supply cache', 3, .45, 6.5, 1.3, .9, 1.0, 'brass', true, true);
        for (i = 0; i < 4; i++) {
            box('Pier', -9 + i * 5.5, 2, -9, 1.2, 4, 1.2, 'stone', true);
            box('Lamp', -9 + i * 5.5, 3.4, -8.3, .3, .7, .18, 'warm', false);
            Engine.light(-9 + i * 5.5, 3.4, -7.8, .3, 1, .64, .33, 13);
        }
        Engine.light(-4, 12, 5, 1.2, 1, .88, .64, 380);
        Engine.light(9, 8, 1, 1.5, .38, .58, .75, 65);
        var player = Engine.spawn('Kiln', true, config.spawn[0], config.spawn[1], config.spawn[2], 1, 1, 1,
                                  m.character, false, false);
        Engine.collider(player, collision.character);
        Engine.setPlayer(player);
        Locomotion.init(player);
        CameraRig.init(config.camera);
        Locomotion.marker = box('Heading marker', config.spawn[0], 1.36, config.spawn[2] + .42, .18, .14, .12,
                                'brass', false);
        var power = true;
        Interactions.register(beacon, 'Power console', {x : -6, y : 0, z : -1.7}, function() {
            power = !power;
            Engine.lightIntensity(cyanLight, power ? 22 : 0);
            Engine.setMaterial(beacon, power ? m.brass : m.dark);
            var p = Engine.position(door);
            Engine.pose(door, p.x, power ? 1.3 : 4.3, p.z, 0, 2.6);
            Engine.solid(door, power);
        });
        Interactions.register(cache, 'Supply cache', {x : 3, y : 0, z : 5.3}, function() {
            Engine.setMaterial(cache, m.cyan);
        });
        Engine.log('Rain Court loaded: single-character 3C, PBR primitives, dynamic RT lights.');
    }
};
