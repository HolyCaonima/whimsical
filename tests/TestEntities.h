#pragma once
#include "core/World.h"
// Historical gameplay test fixture, explicitly composing capabilities. Production
// entity creation has no primitive/character recipe and no implicit component adds.
inline afterlight::Entity spawnTest(afterlight::World& w, std::string name, afterlight::Shape shape,
                                    afterlight::vec3 position, afterlight::vec3 scale, uint32_t material,
                                    bool blocking, bool interactable) {
    using namespace afterlight;
    auto e = w.create(std::move(name));
    w.transforms.add(e, {position});
    RenderComponent r;
    r.shape = shape;
    r.scale = scale;
    r.material = material;
    w.render.add(e, r);
    PhysicsBody b;
    b.shape = shape == Shape::Box ? ColliderShape::box(scale * .5f)
                                  : ColliderShape::capsule(.4f * std::max(scale.x, scale.z), 2 * scale.y);
    b.layer = shape == Shape::Capsule ? CollisionLayer::Character : CollisionLayer::World;
    b.motion = shape == Shape::Capsule || interactable ? BodyMotion::Kinematic : BodyMotion::Static;
    b.blocking = blocking || shape == Shape::Capsule;
    b.pickable = blocking || interactable || shape == Shape::Capsule;
    w.motion.add(e, b);
    if (interactable)
        w.add<Interactable>(e);
    return e;
}
