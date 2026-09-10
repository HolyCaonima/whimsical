#include "SceneBindings.h"
#include "DynamicsSystem.h"
#include "core/World.h"
#include <cstring>
#include <cmath>

namespace whimsical::dynamics {
namespace {
uint32_t integer(duk_context* c, int i) {
    auto n = duk_get_number(c, i);
    if (!std::isfinite(n) || n < 0 || n > UINT32_MAX || std::floor(n) != n)
        throw std::invalid_argument("Expected unsigned integer");
    return uint32_t(n);
}
std::vector<float> floats(duk_context* c, int i) {
    std::vector<float> values;
    if (duk_is_buffer_data(c, i)) {
        duk_size_t bytes = 0;
        auto p = duk_get_buffer_data(c, i, &bytes);
        if (bytes % sizeof(float))
            throw std::invalid_argument("Incomplete float buffer");
        values.resize(bytes / sizeof(float));
        if (bytes)
            std::memcpy(values.data(), p, bytes);
    } else {
        if (!duk_is_array(c, i))
            throw std::invalid_argument("Expected numeric array");
        for (uint32_t k = 0; k < duk_get_length(c, i); ++k) {
            duk_get_prop_index(c, i, k);
            values.push_back(float(duk_get_number(c, -1)));
            duk_pop(c);
        }
    }
    for (auto n : values)
        if (!std::isfinite(n))
            throw std::invalid_argument("Expected finite value");
    return values;
}
duk_ret_t call(duk_context* c) {
    duk_push_heap_stash(c);
    duk_get_prop_string(c, -1, "dynamicsScene");
    auto world = static_cast<World*>(duk_get_pointer(c, -1));
    duk_pop_2(c);
    try {
        auto& system = world->dynamics;
        auto e = integer(c, 0);
        switch (duk_get_current_magic(c)) {
        case 0: {
            auto text = system.state(e).dump();
            duk_push_lstring(c, text.data(), text.size());
            duk_json_decode(c, -1);
            return 1;
        }
        case 1: {
            auto values = system.values(e, integer(c, 1), integer(c, 2), integer(c, 3));
            auto bytes = values.size() * sizeof(float);
            auto buffer = duk_push_fixed_buffer(c, bytes);
            if (bytes)
                std::memcpy(buffer, values.data(), bytes);
            duk_push_buffer_object(c, -1, 0, bytes, DUK_BUFOBJ_FLOAT32ARRAY);
            duk_remove(c, -2);
            return 1;
        }
        case 2:
            if (!duk_is_boolean(c, 1))
                throw std::invalid_argument("Expected paused flag");
            system.control(e, duk_get_boolean(c, 1) != 0, duk_get_boolean(c, 2) != 0);
            break;
        case 3: {
            if (!duk_is_string(c, 1))
                throw std::invalid_argument("Expected state field");
            system.write(e, {stateField(duk_get_string(c, 1)), integer(c, 2), integer(c, 3), floats(c, 4)});
            break;
        }
        case 4:
            if (!duk_is_string(c, 1))
                throw std::invalid_argument("Expected model field");
            system.patch(e, modelField(duk_get_string(c, 1)), integer(c, 2), integer(c, 3), floats(c, 4));
            break;
        }
        return 0;
    } catch (const std::exception& error) {
        duk_push_error_object(c, DUK_ERR_ERROR, "%s", error.what());
    }
    duk_throw_raw(c);
}
} // namespace
void installSceneBindings(duk_context* c, World& world) {
    duk_push_heap_stash(c);
    duk_push_pointer(c, &world);
    duk_put_prop_string(c, -2, "dynamicsScene");
    duk_pop(c);
    duk_get_global_string(c, "Engine");
    duk_get_prop_string(c, -1, "dynamics");
    duk_push_object(c);
    const char* names[] = {"state", "values", "control", "write", "patch"};
    for (int i = 0; i < 5; ++i) {
        duk_push_c_function(c, call, DUK_VARARGS);
        duk_set_magic(c, -1, i);
        duk_put_prop_string(c, -2, names[i]);
    }
    duk_put_prop_string(c, -2, "scene");
    duk_pop_2(c);
}
} // namespace whimsical::dynamics
