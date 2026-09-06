var Hud = {
    initialize: function () {
        this.doc = Engine.ui.loadDocument('/Game/UI/race.rml'); this.doc.show();
        this.nodes = {}; this.cache = {}; this.clock = 0;
        var ids = ['position','lap','lap-time','best-time','speed','speed-fill','grip','countdown','lap-message',
                   'watch','overlay','result-eyebrow','result-title','result-description','standings','continue','dot0','dot1','dot2','dot3'];
        for (var i = 0; i < ids.length; i++) this.nodes[ids[i]] = this.doc.getElementById(ids[i]);
        this.doc.getElementById('restart').on('click', function () {
            Hud.doc.getElementById('restart').blur(); Race.reset();
        });
        this.nodes['continue'].on('click', function () {
            Hud.nodes['continue'].blur();
            if (Race.ready) Race.ready = false; else if (Race.paused) Race.paused = false; else Race.reset();
        });
        this.nodes.watch.on('click', function () {
            Hud.nodes.watch.blur(); Race.autopilot = !Race.autopilot; Race.ready = false;
        });
    },
    time: function (seconds) {
        var minutes = Math.floor(seconds / 60), whole = Math.floor(seconds % 60), fraction = Math.floor(seconds * 100) % 100;
        return minutes + ':' + (whole < 10 ? '0' : '') + whole + '.' + (fraction < 10 ? '0' : '') + fraction;
    },
    text: function (id, value) {
        value = String(value);
        if (this.cache[id] !== value) { this.nodes[id].setText(value); this.cache[id] = value; }
    },
    update: function (dt) {
        var car = Race.cars[0], order = Race.order(), rank = 1;
        for (var i = 0; i < order.length; i++) if (order[i] === car) rank = i + 1;
        this.text('position', rank); this.text('lap', car.lap);
        this.text('speed', Math.round(car.speed * 3.6));
        this.text('lap-time', this.time(car.finish ? car.lastLap : Math.max(0, Race.elapsed - car.lapStart)));
        this.text('best-time', car.best ? this.time(car.best) : '--');
        this.text('grip', car.drifting ? 'DRIFT' : car.track.distance > Track.width ? 'GRASS' : 'GRIP');
        this.nodes.grip.setClass('drift', car.drifting);
        this.nodes['speed-fill'].setProperty('width', Math.min(129, car.speed / 38.1 * 129).toFixed(1) + 'px');
        this.text('countdown', Race.ready ? '' : Race.countdown > 0 ? Math.ceil(Race.countdown) : Race.elapsed < .9 ? 'GO!' : '');
        this.text('lap-message', Race.messageTime > 0 ? Race.message : '');
        this.text('watch', Race.autopilot ? 'TAKE THE WHEEL · V' : 'WATCH AI · V');
        for (var j = 0; j < Race.cars.length; j++) {
            var c = Race.cars[j], p = Track.minimap(c.x, c.z), dot = this.nodes['dot'+j];
            dot.setProperty('left', p.x.toFixed(1)+'px'); dot.setProperty('top', p.y.toFixed(1)+'px');
        }
        var overlay = Race.ready || Race.paused || Race.finished;
        this.nodes.overlay.setProperty('display', overlay ? 'block' : 'none');
        if (overlay) {
            this.text('result-eyebrow', Race.ready ? 'FOUR DRIVERS. THREE LAPS.' : Race.paused ? 'TAKE A BREATHER' : 'CHECKERED FLAG');
            this.text('result-title', Race.ready ? 'Ready on the grid.' : Race.paused ? 'Race paused' : rank === 1 ? 'First across the line.' : 'Race complete.');
            this.text('result-description', Race.ready ? 'W / S to drive. A / D to steer. Hold Space to drift. Press V to watch the AI race.' : Race.paused ? 'Press Esc to get back on the grid.' :
                'P' + rank + '  /  4     ·     Total ' + this.time(car.finish) + '     ·     Best ' + this.time(car.best));
            this.text('continue', Race.ready ? 'START RACE' : Race.paused ? 'BACK TO THE RACE' : 'RACE AGAIN');
            var rml = '';
            if (!Race.paused && !Race.ready) for (var k = 0; k < order.length; k++) {
                var racer = order[k];
                rml += '<div class="standing' + (racer.index === 0 ? ' you' : '') + '">' + (k+1) +
                    '　' + (racer.index === 0 ? 'YOU' : ['','ROSE','AMBER','SAGE'][racer.index]) +
                    '<span class="finish-time">' + (racer.finish ? this.time(racer.finish) : 'RACING') + '</span></div>';
            }
            if (this.lastStandings !== rml) { this.nodes.standings.setInnerRML(rml); this.lastStandings = rml; }
        }
    }
};
