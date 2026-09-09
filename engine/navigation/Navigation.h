#pragma once
#include "physics/PhysicsScene.h"
namespace whimsical {
struct NavigationAgent {
    float radius = .4f, height = 2;
    uint32_t owner = 0;
};
struct NavigationSettings {
    vec3 min{-13, -.5f, -11}, max{13, 8, 11};
    float cellSize = .25f;
    // Current locomotion is planar; other elevations require explicit traversal links.
    float planeTolerance = .025f;
};
class Navigation {
  public:
    static std::optional<vec3> ground(const PhysicsScene&, vec3 point, const NavigationSettings& = {});
    static bool canStand(const PhysicsScene&, vec3 feet, const NavigationAgent& = {},
                         const NavigationSettings& = {});
    static bool lineClear(const PhysicsScene&, vec3 start, vec3 end, const NavigationAgent& = {},
                          const NavigationSettings& = {});
    static std::vector<vec3> findPath(const PhysicsScene&, vec3 start, vec3 end, const NavigationAgent& = {},
                                      const NavigationSettings& = {});
};
} // namespace whimsical
