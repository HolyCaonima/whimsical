#include "ShaderAccess.h"
#include <algorithm>
#include <stdexcept>

namespace afterlight::rg {
namespace {
enum Op : uint16_t {
    EntryPoint = 15,
    TypeImage = 25,
    TypeSampler = 26,
    TypeSampledImage = 27,
    TypePointer = 32,
    Function = 54,
    Parameter = 55,
    FunctionEnd = 56,
    Call = 57,
    Variable = 59,
    Load = 61,
    Store = 62,
    CopyMemory = 63,
    CopyMemorySized = 64,
    AccessChain = 65,
    InBoundsAccessChain = 66,
    PtrAccessChain = 67,
    ArrayLength = 68,
    InBoundsPtrAccessChain = 70,
    Decorate = 71,
    CopyObject = 83,
    Transpose = 84,
    SampledImage = 86,
    FirstImageSample = 87,
    LastImageSample = 97,
    ImageRead = 98,
    ImageWrite = 99,
    ImageOf = 100,
    FirstImageQuery = 101,
    LastImageQuery = 107,
    ImageTexelPointer = 143,
    Select = 169,
    AtomicLoad = 227,
    AtomicStore = 228,
    LastAtomic = 242,
    Phi = 245,
    ReturnValue = 254,
    CopyLogical = 400,
    RayQueryInitialize = 4473,
    TypeAccelerationStructure = 5341,
};
constexpr uint8_t Required = 1, Reads = 2, Writes = 4;
using Roots = std::vector<uint32_t>;
bool unite(Roots& into, const Roots& from) {
    bool changed = false;
    for (auto root : from)
        if (std::find(into.begin(), into.end(), root) == into.end()) {
            into.push_back(root);
            changed = true;
        }
    return changed;
}
struct Instruction {
    const uint32_t* w;
    uint16_t op, length;
    uint32_t function;
};
struct FunctionInfo {
    std::vector<uint32_t> parameters, returns, calls;
    bool reachable = false, hasBody = false;
};
} // namespace

std::vector<ShaderAccess> reflect(const Registry& registry, const uint32_t* words, size_t count) {
    if (count < 5 || words[0] != 0x07230203)
        throw std::runtime_error("Not a SPIR-V module");
    const auto bound = words[3];
    std::vector<Instruction> instructions;
    std::vector<Roots> roots(bound);
    std::vector<uint8_t> opaque(bound), pointer(bound), touched(registry.bindingCount());
    std::vector<uint32_t> bindings(bound, ~0u), sets(bound, ~0u);
    std::vector<FunctionInfo> functions(bound);
    uint32_t function = 0, entry = 0;
    Access stage = Access::Compute;
    for (size_t at = 5; at < count;) {
        const auto* w = words + at;
        const auto length = uint16_t(w[0] >> 16), op = uint16_t(w[0]);
        if (!length || at + length > count)
            throw std::runtime_error("Truncated SPIR-V module");
        at += length;
        if (op == EntryPoint) {
            if (entry)
                throw std::runtime_error("Reflection expects one shader entry point");
            entry = w[2];
            if (w[1] != 0 && w[1] != 4 && w[1] != 5)
                throw std::runtime_error("Unsupported shader stage");
            stage = w[1] == 5 ? Access::Compute : Access::Graphics;
        } else if (op == Decorate && length >= 4) {
            if (w[2] == 33)
                bindings[w[1]] = w[3];
            if (w[2] == 34)
                sets[w[1]] = w[3];
        } else if (op == TypeImage || op == TypeSampler || op == TypeSampledImage ||
                   op == TypeAccelerationStructure)
            opaque[w[1]] = 1;
        else if (op == TypePointer) {
            if (w[2] == 5349)
                throw std::runtime_error("Physical storage pointers need explicit resource provenance");
            pointer[w[1]] = 1;
        } else if (op == Function)
            function = w[2];
        else if (op == Parameter)
            functions[function].parameters.push_back(w[2]);
        else if (op == ReturnValue)
            functions[function].returns.push_back(w[1]);
        else if (op == Call)
            functions[function].calls.push_back(w[3]);
        else if (op == 248)
            functions[function].hasBody = true;
        instructions.push_back({w, op, length, function});
        if (op == FunctionEnd)
            function = 0;
    }
    if (!entry)
        throw std::runtime_error("Shader has no entry point");
    std::vector<uint32_t> pending{entry};
    while (!pending.empty()) {
        auto id = pending.back();
        pending.pop_back();
        if (functions[id].reachable)
            continue;
        functions[id].reachable = true;
        for (auto callee : functions[id].calls)
            pending.push_back(callee);
    }
    for (const auto& instruction : instructions) {
        const auto* w = instruction.w;
        if (instruction.op != Variable || instruction.function)
            continue;
        // Descriptor storage classes must have a binding. Local, input/output and push
        // constant variables are not descriptor resources.
        if (w[3] != 0 && w[3] != 2 && w[3] != 12)
            continue;
        const auto id = w[2], binding = bindings[id];
        if (sets[id] != 0 || binding >= registry.bindingCount())
            throw std::runtime_error("Shader descriptor outside render graph set 0");
        roots[id].push_back(binding);
    }
    std::vector<uint32_t> unresolved;
    auto touch = [&](uint32_t id, uint8_t how, bool resourceObject = false) {
        if (resourceObject && roots[id].empty())
            unresolved.push_back(id);
        for (auto binding : roots[id])
            touched[binding] |= Required | how;
    };
    // A monotone points-to analysis handles forward calls, parameters, returned objects,
    // select/phi and loop back edges. Each parameter has its own provenance; multiple
    // call sites conservatively union possible resources, never discard a possible access.
    bool changed;
    do {
        changed = false;
        unresolved.clear();
        auto follow = [&](uint32_t result, uint32_t source) {
            changed |= unite(roots[result], roots[source]);
        };
        for (const auto& instruction : instructions) {
            if (!instruction.function || !functions[instruction.function].reachable)
                continue;
            const auto* w = instruction.w;
            const auto op = instruction.op, length = instruction.length;
            switch (op) {
            case AccessChain:
            case InBoundsAccessChain:
            case PtrAccessChain:
            case InBoundsPtrAccessChain:
            case CopyObject:
            case CopyLogical:
            case ImageOf:
            case ImageTexelPointer:
                follow(w[2], w[3]);
                touch(w[3], 0);
                break;
            case SampledImage:
                follow(w[2], w[3]);
                follow(w[2], w[4]);
                break;
            case Load:
                touch(w[3], opaque[w[1]] || pointer[w[1]] ? 0 : Reads, opaque[w[1]] != 0);
                if (opaque[w[1]] || pointer[w[1]])
                    follow(w[2], w[3]);
                break;
            case Variable:
                if (length >= 5 && !roots[w[4]].empty())
                    throw std::runtime_error("Unsupported shader resource pointer initializer");
                break;
            case Store:
                touch(w[1], Writes);
                if (!roots[w[2]].empty())
                    throw std::runtime_error("Unsupported shader resource pointer escape through store");
                break;
            case CopyMemory:
            case CopyMemorySized:
                touch(w[1], Writes);
                touch(w[2], Reads);
                break;
            case Call: {
                const auto& callee = functions[w[3]];
                if (!callee.hasBody || callee.parameters.size() != size_t(length - 4))
                    throw std::runtime_error("Cannot reflect unresolved shader function call");
                for (size_t i = 0; i < callee.parameters.size(); ++i) {
                    follow(callee.parameters[i], w[i + 4]);
                    touch(w[i + 4], 0);
                }
                for (auto result : callee.returns)
                    follow(w[2], result);
                break;
            }
            case Select:
                follow(w[2], w[4]);
                follow(w[2], w[5]);
                break;
            case Phi:
                for (uint16_t i = 3; i < length; i += 2)
                    follow(w[2], w[i]);
                break;
            case ImageWrite:
                touch(w[1], Writes, true);
                break;
            case ImageRead:
                touch(w[3], Reads, true);
                break;
            case AtomicStore:
                touch(w[1], Writes);
                break;
            case AtomicLoad:
                touch(w[3], Reads);
                break;
            case ReturnValue:
                touch(w[1], 0);
                break;
            case ArrayLength:
                touch(w[3], 0);
                break;
            case RayQueryInitialize:
                touch(w[2], Reads, true);
                break;
            default:
                if (op >= FirstImageSample && op <= LastImageSample)
                    touch(w[3], Reads, true);
                else if (op >= FirstImageQuery && op <= LastImageQuery)
                    touch(w[3], 0, true);
                else if (op > AtomicStore && op <= LastAtomic)
                    touch(w[3], Reads | Writes);
                // These operations cannot access descriptor contents or forward pointers.
                // Any instruction outside this supported vocabulary is diagnosed, including
                // extension memory operations. Adding an opcode requires classifying it.
                else if (op == 0 || op == 8 || op == 12 || op == Transpose || op == Function ||
                         op == Parameter || op == FunctionEnd || op == Variable || (op >= 77 && op <= 82) ||
                         (op >= 109 && op <= 124) || (op >= 126 && op <= 215) || op == 224 || op == 225 ||
                         (op >= 246 && op <= 257) || op == 317 || (op >= 4472 && op <= 4479) ||
                         (op >= 6016 && op <= 6032)) {
                    // Scalar operations cannot consume a descriptor object. In particular,
                    // pointer-to-integer conversion or an extended instruction must not
                    // erase provenance and make a later memory access invisible.
                    uint16_t first = 0, end = length;
                    if (op == 12)
                        first = 5;
                    else if ((op >= 77 && op <= 82) || op == Transpose || (op >= 109 && op <= 124) ||
                             (op >= 126 && op <= 215))
                        first = 3;
                    if (op == 79 || op == 82)
                        end = 5; // remaining operands are literals
                    if (op == 81)
                        end = 4;
                    if (first)
                        for (uint16_t i = first; i < end; ++i)
                            if (!roots[w[i]].empty())
                                throw std::runtime_error("Unsupported SPIR-V resource operand in opcode " +
                                                         std::to_string(op));
                    if (length >= 3 && (pointer[w[1]] || opaque[w[1]]) && op != Variable && op != Parameter &&
                        op != Function)
                        throw std::runtime_error("Unsupported SPIR-V resource propagation opcode " +
                                                 std::to_string(op));
                } else
                    throw std::runtime_error("Unsupported SPIR-V access opcode " + std::to_string(op));
                break;
            }
        }
    } while (changed);
    if (!unresolved.empty())
        throw std::runtime_error("Cannot resolve shader resource operand " +
                                 std::to_string(unresolved.front()));
    std::vector<ShaderAccess> accesses;
    for (uint32_t binding = 0; binding < touched.size(); ++binding) {
        if (!touched[binding])
            continue;
        const auto ref = registry.binding(binding);
        const auto access = registry[ref.id].kind == Kind::AccelerationStructure ? Access::Trace : stage;
        accesses.push_back(
            {binding, access, bool(touched[binding] & Reads), bool(touched[binding] & Writes)});
    }
    return accesses;
}

void merge(std::vector<ShaderAccess>& into, const std::vector<ShaderAccess>& from) {
    for (const auto& access : from) {
        auto found = std::find_if(into.begin(), into.end(), [&](const ShaderAccess& other) {
            return other.binding == access.binding && other.access == access.access;
        });
        if (found == into.end())
            into.push_back(access);
        else {
            found->reads |= access.reads;
            found->writes |= access.writes;
        }
    }
}
} // namespace afterlight::rg
