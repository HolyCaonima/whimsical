#include "ShaderAccess.h"
#include <stdexcept>

namespace afterlight::rg {
namespace {
// SPIR-V is laid out so one forward pass is enough: decorations precede types, and types
// and global variables precede the functions that use them.
enum Op : uint16_t {
    EntryPoint = 15,
    TypeImage = 25,
    TypeSampler = 26,
    TypeSampledImage = 27,
    Variable = 59,
    Load = 61,
    Store = 62,
    AccessChain = 65,
    InBoundsAccessChain = 66,
    PtrAccessChain = 67,
    InBoundsPtrAccessChain = 70,
    Decorate = 71,
    CopyObject = 83,
    SampledImage = 86,
    FirstImageSample = 87, // sample, fetch, gather and their depth-reference variants
    LastImageSample = 97,
    ImageRead = 98,
    ImageWrite = 99,
    ImageOf = 100,
    AtomicLoad = 227,
    AtomicStore = 228,
    LastAtomic = 242,
    RayQueryInitialize = 4473,
    TypeAccelerationStructure = 5341,
};
constexpr uint32_t SpirvMagic = 0x07230203, BindingDecoration = 33, ComputeExecution = 5;
constexpr uint8_t Reads = 1, Writes = 2;

// Which resource an instruction's operand ultimately names. Everything in a shader reaches
// a binding through a chain of pointers and loaded objects, so following that chain back
// to the variable it started from is the whole of the reflection.
struct Trace {
    std::vector<uint32_t> root;   // id -> the bound variable it came from, or 0
    std::vector<uint8_t> opaque;  // type id -> an image, sampler or structure, not contents
    std::vector<uint32_t> binding; // variable id -> its binding number, or 0xffffffff
    std::vector<uint8_t> touched;  // binding number -> Reads | Writes

    uint32_t of(uint32_t id) const {
        return id < root.size() ? root[id] : 0;
    }
    void follow(uint32_t result, uint32_t from) {
        root[result] = of(from);
    }
    void touch(uint32_t id, uint8_t how) {
        const uint32_t variable = of(id);
        if (variable)
            touched[binding[variable]] |= how;
    }
};
} // namespace

std::vector<ShaderAccess> reflect(const Registry& registry, const uint32_t* words, size_t count) {
    if (count < 5 || words[0] != SpirvMagic)
        throw std::runtime_error("Not a SPIR-V module");
    Trace trace;
    trace.root.assign(words[3], 0);
    trace.opaque.assign(words[3], 0);
    trace.binding.assign(words[3], 0xffffffff);
    trace.touched.assign(registry.bindingCount(), 0);
    bool compute = false;
    for (size_t at = 5; at + 1 <= count;) {
        const uint32_t* word = words + at;
        const uint16_t length = uint16_t(word[0] >> 16), op = uint16_t(word[0] & 0xffff);
        if (!length || at + length > count)
            throw std::runtime_error("Truncated SPIR-V module");
        at += length;
        switch (op) {
        case EntryPoint:
            compute = word[1] == ComputeExecution;
            break;
        case TypeImage:
        case TypeSampler:
        case TypeSampledImage:
        case TypeAccelerationStructure:
            trace.opaque[word[1]] = 1;
            break;
        case Decorate:
            if (length >= 4 && word[2] == BindingDecoration) {
                if (word[3] >= registry.bindingCount())
                    throw std::runtime_error("Shader binding outside the render graph registry");
                trace.binding[word[1]] = word[3];
            }
            break;
        case Variable:
            // Only a bound variable is a resource. Locals and anything else the shader
            // declares stay at root 0, which makes every chain built on them uninteresting.
            if (trace.binding[word[2]] != 0xffffffff)
                trace.root[word[2]] = word[2];
            break;
        case AccessChain:
        case InBoundsAccessChain:
        case PtrAccessChain:
        case InBoundsPtrAccessChain:
        case CopyObject:
        case SampledImage:
        case ImageOf:
            trace.follow(word[2], word[3]);
            break;
        case Load:
            // Loading an image or a structure yields the object itself; the access happens
            // where that object is sampled, read or written. Loading anything else is a
            // read of the contents at the pointer.
            if (trace.opaque[word[1]])
                trace.follow(word[2], word[3]);
            else
                trace.touch(word[3], Reads);
            break;
        case Store:
            trace.touch(word[1], Writes);
            break;
        case ImageWrite:
            trace.touch(word[1], Writes);
            break;
        case ImageRead:
            trace.touch(word[3], Reads);
            break;
        case AtomicStore:
            trace.touch(word[1], Writes);
            break;
        case RayQueryInitialize:
            trace.touch(word[2], Reads);
            break;
        default:
            // Everything left that can reach a resource does so in one of three ways. The
            // extent and level queries are deliberately not among them: they read a
            // resource's shape, not its contents, so a pass that only measures an image
            // still overwrites it.
            if (op >= FirstImageSample && op <= LastImageSample)
                trace.touch(word[3], Reads);
            else if (op == AtomicLoad)
                trace.touch(word[3], Reads);
            else if (op > AtomicStore && op <= LastAtomic)
                trace.touch(word[3], Reads | Writes);
            break;
        }
    }
    std::vector<ShaderAccess> accesses;
    for (uint32_t number = 0; number < trace.touched.size(); ++number) {
        const uint8_t how = trace.touched[number];
        if (!how)
            continue;
        const ResourceRef ref = registry.binding(number);
        const auto& declaration = registry[ref.id];
        ShaderAccess access;
        access.ref = ref;
        access.access = declaration.kind == Kind::AccelerationStructure ? Access::Trace
                        : compute                                       ? Access::Compute
                                                                        : Access::Graphics;
        access.usage = !(how & Writes) ? Usage::Read
                       : (how & Reads) || !declaration.wholeWrites ? Usage::Modify
                                                                   : Usage::Overwrite;
        accesses.push_back(access);
    }
    return accesses;
}

void merge(std::vector<ShaderAccess>& into, const std::vector<ShaderAccess>& from) {
    for (const auto& access : from) {
        auto found = into.end();
        for (auto at = into.begin(); at != into.end(); ++at)
            if (at->ref == access.ref && at->access == access.access)
                found = at;
        if (found == into.end()) {
            into.push_back(access);
            continue;
        }
        const bool reads = consumes(found->usage) || consumes(access.usage);
        const bool writes = produces(found->usage) || produces(access.usage);
        found->usage = !writes ? Usage::Read : reads ? Usage::Modify : Usage::Overwrite;
    }
}
} // namespace afterlight::rg
