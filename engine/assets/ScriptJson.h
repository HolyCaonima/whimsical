#pragma once
#include "Json.h"
#include <duktape.h>

namespace whimsical {
// Script values and documents convert without JSON text, with JSON.stringify
// semantics: undefined and functions are omitted from objects and are null in
// arrays, and non-finite numbers are null.
Json scriptJson(duk_context*, duk_idx_t);
// Pushes plain script objects and arrays, as JSON.parse would.
void pushScriptJson(duk_context*, const Json&);
} // namespace whimsical
