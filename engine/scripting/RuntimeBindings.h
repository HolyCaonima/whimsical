#pragma once
#include <duktape.h>
namespace afterlight {
// Scene, simulation, view and authoring access share the existing World/Catalog contracts.
void installRuntimeBindings(duk_context*);
} // namespace afterlight
