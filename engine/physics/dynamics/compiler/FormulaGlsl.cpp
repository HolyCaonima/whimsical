#include "FormulaGlsl.h"
#include <algorithm>
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
struct SymbolicJacobian {
    std::vector<MathNode> nodes;
    std::vector<uint32_t> values;
    std::map<NodeKey, uint32_t> common;

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
    uint32_t constant(float value) {
        return raw({MathOp::Constant, 0, 0, 0, value});
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
        if (nodes[a].op == MathOp::Negate)
            return nodes[a].a;
        return unary(MathOp::Negate, a);
    }
    uint32_t sum(uint32_t a, uint32_t b) {
        if (a == ZeroDerivative || is(a, 0.0f))
            return b;
        if (b == ZeroDerivative || is(b, 0.0f))
            return a;
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
        return raw({MathOp::Multiply, a, b});
    }
    uint32_t quotient(uint32_t a, uint32_t b) {
        if (a == ZeroDerivative || is(a, 0.0f))
            return ZeroDerivative;
        if (is(b, 1.0f))
            return a;
        return raw({MathOp::Divide, a, b});
    }
};
bool supportsSymbolicDerivative(const Formula& formula) {
    return std::none_of(formula.nodes.begin(), formula.nodes.end(), [](const MathNode& node) {
        return node.op == MathOp::Abs || node.op == MathOp::Min || node.op == MathOp::Max ||
               node.op == MathOp::Select;
    });
}
std::unique_ptr<SymbolicJacobian> buildSymbolicJacobian(
    const Formula& formula,
    const std::vector<int32_t>& derivativeColumn,
    size_t columns) {
    auto result = std::make_unique<SymbolicJacobian>(formula);
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
std::string emit(const Formula& formula, const std::string& name,
                 const std::vector<uint32_t>* derivativeInputs) {
    const auto inputs = formula.inputs;
    const auto& nodes = formula.nodes;
    const auto& outputs = formula.outputs;
    formula.validate();
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
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << "void " << name << "(in float x[" << std::max(1u, inputs) << "], out float y[" << outputs.size()
      << "]";
    if (derivativeInputs)
        s << ", out float j[" << std::max(size_t(1), outputs.size() * derivativeInputs->size()) << "]";
    s << ") {\n";
    for (size_t i = 0; i < generatedNodes.size(); ++i) {
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
            expr = "(" + a + "<" + b + " ? 1.0 : 0.0)";
            break;
        case MathOp::Select:
            expr = "(" + a + "!=0.0 ? " + b + " : " + c + ")";
            break;
        }
        s << "float n" << i << "=" << expr << ";\n";
    }
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
    return emit(formula, name, nullptr);
}
std::string emitGlslDerivative(const Formula& formula, const std::string& name,
                               const std::vector<uint32_t>& derivativeInputs) {
    return emit(formula, name, &derivativeInputs);
}
} // namespace whimsical::dynamics