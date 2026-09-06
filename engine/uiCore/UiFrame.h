#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace afterlight::ui {
// Backend-neutral immutable resources. A mailbox drop cannot lose a creation or release command.
struct Vertex {
    float x, y;
    uint32_t color;
    float u, v;
};
struct Geometry {
    uint64_t id;
    std::vector<Vertex> vertices;
    std::vector<int> indices;
};
struct Texture {
    uint64_t id;
    int width, height;
    std::vector<uint8_t> pixels; // RGBA, premultiplied alpha.
};
struct Draw {
    std::shared_ptr<const Geometry> geometry;
    std::shared_ptr<const Texture> texture;
    std::array<float, 16> transform;
    float x = 0, y = 0;
    std::array<int, 4> scissor; // x, y, width, height in context pixels.
};
struct UiFrame {
    int width = 0, height = 0;
    std::vector<Draw> draws;
};
} // namespace afterlight::ui
