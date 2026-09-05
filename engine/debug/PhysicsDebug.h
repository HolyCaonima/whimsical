#pragma once
#include "core/Types.h"
#include "physics/PhysicsScene.h"
namespace afterlight {
// Snapshot adapter: the render thread receives line values, never PhysicsScene pointers or handles.
inline void appendPhysicsDebug(const PhysicsScene& scene, Frame& frame) {
    frame.physicsDebug = true;
    for (auto h : scene.bodies()) {
        const auto& body = scene.body(h);
        if (!body.blocking && !body.walkable && !body.pickable && body.layer != CollisionLayer::Trigger)
            continue;
        vec3 color = body.layer == CollisionLayer::Trigger     ? vec3(1, .35f, .7f)
                     : body.layer == CollisionLayer::Character ? vec3(.35f, 1, .6f)
                     : body.walkable                           ? vec3(.3f, .65f, 1)
                                                               : vec3(1, .7f, .25f);
        auto line = [&](vec3 a, vec3 b) {
            frame.physicsLines.push_back({body.pose.position + body.pose.rotation * a,
                                          body.pose.position + body.pose.rotation * b, color});
        };
        if (body.shape.type == ColliderType::Box) {
            for (int axis = 0; axis < 3; ++axis)
                for (int a = -1; a <= 1; a += 2)
                    for (int b = -1; b <= 1; b += 2) {
                        vec3 p = body.shape.halfExtents, q = p;
                        p[(axis + 1) % 3] *= float(a);
                        p[(axis + 2) % 3] *= float(b);
                        q = p;
                        p[axis] *= -1;
                        line(p, q);
                    }
        } else {
            const float r = body.shape.radius, segment = body.shape.halfSegment;
            constexpr int Steps = 24;
            for (int i = 0; i < Steps; ++i) {
                float a = 2 * Pi * i / Steps, b = 2 * Pi * (i + 1) / Steps;
                for (float y : {-segment, segment})
                    line({r * std::cos(a), y, r * std::sin(a)}, {r * std::cos(b), y, r * std::sin(b)});
                if (i % 6 == 0)
                    line({r * std::cos(a), -segment, r * std::sin(a)},
                         {r * std::cos(a), segment, r * std::sin(a)});
                float capA = Pi * i / Steps, capB = Pi * (i + 1) / Steps;
                for (float sign : {-1.f, 1.f})
                    for (bool zPlane : {false, true}) {
                        auto cap = [&](float angle) {
                            float y = sign * (segment + r * std::sin(angle));
                            return zPlane ? vec3(0, y, r * std::cos(angle)) : vec3(r * std::cos(angle), y, 0);
                        };
                        line(cap(capA), cap(capB));
                    }
            }
        }
    }
}
} // namespace afterlight
