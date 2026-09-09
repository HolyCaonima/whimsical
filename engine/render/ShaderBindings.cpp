#include "render/RenderResources.h"
#include <filesystem>
#include <fstream>
#include <iostream>

// Emits the GLSL declaration blocks for every resource in the render graph registry, so
// that the shader sources and the descriptor set layout cannot disagree about a binding.
// Optional resources have no shader view, which is why the tool can construct the registry
// without knowing anything about the run it will serve.
int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: shader_bindings <output directory>\n";
        return 2;
    }
    try {
        std::filesystem::path directory(argv[1]);
        std::filesystem::create_directories(directory);
        for (const auto& [name, source] : whimsical::RenderResources(false, 0).registry.glsl()) {
            std::ofstream out(directory / name, std::ios::binary);
            out.exceptions(std::ios::failbit | std::ios::badbit);
            out << "// Generated from engine/render/RenderResources.cpp. Do not edit.\n" << source;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "shader_bindings: " << error.what() << '\n';
        return 1;
    }
}
