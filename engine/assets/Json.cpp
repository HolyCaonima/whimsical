#include "Json.h"
#include <duktape.h>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace whimsical {
Json::Json(double v) : value_(v) {
    if (!std::isfinite(v))
        throw std::invalid_argument("JSON number must be finite");
}
uint32_t Json::uint() const {
    double n = number();
    if (n < 0 || n > UINT32_MAX || n != std::floor(n))
        throw std::invalid_argument("Expected unsigned integer");
    return uint32_t(n);
}
using Heap = std::unique_ptr<duk_context, decltype(&duk_destroy_heap)>;
static Heap heap() {
    Heap c(duk_create_heap_default(), duk_destroy_heap);
    if (!c)
        throw std::runtime_error("Cannot allocate JSON parser");
    return c;
}
static Json read(duk_context* c) {
    if (duk_is_null(c, -1))
        return Json();
    if (duk_is_boolean(c, -1))
        return duk_get_boolean(c, -1) != 0;
    if (duk_is_number(c, -1))
        return duk_get_number(c, -1);
    if (duk_is_string(c, -1)) {
        duk_size_t n;
        const char* p = duk_get_lstring(c, -1, &n);
        return std::string(p, n);
    }
    if (duk_is_array(c, -1)) {
        auto result = Json::array();
        auto n = duk_get_length(c, -1);
        for (duk_uarridx_t i = 0; i < n; ++i) {
            duk_get_prop_index(c, -1, i);
            result.push(read(c));
            duk_pop(c);
        }
        return result;
    }
    auto result = Json::object();
    duk_enum(c, -1, DUK_ENUM_OWN_PROPERTIES_ONLY);
    while (duk_next(c, -1, 1)) {
        result[duk_get_string(c, -2)] = read(c);
        duk_pop_2(c);
    }
    duk_pop(c);
    return result;
}
Json Json::parse(const std::string& text) {
    auto c = heap();
    duk_push_lstring(c.get(), text.data(), text.size());
    if (duk_safe_call(
            c.get(),
            [](duk_context* c, void*) -> duk_ret_t {
                duk_json_decode(c, -1);
                return 1;
            },
            nullptr, 1, 1) != DUK_EXEC_SUCCESS)
        throw std::invalid_argument(std::string("Invalid JSON: ") + duk_safe_to_string(c.get(), -1));
    return read(c.get());
}
static void push(duk_context* c, const Json& j) {
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
        for (auto& item : j.elements()) {
            push(c, item);
            duk_put_prop_index(c, -2, i++);
        }
        break;
    }
    case 5:
        duk_push_bare_object(c);
        for (auto& item : j.members()) {
            push(c, item.second);
            duk_put_prop_lstring(c, -2, item.first.data(), item.first.size());
        }
        break;
    }
}
std::string Json::dump() const {
    auto c = heap();
    whimsical::push(c.get(), *this);
    duk_json_encode(c.get(), -1);
    return duk_get_string(c.get(), -1);
}
} // namespace whimsical
