#include "FormulaGlsl.h"
#include <algorithm>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>

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
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << "void " << name << "(in float x[" << std::max(1u, inputs) << "], out float y[" << outputs.size()
      << "]";
    if (derivativeInputs)
        s << ", out float j[" << std::max(size_t(1), outputs.size() * derivativeInputs->size()) << "]";
    s << ") {\n";
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& n = nodes[i];
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
    if (derivativeInputs) {
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