#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

// Logical resource declarations. This half of the render graph knows nothing about Vulkan:
// it is the vocabulary a render feature uses to say what a resource *is*, and it is the
// single source from which the descriptor layout, the GLSL binding block and the physical
// allocation are derived. Nothing else in the engine names a binding number.
namespace afterlight::rg {
enum class Format : uint8_t { RGBA16F, RGBA32F, RG16F, R16F, R32F, RGBA8, D32 };
struct FormatInfo {
    const char* glsl; // storage image qualifier
    uint32_t bytes;
};
FormatInfo formatInfo(Format);

enum class Kind : uint8_t { Image, Buffer, Tlas };

// Who owns the memory and how long the contents mean anything. This is the ownership
// statement the graph compiles against; it decides aliasing, clearing and cross-frame
// barriers, and it is the only place a feature says "this survives the frame".
enum class Lifetime : uint8_t {
    Transient,  // graph-owned, meaningful only between its first write and last read
    Persistent, // graph-owned, contents carried into the next frame in place
    History,    // graph-owned pair; previous() names the half written last frame
    Imported,   // owner-allocated, graph-synchronised (swapchain, acceleration structure)
    External,   // owner-allocated and owner-synchronised; the graph only binds it
};
inline bool graphOwned(Lifetime lifetime) {
    return lifetime == Lifetime::Transient || lifetime == Lifetime::Persistent ||
           lifetime == Lifetime::History;
}

// Where the generated declaration lands in the shader sources, which also fixes the
// descriptor stage mask: shared declarations are visible to raster, the rest are compute.
enum class Section : uint8_t { Shared, Compute, Rtxdi };

enum class BindingType : uint8_t { None, Uniform, Storage, StorageImage, SamplerArray, Tlas };

// One shader-visible view of a resource. A history resource has two of these, and the two
// halves swap physical resources every frame without either shader knowing.
struct ShaderView {
    BindingType type = BindingType::None;
    bool readOnly = false;
    std::string name;  // GLSL identifier, or the instance name of a uniform block
    std::string block; // block name for buffer declarations
    std::string body;  // block members for buffer declarations
    uint32_t count = 1;
    uint32_t binding = 0; // assigned by the registry
    explicit operator bool() const {
        return type != BindingType::None;
    }
};

struct Declaration {
    std::string name;
    Kind kind = Kind::Image;
    Lifetime lifetime = Lifetime::Transient;
    Section section = Section::Compute;
    Format format = Format::RGBA16F;
    uint16_t divisor = 1;  // screen size / divisor, rounded up
    bool readback = false; // host-cached buffer read after the frame fence
    std::function<uint64_t(uint32_t width, uint32_t height)> bytes; // buffers
    ShaderView view;
    ShaderView previous; // Lifetime::History only
};

enum class Slot : uint8_t { Current, Previous };

struct ResourceId {
    uint16_t index = 0xffff;
    bool valid() const {
        return index != 0xffff;
    }
    bool operator==(ResourceId other) const {
        return index == other.index;
    }
    bool operator<(ResourceId other) const {
        return index < other.index;
    }
};

// A resource plus which half of a history pair is meant. Non-history resources ignore it.
struct ResourceRef {
    ResourceId id;
    Slot slot = Slot::Current;
    ResourceRef() = default;
    ResourceRef(ResourceId resource) : id(resource) {}
    ResourceRef(ResourceId resource, Slot half) : id(resource), slot(half) {}
};
inline ResourceRef previous(ResourceId id) {
    return {id, Slot::Previous};
}
using ResourceList = std::vector<ResourceRef>;

// Declaration order is binding order. Features register into one registry at startup; the
// registry is then the only thing that knows binding numbers exist.
class Registry {
    std::vector<Declaration> declarations_;
    uint32_t bindings_ = 0;

  public:
    ResourceId declare(Declaration);
    const Declaration& operator[](ResourceId id) const {
        return declarations_[id.index];
    }
    size_t size() const {
        return declarations_.size();
    }
    uint32_t bindingCount() const {
        return bindings_;
    }
    // Physical slots: a history resource owns two, everything else one.
    uint32_t physicalCount() const;
    uint32_t physicalBase(ResourceId) const;
    const ShaderView& view(ResourceRef) const;
    // Every graph-owned half whose contents a history reset has to discard. Depth is the
    // one exclusion: it is cleared by its own load op on every frame that uses it.
    ResourceList resettable() const;
    // Generated GLSL, keyed by the include name the shader sources ask for.
    std::map<std::string, std::string> glsl() const;
};
} // namespace afterlight::rg
