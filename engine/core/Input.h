#pragma once
#include "uiCore/UiInput.h"
#include <array>
#include <string>
#include <vector>
namespace afterlight {
// Platform input data, independent of World, animation and rendering.
struct Input {
    std::vector<ui::InputEvent> uiEvents;
    bool pointerCaptured = false, keyboardCaptured = false;
    std::u32string text;
    std::array<bool, 256> keys{}, pressed{};
    bool left = false, right = false, middle = false, leftPressed = false, rightPressed = false,
         focused = true;
    float mouseX = 0, mouseY = 0, deltaX = 0, deltaY = 0, wheel = 0;
    uint32_t width = 1280, height = 800;
};
} // namespace afterlight
