#pragma once
#include "core/World.h"

namespace whimsical {
// The ordinary case: ECS owns the value, native code edits a draft, scripts and
// maps share its codec. Backend components supply their own preparation/cleanup.
template <class T, class Decode, class Encode, class Validate>
ComponentContract dataComponent(const char* name, Decode decode, Encode encode, Validate validate) {
    auto c = component<T, T>(name);
    c.nativeValue = [](const void* value) -> std::any { return *static_cast<const T*>(value); };
    c.decode = [decode, validate](const Json& j) -> std::any {
        T value = decode(j);
        validate(value);
        return value;
    };
    c.encode = [encode](const std::any& v) { return encode(std::any_cast<const T&>(v)); };
    c.validateValue = [validate](const std::any& v) { validate(std::any_cast<const T&>(v)); };
    c.prepare = [](const World&, Entity e, const std::any& v, AssetManager&) -> PreparedComponent {
        return [e, value = std::any_cast<const T&>(v)](ComponentAccess& access) {
            if (access.world.has<T>(e))
                access.world.edit<T>(e, [&](T& draft) { draft = value; });
            else
                access.world.add<T>(e, value);
        };
    };
    c.inspect = [](const World& w, Entity e) -> std::optional<std::any> { return w.get<T>(e); };
    return c;
}
} // namespace whimsical
