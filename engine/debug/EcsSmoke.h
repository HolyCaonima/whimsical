#pragma once
#include "core/World.h"
#include "assets/StaticMesh.h"
#include <iostream>
namespace whimsical {
// GPU lifecycle regression driven by *presented* frames, so the mailbox cannot
// collapse all mutations into one unobserved snapshot. Use with the Afterlight project map.
class EcsSmoke {
    uint32_t stage_ = 0, nextFrame_ = 12;
    Entity first_ = 0, second_ = 0;
    std::shared_ptr<const SkinnedMesh> skin_;
    static std::shared_ptr<const StaticMesh> mesh(uint32_t triangles) {
        auto m = std::make_shared<StaticMesh>();
        for (uint32_t i = 0; i < triangles; ++i) {
            float x = float(i % 100) * .02f, z = float(i / 100) * .02f;
            for (vec3 p : {vec3(x, 0, z), vec3(x, 0, z + .01f), vec3(x + .01f, 0, z)}) {
                m->indices.push_back(uint32_t(m->vertices.size()));
                m->vertices.push_back({p, {0, 1, 0}, {0, 0}, {1, 0, 0, 1}});
            }
        }
        return m;
    }

  public:
    bool complete() const {
        return stage_ == 9;
    }
    void advance(World& w, uint32_t rendered) {
        if (complete() || rendered < nextFrame_)
            return;
        auto player = w.findObject(w.resources.references.at("player"));
        switch (stage_) {
        case 0:
            skin_ = w.get<Skin>(player).mesh;
            w.setEnabled(player, false);
            break;
        case 1:
            w.setEnabled(player, true);
            break;
        case 2:
            w.remove<Skin>(player);
            break;
        case 3:
            w.animation.setSkinnedMesh(player, skin_);
            break;
        case 4: {
            auto shared = mesh(1);
            first_ = w.create("Geometry lifecycle A");
            second_ = w.create("Geometry lifecycle B");
            for (auto e : {first_, second_}) {
                w.transforms.add(e, {{35, 1, 0}});
                w.render.add(e, {}, shared);
            }
            break;
        }
        case 5:
            w.destroy(first_);
            break;
        case 6:
            w.render.setStaticMesh(second_, mesh(20000));
            break;
        case 7:
            w.destroy(second_);
            break;
        case 8:
            break;
        }
        std::cout << "[ECS smoke] completed stage " << ++stage_ << '\n';
        nextFrame_ = rendered + 12;
    }
};
} // namespace whimsical
