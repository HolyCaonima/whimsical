#include "Registry.h"
#include <sstream>
#include <stdexcept>

namespace whimsical::rg {
FormatInfo formatInfo(Format format) {
    switch (format) {
    case Format::RGBA16F:
        return {"rgba16f", 8};
    case Format::RGBA32F:
        return {"rgba32f", 16};
    case Format::RG16F:
        return {"rg16f", 4};
    case Format::R16F:
        return {"r16f", 2};
    case Format::R32F:
        return {"r32f", 4};
    case Format::RGBA8:
        return {"rgba8", 4};
    case Format::D32:
        return {"", 4};
    case Format::R32Uint:
        return {"r32ui", 4};
    }
    throw std::runtime_error("Unknown render graph format");
}

ResourceId Registry::declare(Declaration declaration) {
    if (declarations_.size() >= 0xffff)
        throw std::runtime_error("Render graph resource capacity exceeded");
    const ResourceId id{uint16_t(declarations_.size())};
    if (declaration.view) {
        declaration.view.binding = uint32_t(bindings_.size());
        bindings_.push_back(id);
    }
    if (declaration.previous) {
        if (declaration.lifetime != Lifetime::History)
            throw std::runtime_error("Only a history resource has a previous view: " + declaration.name);
        declaration.previous.binding = uint32_t(bindings_.size());
        bindings_.push_back(previous(id));
    }
    declarations_.push_back(std::move(declaration));
    return id;
}

void Registry::truncateImports(size_t first) {
    for (size_t i = first; i < declarations_.size(); ++i) {
        const auto& d = declarations_[i];
        if (d.lifetime != Lifetime::Imported || d.view || d.previous)
            throw std::logic_error("Only unbound frame imports may be removed");
    }
    declarations_.resize(first);
}

uint32_t Registry::physicalCount() const {
    uint32_t count = 0;
    for (const auto& declaration : declarations_)
        count += declaration.lifetime == Lifetime::History ? 2 : 1;
    return count;
}

uint32_t Registry::physicalBase(ResourceId id) const {
    uint32_t base = 0;
    for (uint16_t i = 0; i < id.index; ++i)
        base += declarations_[i].lifetime == Lifetime::History ? 2 : 1;
    return base;
}

ResourceList Registry::resettable() const {
    ResourceList list;
    for (uint16_t i = 0; i < declarations_.size(); ++i) {
        const auto& declaration = declarations_[i];
        if (!graphOwned(declaration.lifetime) || !crossesFrames(declaration.lifetime) ||
            hostRead(declaration))
            continue;
        list.push_back(ResourceId{i});
        if (declaration.lifetime == Lifetime::History)
            list.push_back(previous(ResourceId{i}));
    }
    return list;
}

const ShaderView& Registry::view(ResourceRef ref) const {
    const auto& declaration = declarations_[ref.id.index];
    return ref.slot == Slot::Previous ? declaration.previous : declaration.view;
}

static void emit(std::ostringstream& out, const Declaration& declaration, const ShaderView& view) {
    switch (view.type) {
    case BindingType::Uniform:
        out << "layout(set=0,binding=" << view.binding << ",std140) uniform " << view.block << " {\n"
            << view.body << "\n} " << view.name << ";\n";
        break;
    case BindingType::Storage:
        out << "layout(set=0,binding=" << view.binding << ",std430) " << (view.readOnly ? "readonly " : "")
            << "buffer " << view.block << " {\n"
            << view.body << "\n};\n";
        break;
    case BindingType::StorageImage:
        out << "layout(set=0,binding=" << view.binding << "," << formatInfo(declaration.format).glsl
            << ") uniform " << (declaration.format == Format::R32Uint ? "uimage2D " : "image2D ")
            << view.name << ";\n";
        break;
    case BindingType::SamplerArray:
        out << "layout(set=0,binding=" << view.binding << ") uniform sampler2D " << view.name << "["
            << view.count << "];\n";
        break;
    case BindingType::Tlas:
        out << "layout(set=0,binding=" << view.binding << ") uniform accelerationStructureEXT " << view.name
            << ";\n";
        break;
    case BindingType::None:
        break;
    }
}

std::map<std::string, std::string> Registry::glsl() const {
    std::map<std::string, std::ostringstream> sections;
    for (const auto& declaration : declarations_) {
        auto& out = sections[declaration.section];
        emit(out, declaration, declaration.view);
        emit(out, declaration, declaration.previous);
    }
    std::map<std::string, std::string> result;
    for (auto& section : sections)
        result.emplace("graph." + section.first + ".glsl", section.second.str());
    return result;
}
} // namespace whimsical::rg
