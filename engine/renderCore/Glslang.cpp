#define WHIMSICAL_GLSLANG_EXPORTS
#include "Glslang.h"
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <string>
#include <vector>

WHIMSICAL_GLSLANG_API bool whimsicalCompileGlsl(WhimsicalShaderStage stage, const char* source, void* user,
                                                WhimsicalGlslResult result) {
    static const bool initialized = glslang::InitializeProcess();
    (void)initialized;
    const auto language = stage == WhimsicalVertex ? EShLangVertex : stage == WhimsicalFragment ? EShLangFragment
                                                                                                : EShLangCompute;
    // The same translation as `glslang --target-env vulkan1.2 -V`.
    const auto messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
    glslang::TShader shader(language);
    shader.setStrings(&source, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, language, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);
    if (!shader.parse(GetDefaultResources(), 100, false, messages)) {
        result(user, nullptr, 0, (std::string(shader.getInfoLog()) + shader.getInfoDebugLog()).c_str());
        return false;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        result(user, nullptr, 0, (std::string(program.getInfoLog()) + program.getInfoDebugLog()).c_str());
        return false;
    }
    std::vector<unsigned int> words;
    glslang::GlslangToSpv(*program.getIntermediate(language), words);
    result(user, words.data(), words.size(), nullptr);
    return true;
}
