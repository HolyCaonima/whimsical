#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Logical resource declarations. This half of the render graph knows nothing about Vulkan:
// it is the vocabulary a render feature uses to say what a resource *is* and what a pass
// does to it, and it is the single source from which the descriptor layout, the GLSL
// binding block and the physical allocation are derived. Nothing else in the engine names
// a binding number.
namespace afterlight::rg {
enum class Format : uint8_t { RGBA16F, RGBA32F, RG16F, R16F, R32F, RGBA8, D32 };
struct FormatInfo {
    const char* glsl; // storage image qualifier
    uint32_t bytes;
};
FormatInfo formatInfo(Format);

enum class Kind : uint8_t { Image, Buffer, AccelerationStructure };

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
// Contents that exist outside this frame. Both directions of that matter to the compiler:
// a pass may consume them without a producer in this frame, and whatever produced them
// last is worth keeping however much else is culled.
inline bool crossesFrames(Lifetime lifetime) {
    return lifetime != Lifetime::Transient;
}

// Where a pass touches a resource. A site deliberately says nothing about direction, so no
// access can imply the wrong one; Usage is the only statement about the contents.
enum class Access : uint8_t {
    Compute,  // descriptor access from a compute shader
    Graphics, // descriptor access from a vertex or fragment shader
    Color,    // colour attachment
    Depth,    // depth attachment
    Transfer, // copy, blit or clear
    Vertex,   // vertex attribute fetch
    Index,    // index fetch
    Build,    // acceleration structure build
    Trace,    // acceleration structure walked by a ray query
    Present,  // handed to the presentation engine
    Host,     // read through mapped memory once the frame fence completes
};

// What a pass does to the contents it declares. This is the whole resource contract:
// dependency edges, culling, liveness, storage reuse, attachment load/store ops and
// barriers are all functions of it and of nothing else.
enum class Usage : uint8_t {
    Read,      // consumes the contents that reached this pass and produces none
    Overwrite, // produces every element; whatever was there is dead
    Modify,    // consumes and produces, so the old contents must reach this pass intact
};
inline bool produces(Usage usage) {
    return usage != Usage::Read;
}
inline bool consumes(Usage usage) {
    return usage != Usage::Overwrite;
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
    uint16_t divisor = 1; // screen size / divisor, rounded up
    // The state the frame hands this resource to its consumer in: the swapchain to the
    // presentation engine, a readback buffer to the host. The graph emits that transition
    // itself after the last pass, so nothing has to exist purely in order to make one.
    std::optional<Access> handover;
    std::function<uint64_t(uint32_t width, uint32_t height)> bytes; // buffers
    ShaderView view;
    ShaderView previous; // Lifetime::History only
};

// A buffer the host reads after the frame fence. It lives in readback memory and the frame
// ends with the barrier that makes the last write visible, which is a real dependency
// rather than something the fence is assumed to cover.
inline bool hostRead(const Declaration& declaration) {
    return declaration.handover == Access::Host;
}

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
    bool operator==(ResourceRef other) const {
        return id == other.id && slot == other.slot;
    }
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
    // Every graph-owned half whose contents a history reset has to discard. Transients are
    // excluded because their contents never reach a second frame, and the compiler already
    // rejects a pass that consumes one before this frame produced it.
    ResourceList resettable() const;
    // Generated GLSL, keyed by the include name the shader sources ask for.
    std::map<std::string, std::string> glsl() const;
};
} // namespace afterlight::rg
