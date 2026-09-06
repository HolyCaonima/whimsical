#include "AnimationInspector.h"
#include "core/World.h"
#include <algorithm>

namespace afterlight::ui {
AnimationInspectorLayout animationInspectorLayout(const AnimationInspection& inspection, uint32_t width,
                                                  uint32_t height) {
    AnimationInspectorLayout result;
    if (!inspection.entity || width < 620 || height < 450)
        return result;
    result.panel = {28, 216, 300, 64};
    for (size_t i = 0; i < inspection.schema.size(); ++i) {
        int y = result.panel.y + 80 + int(i) * 60;
        if (y + 40 > int(height) - 174)
            break;
        result.controls.push_back({i, {40, y, 28, 28}, {72, y, 212, 28}, {288, y, 28, 28}});
        result.panel.height += 60;
    }
    return result;
}
bool animationInspectorInput(World& world, const Input& input) {
    if (!input.focused)
        return false;
    auto inspection = world.inspectAnimation(world.selected);
    auto layout = animationInspectorLayout(inspection, input.width, input.height);
    if (!layout.panel.contains(input.mouseX, input.mouseY))
        return false;
    if (input.leftPressed) {
        for (const auto& control : layout.controls) {
            int direction = control.previous.contains(input.mouseX, input.mouseY) ? -1
                            : control.next.contains(input.mouseX, input.mouseY)   ? 1
                                                                                  : 0;
            if (!direction)
                continue;
            const auto& attribute = inspection.schema[control.attribute];
            const auto& current = inspection.values.at(attribute.key);
            auto found = std::find_if(attribute.options.begin(), attribute.options.end(),
                                      [&](const animation::EnumOption& o) { return o.value == current; });
            int count = int(attribute.options.size());
            int index = (int(found - attribute.options.begin()) + direction + count) % count;
            world.setAnimationAttribute(inspection.entity, attribute.key, attribute.options[index].value);
        }
    }
    return true;
}
} // namespace afterlight::ui
