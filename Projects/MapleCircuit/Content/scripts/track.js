var MathUtil = {
    clamp: function (v, a, b) { return Math.max(a, Math.min(b, v)); },
    wrap: function (v, size) { return ((v % size) + size) % size; },
    angle: function (v) { return MathUtil.wrap(v + Math.PI, Math.PI * 2) - Math.PI; },
    rotation: function (yaw) { return {x: 0, y: Math.sin(yaw / 2), z: 0, w: Math.cos(yaw / 2)}; }
};

var Track = {
    initialize: function (data) {
        this.points = data.track;
        this.length = data.trackLength;
        this.width = data.roadHalfWidth;
        this.step = this.length / this.points.length;
        this.minimum = [Infinity, Infinity]; this.maximum = [-Infinity, -Infinity];
        for (var i = 0; i < this.points.length; i++) {
            for (var k = 0; k < 2; k++) {
                this.minimum[k] = Math.min(this.minimum[k], this.points[i][k]);
                this.maximum[k] = Math.max(this.maximum[k], this.points[i][k]);
            }
        }
    },
    at: function (distance, lane) {
        var u = MathUtil.wrap(distance, this.length) / this.step;
        var i = Math.floor(u), t = u - i;
        var a = this.points[i], b = this.points[(i + 1) % this.points.length];
        var dx = b[0] - a[0], dz = b[1] - a[1], n = Math.sqrt(dx * dx + dz * dz);
        dx /= n; dz /= n;
        return {x: a[0] + (b[0] - a[0]) * t + dz * lane,
                z: a[1] + (b[1] - a[1]) * t - dx * lane, yaw: Math.atan2(dx, dz)};
    },
    project: function (x, z) {
        var best = Infinity, result;
        for (var i = 0; i < this.points.length; i++) {
            var a = this.points[i], b = this.points[(i + 1) % this.points.length];
            var dx = b[0] - a[0], dz = b[1] - a[1], n = dx * dx + dz * dz;
            var t = MathUtil.clamp(((x - a[0]) * dx + (z - a[1]) * dz) / n, 0, 1);
            var ex = x - a[0] - t * dx, ez = z - a[1] - t * dz;
            var d = ex * ex + ez * ez;
            if (d < best) {
                best = d;
                result = {s: (i + t) * this.step, distance: Math.sqrt(d),
                          lane: (ex * dz - ez * dx) / Math.sqrt(n), yaw: Math.atan2(dx, dz)};
            }
        }
        return result;
    },
    crossed: function (previous, current, gate) {
        var advance = MathUtil.angle((current - previous) / this.length * Math.PI * 2) * this.length / (Math.PI * 2);
        var toGate = MathUtil.wrap(gate - previous, this.length);
        // Segment projection can land exactly on a gate. Include that endpoint; the
        // ordered nextGate state prevents duplicate awards on the following tick.
        return advance > 0 && toGate <= advance + 0.000001;
    },
    minimap: function (x, z) {
        return {x: (35 + (x - this.minimum[0]) / (this.maximum[0] - this.minimum[0]) * 290) / 2,
                y: (35 + (this.maximum[1] - z) / (this.maximum[1] - this.minimum[1]) * 330) / 2};
    }
};
