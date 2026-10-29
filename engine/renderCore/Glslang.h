#pragma once
#include <cstddef>
#include <cstdint>

// In-process GLSL to SPIR-V. The prebuilt glslang libraries use the release C
// runtime, so they live in a DLL with that runtime and serve every build
// configuration. Only C types cross this boundary.
#ifdef WHIMSICAL_GLSLANG_EXPORTS
#define WHIMSICAL_GLSLANG_API extern "C" __declspec(dllexport)
#else
#define WHIMSICAL_GLSLANG_API extern "C" __declspec(dllimport)
#endif

enum WhimsicalShaderStage { WhimsicalCompute, WhimsicalVertex, WhimsicalFragment };
// Receives the SPIR-V words on success, otherwise null words and the diagnostics.
using WhimsicalGlslResult = void (*)(void* user, const uint32_t* words, size_t count, const char* log);
// Thread-safe: independent sources may compile concurrently.
WHIMSICAL_GLSLANG_API bool whimsicalCompileGlsl(WhimsicalShaderStage stage, const char* source, void* user,
                                                WhimsicalGlslResult result);
