#include "ScriptJson.h"
#include <cmath>
#include <stdexcept>

namespace whimsical {
namespace {
bool omitted(duk_context* c, duk_idx_t index) {
    return duk_is_undefined(c, index) || duk_is_function(c, index);
}
Json read(duk_context* c, duk_idx_t index, int depth) {
    if (depth > 1000)
        throw std::invalid_argument("Script value is cyclic or nested too deeply for JSON");
    switch (duk_get_type(c, index)) {
    case DUK_TYPE_BOOLEAN:
        return duk_get_boolean(c, index) != 0;
    case DUK_TYPE_NUMBER: {
        const double value = duk_get_number(c, index);
        return std::isfinite(value) ? Json(value) : Json();
    }
    case DUK_TYPE_STRING: {
        duk_size_t length;
        const char* text = duk_get_lstring(c, index, &length);
        return std::string(text, length);
    }
    case DUK_TYPE_OBJECT:
    case DUK_TYPE_BUFFER:
        break;
    default:
        return Json();
    }
    duk_require_stack(c, 3);
    if (duk_is_array(c, index)) {
        Json::Array elements;
        const auto length = duk_get_length(c, index);
        elements.reserve(length);
        for (duk_uarridx_t i = 0; i < length; ++i) {
            duk_get_prop_index(c, index, i);
            elements.push_back(omitted(c, -1) ? Json() : read(c, duk_get_top_index(c), depth + 1));
            duk_pop(c);
        }
        return elements;
    }
    Json::Object members;
    duk_enum(c, index, DUK_ENUM_OWN_PROPERTIES_ONLY);
    while (duk_next(c, -1, 1)) {
        if (!omitted(c, -1))
            members.insert_or_assign(duk_get_string(c, -2), read(c, duk_get_top_index(c), depth + 1));
        duk_pop_2(c);
    }
    duk_pop(c);
    return members;
}
} // namespace
Json scriptJson(duk_context* c, duk_idx_t index) {
    return read(c, duk_normalize_index(c, index), 0);
}
void pushScriptJson(duk_context* c, const Json& j) {
    duk_require_stack(c, 2);
    switch (j.value().index()) {
    case 0:
        duk_push_null(c);
        break;
    case 1:
        duk_push_boolean(c, j.boolean());
        break;
    case 2:
        duk_push_number(c, j.number());
        break;
    case 3:
        duk_push_lstring(c, j.string().data(), j.string().size());
        break;
    case 4: {
        duk_push_array(c);
        duk_uarridx_t i = 0;
        for (const auto& item : j.elements()) {
            pushScriptJson(c, item);
            duk_put_prop_index(c, -2, i++);
        }
        break;
    }
    case 5:
        duk_push_object(c);
        for (const auto& [key, item] : j.members()) {
            pushScriptJson(c, item);
            duk_put_prop_lstring(c, -2, key.data(), key.size());
        }
        break;
    }
}
} // namespace whimsical
