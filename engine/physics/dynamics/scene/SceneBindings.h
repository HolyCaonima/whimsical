#pragma once
#include <duktape.h>
namespace whimsical {
class World;
namespace dynamics {
void installSceneBindings(duk_context*, World&);
}
} // namespace whimsical
