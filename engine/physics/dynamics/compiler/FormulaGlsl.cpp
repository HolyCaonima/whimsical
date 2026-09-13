#include "FormulaGlsl.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <locale>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace whimsical::dynamics {
namespace {
std::string num(float value) {
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << std::scientific << std::setprecision(9) << value;
    return s.str();
}
uint32_t arity(MathOp op) {
    switch (op) {
    case MathOp::Constant:
    case MathOp::Input:
        return 0;
    case MathOp::Negate:
    case MathOp::Sqrt:
    case MathOp::Sin:
    case MathOp::Cos:
    case MathOp::Exp:
    case MathOp::Log:
    case MathOp::Abs:
        return 1;
    case MathOp::Select:
        return 3;
    default:
        return 2;
    }
}
constexpr uint32_t ZeroDerivative = UINT32_MAX;
uint32_t bits(float value) {
    uint32_t result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}
using NodeKey = std::tuple<MathOp, uint32_t, uint32_t, uint32_t, uint32_t>;
Formula simplify(const Formula& source) {
    source.validate();
    Formula result;
    result.inputs = source.inputs;
    std::map<NodeKey, uint32_t> common;
    std::vector<uint32_t> remap(source.nodes.size());
    auto raw = [&](MathNode node) {
        NodeKey key{node.op, node.a, node.b, node.c, bits(node.value)};
        if (auto found = common.find(key); found != common.end())
            return found->second;
        auto id = uint32_t(result.nodes.size());
        result.nodes.push_back(node);
        common.emplace(key, id);
        return id;
    };
    auto constant = [&](float value) {
        return raw(MathNode{MathOp::Constant, 0, 0, 0, value});
    };
    auto constantValue = [&](uint32_t id, float& value) {
        if (result.nodes[id].op != MathOp::Constant)
            return false;
        value = result.nodes[id].value;
        return true;
    };
    auto is = [&](uint32_t id, float value) {
        return result.nodes[id].op == MathOp::Constant && result.nodes[id].value == value;
    };
    auto negate = [&](uint32_t id) {
        float value;
        if (constantValue(id, value) && std::isfinite(-value))
            return constant(-value);
        if (result.nodes[id].op == MathOp::Negate)
            return result.nodes[id].a;
        return raw(MathNode{MathOp::Negate, id});
    };
    for (size_t i = 0; i < source.nodes.size(); ++i) {
        auto node = source.nodes[i];
        if (node.op == MathOp::Sum)
            throw std::logic_error("Collection sums must be lowered before local GLSL generation");
        const auto count = arity(node.op);
        if (count > 0)
            node.a = remap[node.a];
        if (count > 1)
            node.b = remap[node.b];
        if (count > 2)
            node.c = remap[node.c];
        uint32_t id = ZeroDerivative;
        float a = 0, b = 0, folded = 0;
        const bool ca = count > 0 && constantValue(node.a, a);
        const bool cb = count > 1 && constantValue(node.b, b);
        if (node.op == MathOp::Constant || node.op == MathOp::Input) {
            id = raw(node);
        } else if (node.op == MathOp::Negate) {
            id = negate(node.a);
        } else if (node.op == MathOp::Add) {
            if (is(node.a, 0))
                id = node.b;
            else if (is(node.b, 0))
                id = node.a;
            else if (ca && cb && std::isfinite(folded = a + b))
                id = constant(folded);
            else {
                if (node.b < node.a)
                    std::swap(node.a, node.b);
                id = raw(node);
            }
        } else if (node.op == MathOp::Subtract) {
            if (is(node.b, 0))
                id = node.a;
            else if (node.a == node.b)
                id = constant(0);
            else if (is(node.a, 0))
                id = negate(node.b);
            else if (ca && cb && std::isfinite(folded = a - b))
                id = constant(folded);
            else
                id = raw(node);
        } else if (node.op == MathOp::Multiply) {
            if (is(node.a, 0) || is(node.b, 0))
                id = constant(0);
            else if (is(node.a, 1))
                id = node.b;
            else if (is(node.b, 1))
                id = node.a;
            else if (is(node.a, -1))
                id = negate(node.b);
            else if (is(node.b, -1))
                id = negate(node.a);
            else if (ca && cb && std::isfinite(folded = a * b))
                id = constant(folded);
            else {
                if (node.b < node.a)
                    std::swap(node.a, node.b);
                id = raw(node);
            }
        } else if (node.op == MathOp::Divide) {
            if (is(node.b, 1))
                id = node.a;
            else if (ca && cb && b != 0 && std::isfinite(folded = a / b))
                id = constant(folded);
            else
                id = raw(node);
        } else if (node.op == MathOp::Select) {
            if (node.b == node.c)
                id = node.b;
            else if (ca)
                id = a != 0 ? node.b : node.c;
            else
                id = raw(node);
        } else if (node.op == MathOp::Less) {
            id = ca && cb ? constant(a < b ? 1.f : 0.f) : raw(node);
        } else if (node.op == MathOp::Min || node.op == MathOp::Max) {
            if (node.a == node.b)
                id = node.a;
            else if (ca && cb)
                id = constant(node.op == MathOp::Min ? std::min(a, b) : std::max(a, b));
            else
                id = raw(node);
        } else if (ca && count == 1) {
            switch (node.op) {
            case MathOp::Sqrt:
                folded = std::sqrt(a);
                break;
            case MathOp::Sin:
                folded = std::sin(a);
                break;
            case MathOp::Cos:
                folded = std::cos(a);
                break;
            case MathOp::Exp:
                folded = std::exp(a);
                break;
            case MathOp::Log:
                folded = std::log(a);
                break;
            case MathOp::Abs:
                folded = std::abs(a);
                break;
            default:
                folded = 0;
                break;
            }
            id = std::isfinite(folded) ? constant(folded) : raw(node);
        } else if (node.op == MathOp::Atan2 && ca && cb) {
            folded = std::atan2(a, b);
            id = std::isfinite(folded) ? constant(folded) : raw(node);
        } else {
            id = raw(node);
        }
        remap[i] = id;
    }
    for (auto output : source.outputs)
        result.outputs.push_back(remap[output]);

    // Algebraic identities can disconnect nodes that were live in the input DAG.
    // Compact again so downstream differentiation and source generation never see them.
    std::vector<uint8_t> used(result.nodes.size());
    for (auto output : result.outputs)
        used[output] = 1;
    for (size_t i = used.size(); i-- > 0;)
        if (used[i]) {
            const auto& node = result.nodes[i];
            const auto count = arity(node.op);
            if (count > 0)
                used[node.a] = 1;
            if (count > 1)
                used[node.b] = 1;
            if (count > 2)
                used[node.c] = 1;
        }
    std::vector<uint32_t> compactMap(result.nodes.size());
    std::vector<MathNode> compact;
    for (size_t i = 0; i < result.nodes.size(); ++i)
        if (used[i]) {
            auto node = result.nodes[i];
            const auto count = arity(node.op);
            if (count > 0)
                node.a = compactMap[node.a];
            if (count > 1)
                node.b = compactMap[node.b];
            if (count > 2)
                node.c = compactMap[node.c];
            compactMap[i] = uint32_t(compact.size());
            compact.push_back(node);
        }
    for (auto& output : result.outputs)
        output = compactMap[output];
    result.nodes = std::move(compact);
    return result;
}
struct SymbolicJacobian {
    std::vector<MathNode> nodes;
    std::vector<uint32_t> values;
    std::map<NodeKey, uint32_t> common;
    // Compiler-local predicates preserve the exact min/max tie convention.
    // They are binary nodes for liveness/CSE, without extending the model DSL.
    enum class Comparison { LessEqual, GreaterEqual };
    std::map<uint32_t, Comparison> comparisons;
    std::map<std::tuple<Comparison, uint32_t, uint32_t>, uint32_t> commonComparisons;

    explicit SymbolicJacobian(const Formula& formula) : nodes(formula.nodes) {
        for (uint32_t i = 0; i < nodes.size(); ++i) {
            const auto& n = nodes[i];
            common.emplace(NodeKey{n.op, n.a, n.b, n.c, bits(n.value)}, i);
        }
    }
    uint32_t raw(MathNode node) {
        NodeKey key{node.op, node.a, node.b, node.c, bits(node.value)};
        if (auto found = common.find(key); found != common.end())
            return found->second;
        auto id = uint32_t(nodes.size());
        nodes.push_back(node);
        common.emplace(key, id);
        return id;
    }
    bool is(uint32_t id, float value) const {
        return id != ZeroDerivative && nodes[id].op == MathOp::Constant && nodes[id].value == value;
    }
    bool constantValue(uint32_t id, float& value) const {
        if (id == ZeroDerivative || nodes[id].op != MathOp::Constant)
            return false;
        value = nodes[id].value;
        return true;
    }
    uint32_t constant(float value) {
        return raw({MathOp::Constant, 0, 0, 0, value});
    }
    uint32_t compare(Comparison op, uint32_t a, uint32_t b) {
        auto key = std::make_tuple(op, a, b);
        if (auto found = commonComparisons.find(key); found != commonComparisons.end())
            return found->second;
        const auto id = uint32_t(nodes.size());
        nodes.push_back({MathOp::Less, a, b});
        comparisons.emplace(id, op);
        commonComparisons.emplace(key, id);
        return id;
    }
    uint32_t choose(uint32_t condition, uint32_t yes, uint32_t no) {
        if (yes == ZeroDerivative) yes = constant(0.0f);
        if (no == ZeroDerivative) no = constant(0.0f);
        if (yes == no) return yes;
        float value;
        if (constantValue(condition, value)) return value != 0.0f ? yes : no;
        return raw({MathOp::Select, condition, yes, no});
    }
    uint32_t unary(MathOp op, uint32_t a) {
        return raw({op, a});
    }
    uint32_t negative(uint32_t a) {
        if (a == ZeroDerivative)
            return a;
        if (is(a, 0.0f))
            return ZeroDerivative;
        if (is(a, 1.0f))
            return constant(-1.0f);
        float value;
        if (constantValue(a, value) && std::isfinite(-value))
            return constant(-value);
        if (nodes[a].op == MathOp::Negate)
            return nodes[a].a;
        return unary(MathOp::Negate, a);
    }
    uint32_t sum(uint32_t a, uint32_t b) {
        if (a == ZeroDerivative || is(a, 0.0f))
            return b;
        if (b == ZeroDerivative || is(b, 0.0f))
            return a;
        float av, bv, folded;
        if (constantValue(a, av) && constantValue(b, bv) && std::isfinite(folded = av + bv))
            return constant(folded);
        if (b < a)
            std::swap(a, b);
        return raw({MathOp::Add, a, b});
    }
    uint32_t product(uint32_t a, uint32_t b) {
        if (a == ZeroDerivative || b == ZeroDerivative || is(a, 0.0f) || is(b, 0.0f))
            return ZeroDerivative;
        if (is(a, 1.0f))
            return b;
        if (is(b, 1.0f))
            return a;
        if (is(a, -1.0f))
            return negative(b);
        if (is(b, -1.0f))
            return negative(a);
        float av, bv, folded;
        if (constantValue(a, av) && constantValue(b, bv) && std::isfinite(folded = av * bv))
            return constant(folded);
        if (b < a)
            std::swap(a, b);
        return raw({MathOp::Multiply, a, b});
    }
    uint32_t quotient(uint32_t a, uint32_t b) {
        if (a == ZeroDerivative || is(a, 0.0f))
            return ZeroDerivative;
        if (is(b, 1.0f))
            return a;
        float av, bv, folded;
        if (constantValue(a, av) && constantValue(b, bv) && bv != 0 &&
            std::isfinite(folded = av / bv))
            return constant(folded);
        return raw({MathOp::Divide, a, b});
    }
};
bool supportsSymbolicDerivative(const Formula& formula) {
    return std::none_of(formula.nodes.begin(), formula.nodes.end(), [](const MathNode& node) {
        return node.op == MathOp::Abs || node.op == MathOp::Select;
    });
}
std::unique_ptr<SymbolicJacobian> buildSymbolicJacobian(
    const Formula& formula,
    const std::vector<int32_t>& derivativeColumn,
    size_t columns) {
    auto result = std::make_unique<SymbolicJacobian>(formula);
    const bool piecewise = std::any_of(formula.nodes.begin(), formula.nodes.end(), [](const MathNode& node) {
        return node.op == MathOp::Min || node.op == MathOp::Max;
    });
    result->values.assign(formula.outputs.size() * columns, ZeroDerivative);
    std::vector<uint8_t> depends(formula.nodes.size());
    for (size_t i = 0; i < formula.nodes.size(); ++i) {
        const auto& n = formula.nodes[i];
        if (n.op == MathOp::Input)
            depends[i] = derivativeColumn[n.a] >= 0;
        else if (n.op == MathOp::Constant || n.op == MathOp::Less)
            depends[i] = 0;
        else {
            const auto count = arity(n.op);
            depends[i] = (count > 0 && depends[n.a]) || (count > 1 && depends[n.b]);
        }
    }
    const auto one = result->constant(1.0f);
    for (size_t row = 0; row < formula.outputs.size(); ++row) {
        std::vector<uint32_t> adjoint(formula.nodes.size(), ZeroDerivative);
        const auto output = formula.outputs[row];
        if (depends[output])
            adjoint[output] = one;
        for (size_t i = formula.nodes.size(); i-- > 0;) {
            const auto d = adjoint[i];
            if (d == ZeroDerivative)
                continue;
            const auto& n = formula.nodes[i];
            auto add = [&](uint32_t child, uint32_t term) {
                // A zero adjoint must still suppress a singular derivative in an
                // unselected branch. Encode that value selection in the DAG so
                // CSE and liveness also apply to piecewise derivatives.
                if (piecewise && n.op != MathOp::Add && n.op != MathOp::Subtract &&
                    n.op != MathOp::Negate && n.op != MathOp::Min && n.op != MathOp::Max)
                    term = result->choose(d, term, ZeroDerivative);
                adjoint[child] = result->sum(adjoint[child], term);
            };
            switch (n.op) {
            case MathOp::Input: {
                const auto column = derivativeColumn[n.a];
                if (column >= 0) {
                    const auto at = row * columns + size_t(column);
                    result->values[at] = result->sum(result->values[at], d);
                }
                break;
            }
            case MathOp::Add:
                if (depends[n.a])
                    add(n.a, d);
                if (depends[n.b])
                    add(n.b, d);
                break;
            case MathOp::Subtract:
                if (depends[n.a])
                    add(n.a, d);
                if (depends[n.b])
                    add(n.b, result->negative(d));
                break;
            case MathOp::Multiply:
                if (depends[n.a])
                    add(n.a, result->product(d, n.b));
                if (depends[n.b])
                    add(n.b, result->product(d, n.a));
                break;
            case MathOp::Divide:
                if (depends[n.a])
                    add(n.a, result->quotient(d, n.b));
                if (depends[n.b])
                    add(n.b,
                        result->quotient(result->product(result->negative(d), n.a),
                                         result->product(n.b, n.b)));
                break;
            case MathOp::Negate:
                if (depends[n.a])
                    add(n.a, result->negative(d));
                break;
            case MathOp::Min:
            case MathOp::Max: {
                const bool minimum = n.op == MathOp::Min;
                if (depends[n.a]) {
                    const auto condition = result->compare(minimum ? SymbolicJacobian::Comparison::LessEqual :
                        SymbolicJacobian::Comparison::GreaterEqual, n.a, n.b);
                    add(n.a, result->choose(condition, d, ZeroDerivative));
                    if (depends[n.b]) add(n.b, result->choose(condition, ZeroDerivative, d));
                } else if (depends[n.b]) {
                    const auto condition = result->raw({MathOp::Less, minimum ? n.b : n.a, minimum ? n.a : n.b});
                    add(n.b, result->choose(condition, d, ZeroDerivative));
                }
                break;
            }
            case MathOp::Sqrt:
                if (depends[n.a])
                    add(n.a, result->quotient(d, result->product(result->constant(2.0f), uint32_t(i))));
                break;
            case MathOp::Sin:
                if (depends[n.a])
                    add(n.a, result->product(d, result->unary(MathOp::Cos, n.a)));
                break;
            case MathOp::Cos:
                if (depends[n.a])
                    add(n.a, result->product(result->negative(d), result->unary(MathOp::Sin, n.a)));
                break;
            case MathOp::Exp:
                if (depends[n.a])
                    add(n.a, result->product(d, uint32_t(i)));
                break;
            case MathOp::Log:
                if (depends[n.a])
                    add(n.a, result->quotient(d, n.a));
                break;
            case MathOp::Atan2: {
                const auto denominator =
                    result->sum(result->product(n.a, n.a), result->product(n.b, n.b));
                if (depends[n.a])
                    add(n.a, result->quotient(result->product(d, n.b), denominator));
                if (depends[n.b])
                    add(n.b, result->quotient(result->product(result->negative(d), n.a), denominator));
                break;
            }
            default:
                break;
            }
        }
    }
    const auto zero = result->constant(0.0f);
    for (auto& value : result->values)
        if (value == ZeroDerivative)
            value = zero;
    return result;
}
std::string emit(const Formula& inputFormula, const std::string& name,
                 const std::vector<uint32_t>* derivativeInputs, bool emitValues) {
    const auto formula = simplify(inputFormula);
    const auto inputs = formula.inputs;
    const auto& nodes = formula.nodes;
    const auto& outputs = formula.outputs;
    std::vector<int32_t> derivativeColumn(inputs, -1);
    if (derivativeInputs)
        for (uint32_t column = 0; column < derivativeInputs->size(); ++column) {
            auto input = derivativeInputs->at(column);
            if (input >= inputs || derivativeColumn[input] >= 0)
                throw std::invalid_argument("Invalid projected derivative inputs");
            derivativeColumn[input] = int32_t(column);
        }
    std::unique_ptr<SymbolicJacobian> symbolic;
    if (derivativeInputs && supportsSymbolicDerivative(formula))
        symbolic = buildSymbolicJacobian(formula, derivativeColumn, derivativeInputs->size());
    const auto& generatedNodes = symbolic ? symbolic->nodes : nodes;
    std::vector<uint8_t> required(generatedNodes.size());
    if (emitValues)
        for (auto output : outputs)
            required[output] = 1;
    if (symbolic)
        for (auto output : symbolic->values)
            required[output] = 1;
    // The fallback reverse pass consumes the primal output graph.
    if (derivativeInputs && !symbolic)
        for (auto output : outputs)
            required[output] = 1;
    for (size_t i = required.size(); i-- > 0;)
        if (required[i]) {
            const auto& node = generatedNodes[i];
            const auto count = arity(node.op);
            if (count > 0)
                required[node.a] = 1;
            if (count > 1)
                required[node.b] = 1;
            if (count > 2)
                required[node.c] = 1;
        }
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << "void " << name << "(in float x[" << std::max(1u, inputs) << "]";
    if (emitValues)
        s << ", out float y[" << outputs.size() << "]";
    if (derivativeInputs)
        s << ", out float j[" << std::max(size_t(1), outputs.size() * derivativeInputs->size()) << "]";
    s << ") {\n";
    for (size_t i = 0; i < generatedNodes.size(); ++i) {
        if (!required[i])
            continue;
        const auto& n = generatedNodes[i];
        auto a = "n" + std::to_string(n.a), b = "n" + std::to_string(n.b), c = "n" + std::to_string(n.c);
        std::string expr;
        switch (n.op) {
        case MathOp::Constant:
            expr = num(n.value);
            break;
        case MathOp::Input:
            expr = "x[" + std::to_string(n.a) + "]";
            break;
        case MathOp::Add:
            expr = a + "+" + b;
            break;
        case MathOp::Subtract:
            expr = a + "-" + b;
            break;
        case MathOp::Multiply:
            expr = a + "*" + b;
            break;
        case MathOp::Divide:
            expr = a + "/" + b;
            break;
        case MathOp::Negate:
            expr = "-" + a;
            break;
        case MathOp::Sqrt:
            expr = "sqrt(" + a + ")";
            break;
        case MathOp::Sin:
            expr = "sin(" + a + ")";
            break;
        case MathOp::Cos:
            expr = "cos(" + a + ")";
            break;
        case MathOp::Exp:
            expr = "exp(" + a + ")";
            break;
        case MathOp::Log:
            expr = "log(" + a + ")";
            break;
        case MathOp::Abs:
            expr = "abs(" + a + ")";
            break;
        case MathOp::Min:
            expr = "min(" + a + "," + b + ")";
            break;
        case MathOp::Max:
            expr = "max(" + a + "," + b + ")";
            break;
        case MathOp::Atan2:
            expr = "atan(" + a + "," + b + ")";
            break;
        case MathOp::Less:
            if (symbolic && symbolic->comparisons.count(uint32_t(i)))
                expr = "(" + a + (symbolic->comparisons.at(uint32_t(i)) == SymbolicJacobian::Comparison::LessEqual ?
                    "<=" : ">=") + b + " ? 1.0 : 0.0)";
            else
                expr = "(" + a + "<" + b + " ? 1.0 : 0.0)";
            break;
        case MathOp::Select:
            expr = "(" + a + "!=0.0 ? " + b + " : " + c + ")";
            break;
        }
        s << "float n" << i << "=" << expr << ";\n";
    }
    if (emitValues)
        for (size_t row = 0; row < outputs.size(); ++row)
            s << "y[" << row << "]=n" << outputs[row] << ";\n";
    if (symbolic) {
        for (size_t i = 0; i < symbolic->values.size(); ++i)
            s << "j[" << i << "]=n" << symbolic->values[i] << ";\n";
    } else if (derivativeInputs) {
        // Keep only paths from this formula's outputs to the requested inputs.
        // Parameter/history/time-only branches still compute values, but generate no
        // adjoint storage or instructions.
        std::vector<uint8_t> depends(nodes.size());
        for (size_t i = 0; i < nodes.size(); ++i) {
            const auto& n = nodes[i];
            if (n.op == MathOp::Input)
                depends[i] = derivativeColumn[n.a] >= 0;
            else if (n.op == MathOp::Constant || n.op == MathOp::Less)
                depends[i] = 0;
            else if (n.op == MathOp::Select)
                depends[i] = depends[n.b] || depends[n.c];
            else {
                auto count = arity(n.op);
                depends[i] = (count > 0 && depends[n.a]) || (count > 1 && depends[n.b]);
            }
        }
        for (size_t row = 0; row < outputs.size(); ++row) {
            for (size_t input = 0; input < derivativeInputs->size(); ++input)
                s << "j[" << row * derivativeInputs->size() + input << "]=0.0;\n";
            std::vector<uint8_t> active(nodes.size());
            active[outputs[row]] = depends[outputs[row]];
            for (size_t i = nodes.size(); i-- > 0;)
                if (active[i]) {
                    const auto& n = nodes[i];
                    auto activate = [&](uint32_t child) {
                        if (depends[child])
                            active[child] = 1;
                    };
                    if (n.op == MathOp::Select) {
                        activate(n.b);
                        activate(n.c);
                    } else if (n.op != MathOp::Constant && n.op != MathOp::Input &&
                               n.op != MathOp::Less) {
                        auto count = arity(n.op);
                        if (count > 0)
                            activate(n.a);
                        if (count > 1)
                            activate(n.b);
                    }
                }
            if (!active[outputs[row]])
                continue;
            s << "{\n";
            for (size_t i = 0; i < nodes.size(); ++i)
                if (active[i])
                    s << "float g" << i << "=" << (i == outputs[row] ? "1.0" : "0.0") << ";\n";
            for (size_t i = nodes.size(); i-- > 0;) {
                const auto& n = nodes[i];
                if (!active[i] || n.op == MathOp::Constant || n.op == MathOp::Less)
                    continue;
                auto a = "n" + std::to_string(n.a), b = "n" + std::to_string(n.b),
                     d = "g" + std::to_string(i);
                auto add = [&](uint32_t index, const std::string& term) {
                    if (active[index])
                        s << "g" << index << " += " << term << ";\n";
                };
                // Unselected branches have zero adjoint. Do not evaluate their singular derivatives.
                s << "if (" << d << " != 0.0) {\n";
                switch (n.op) {
                case MathOp::Input:
                    s << "j[" << row * derivativeInputs->size() + derivativeColumn[n.a] << "] += " << d
                      << ";\n";
                    break;
                case MathOp::Add:
                    add(n.a, d);
                    add(n.b, d);
                    break;
                case MathOp::Subtract:
                    add(n.a, d);
                    add(n.b, "-" + d);
                    break;
                case MathOp::Multiply:
                    add(n.a, d + "*" + b);
                    add(n.b, d + "*" + a);
                    break;
                case MathOp::Divide:
                    add(n.a, d + "/" + b);
                    add(n.b, "-" + d + "*" + a + "/(" + b + "*" + b + ")");
                    break;
                case MathOp::Negate:
                    add(n.a, "-" + d);
                    break;
                case MathOp::Sqrt:
                    add(n.a, d + "/(2.0*n" + std::to_string(i) + ")");
                    break;
                case MathOp::Sin:
                    add(n.a, d + "*cos(" + a + ")");
                    break;
                case MathOp::Cos:
                    add(n.a, "-" + d + "*sin(" + a + ")");
                    break;
                case MathOp::Exp:
                    add(n.a, d + "*n" + std::to_string(i));
                    break;
                case MathOp::Log:
                    add(n.a, d + "/" + a);
                    break;
                case MathOp::Abs:
                    add(n.a, d + "*sign(" + a + ")");
                    break;
                case MathOp::Min:
                case MathOp::Max:
                    if (active[n.a] && active[n.b]) {
                        s << "if (" << a << (n.op == MathOp::Min ? " <= " : " >= ") << b << ") {\n";
                        add(n.a, d);
                        s << "} else {\n";
                        add(n.b, d);
                        s << "}\n";
                    } else if (active[n.a]) {
                        s << "if (" << a << (n.op == MathOp::Min ? " <= " : " >= ") << b << ") {\n";
                        add(n.a, d);
                        s << "}\n";
                    } else if (active[n.b]) {
                        s << "if (" << a << (n.op == MathOp::Min ? " > " : " < ") << b << ") {\n";
                        add(n.b, d);
                        s << "}\n";
                    }
                    break;
                case MathOp::Atan2:
                    add(n.a, d + "*" + b + "/(" + a + "*" + a + "+" + b + "*" + b + ")");
                    add(n.b, "-" + d + "*" + a + "/(" + a + "*" + a + "+" + b + "*" + b + ")");
                    break;
                case MathOp::Select:
                    if (active[n.b] && active[n.c]) {
                        s << "if (" << a << "!=0.0) {\n";
                        add(n.b, d);
                        s << "} else {\n";
                        add(n.c, d);
                        s << "}\n";
                    } else if (active[n.b]) {
                        s << "if (" << a << "!=0.0) {\n";
                        add(n.b, d);
                        s << "}\n";
                    } else if (active[n.c]) {
                        s << "if (" << a << "==0.0) {\n";
                        add(n.c, d);
                        s << "}\n";
                    }
                    break;
                default:
                    break;
                }
                s << "}\n";
            }
            s << "}\n";
        }
    }
    s << "}\n";
    return s.str();
}
} // namespace
std::string emitGlsl(const Formula& formula, const std::string& name) {
    return emit(formula, name, nullptr, true);
}
std::string emitGlslDerivative(const Formula& formula, const std::string& name,
                               const std::vector<uint32_t>& derivativeInputs) {
    return emit(formula, name, &derivativeInputs, true);
}
std::string emitGlslJacobian(const Formula& formula, const std::string& name,
                            const std::vector<uint32_t>& derivativeInputs) {
    return emit(formula, name, &derivativeInputs, false);
}
std::optional<std::vector<float>> constantJacobian(
    const Formula& inputFormula, const std::vector<uint32_t>& derivativeInputs) {
    const auto formula = simplify(inputFormula);
    if (!supportsSymbolicDerivative(formula))
        return {};
    std::vector<int32_t> derivativeColumn(formula.inputs, -1);
    for (uint32_t column = 0; column < derivativeInputs.size(); ++column) {
        const auto input = derivativeInputs[column];
        if (input >= formula.inputs || derivativeColumn[input] >= 0)
            throw std::invalid_argument("Invalid projected derivative inputs");
        derivativeColumn[input] = int32_t(column);
    }
    auto symbolic = buildSymbolicJacobian(formula, derivativeColumn, derivativeInputs.size());
    std::vector<float> result;
    result.reserve(symbolic->values.size());
    for (auto id : symbolic->values) {
        if (symbolic->nodes[id].op != MathOp::Constant)
            return {};
        result.push_back(symbolic->nodes[id].value);
    }
    return result;
}
std::optional<DifferenceFactor> factorDifferenceNorm(
    const Formula& source, uint32_t stateInputs,
    const std::vector<std::pair<uint32_t, uint32_t>>& coordinates) {
    const auto formula = simplify(source);
    auto ordered = [](auto pairs) {
        for (auto& pair : pairs) if (pair.second < pair.first) std::swap(pair.first, pair.second);
        std::sort(pairs.begin(), pairs.end());
        return pairs;
    };
    const auto expected = ordered(coordinates);
    if (expected.empty()) return {};
    for (uint32_t root = 0; root < formula.nodes.size(); ++root) {
        std::vector<std::pair<uint32_t, uint32_t>> found;
        std::vector<uint32_t> pending{root};
        bool norm = true;
        while (!pending.empty() && norm) {
            const auto node = formula.nodes[pending.back()]; pending.pop_back();
            if (node.op == MathOp::Constant && node.value == 0) continue;
            if (node.op == MathOp::Add) { pending.push_back(node.a); pending.push_back(node.b); continue; }
            if (node.op != MathOp::Multiply || node.a != node.b) { norm = false; break; }
            const auto difference = formula.nodes[node.a];
            if (difference.op != MathOp::Subtract) { norm = false; break; }
            const auto a = formula.nodes[difference.a], b = formula.nodes[difference.b];
            if (a.op != MathOp::Input || b.op != MathOp::Input) { norm = false; break; }
            found.emplace_back(a.a, b.a);
        }
        if (!norm || ordered(found) != expected) continue;
        auto scalar = formula;
        scalar.nodes[root] = {MathOp::Input, scalar.inputs++};
        scalar = simplify(scalar);
        if (std::any_of(scalar.nodes.begin(), scalar.nodes.end(), [&](const MathNode& node) {
                return node.op == MathOp::Input && node.a < stateInputs;
            })) continue;
        return DifferenceFactor{std::move(scalar), formula.inputs, coordinates};
    }
    return {};
}
std::optional<DifferenceBound> differenceBound(const Formula& source, bool nonnegative,
                                              uint32_t parameterFirst, uint32_t parameterCount) {
    auto formula = simplify(source);
    if (formula.outputs.size() != 1)
        return {};
    const auto& root = formula.nodes[formula.outputs[0]];
    if (root.op != MathOp::Subtract)
        return {};
    auto sum = nonnegative ? root.a : root.b;
    auto radius = nonnegative ? root.b : root.a;
    if (formula.nodes[sum].op == MathOp::Divide) {
        const auto quotient = formula.nodes[sum];
        // A normalized quadratic bound is valid only for a positive scale.
        // Invalid parameter values select the existing full-domain fallback.
        const auto product = uint32_t(formula.nodes.size());
        formula.nodes.push_back({MathOp::Multiply, radius, quotient.b});
        const auto zero = uint32_t(formula.nodes.size());
        formula.nodes.push_back({MathOp::Constant, 0, 0, 0, 0});
        const auto positive = uint32_t(formula.nodes.size());
        formula.nodes.push_back({MathOp::Less, zero, quotient.b});
        const auto invalid = uint32_t(formula.nodes.size());
        formula.nodes.push_back({MathOp::Constant, 0, 0, 0, -1});
        radius = uint32_t(formula.nodes.size());
        formula.nodes.push_back({MathOp::Select, positive, product, invalid});
        sum = quotient.a;
    }
    DifferenceBound result;
    std::vector<uint32_t> pending{sum};
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        const auto& node = formula.nodes[id];
        if (node.op == MathOp::Constant && node.value == 0)
            continue;
        if (node.op == MathOp::Add) {
            pending.push_back(node.b);
            pending.push_back(node.a);
            continue;
        }
        if (node.op != MathOp::Multiply || node.a != node.b)
            return {};
        const auto& difference = formula.nodes[node.a];
        if (difference.op != MathOp::Subtract)
            return {};
        const auto& a = formula.nodes[difference.a];
        const auto& b = formula.nodes[difference.b];
        if (a.op != MathOp::Input || b.op != MathOp::Input)
            return {};
        result.coordinates.emplace_back(a.a, b.a);
    }
    if (result.coordinates.empty())
        return {};
    pending = {radius};
    std::vector<bool> visited(formula.nodes.size());
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        if (visited[id])
            continue;
        visited[id] = true;
        const auto& node = formula.nodes[id];
        if (node.op == MathOp::Input &&
            (node.a < parameterFirst || node.a - parameterFirst >= parameterCount))
            return {};
        const auto count = arity(node.op);
        if (count > 0) pending.push_back(node.a);
        if (count > 1) pending.push_back(node.b);
        if (count > 2) pending.push_back(node.c);
    }
    formula.outputs = {radius};
    result.squaredRadius = std::move(formula);
    return result;
}
std::optional<DifferenceBound> supportBound(const Formula& source, uint32_t parameterFirst,
                                           uint32_t parameterCount) {
    const auto formula = simplify(source);
    auto zero = [&](uint32_t id) {
        return formula.nodes[id].op == MathOp::Constant && formula.nodes[id].value == 0;
    };
    std::optional<DifferenceBound> result;
    std::string signature;
    for (auto output : formula.outputs) {
        std::vector<uint32_t> pending{output};
        std::optional<DifferenceBound> bound;
        while (!pending.empty() && !bound) {
            const auto node = formula.nodes[pending.back()]; pending.pop_back();
            if (node.op == MathOp::Multiply) {
                pending.push_back(node.a); pending.push_back(node.b);
            } else if (node.op == MathOp::Negate) {
                pending.push_back(node.a);
            } else if ((node.op == MathOp::Max || node.op == MathOp::Min) && (zero(node.a) || zero(node.b))) {
                auto part = formula;
                part.outputs = {zero(node.a) ? node.b : node.a};
                bound = differenceBound(part, node.op == MathOp::Min, parameterFirst, parameterCount);
            }
        }
        if (!bound) return {};
        const auto key = emitGlsl(bound->squaredRadius, "supportRadius");
        if (result && (result->coordinates != bound->coordinates || signature != key)) return {};
        result = std::move(bound); signature = key;
    }
    return result;
}
} // namespace whimsical::dynamics
