#pragma once
#include <cstdint>
namespace whimsical::ui {
// Ordered platform events preserve clicks, key repeats and edits between simulation ticks.
struct InputEvent {
    enum class Type { KeyDown, KeyUp, Text, MouseMove, MouseDown, MouseUp, Wheel, FocusLost };
    Type type;
    uint32_t code = 0;
    float x = 0, y = 0;
    int modifiers = 0; // shift=1, control=2, alt=4; independent of RmlUi enums.
};
} // namespace whimsical::ui
