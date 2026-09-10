#include "FormulaGlsl.h"
#include <algorithm>
#include <iomanip>
#include <locale>
#include <sstream>

namespace whimsical::dynamics {
namespace {
std::string num(float value) {
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << std::scientific << std::setprecision(9) << value;
    return s.str();
}
} // namespace
std::string emitGlsl(const Formula& formula, const std::string& name, bool derivatives) {
    const auto inputs = formula.inputs;
    const auto& nodes = formula.nodes;
    const auto& outputs = formula.outputs;
    formula.validate();
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << "void " << name << "(in float x[" << std::max(1u, inputs) << "], out float y[" << outputs.size()
      << "]";
    if (derivatives)
        s << ", out float j[" << std::max(size_t(1), outputs.size() * inputs) << "]";
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
    if (derivatives)
        for (size_t row = 0; row < outputs.size(); ++row) {
            s << "{\n";
            for (uint32_t input = 0; input < inputs; ++input)
                s << "j[" << row * inputs + input << "]=0.0;\n";
            for (size_t i = 0; i < nodes.size(); ++i)
                s << "float g" << i << "=" << (i == outputs[row] ? "1.0" : "0.0") << ";\n";
            for (size_t i = nodes.size(); i-- > 0;) {
                const auto& n = nodes[i];
                if (n.op == MathOp::Constant || n.op == MathOp::Less)
                    continue;
                auto a = "n" + std::to_string(n.a), b = "n" + std::to_string(n.b),
                     d = "g" + std::to_string(i);
                auto add = [&](uint32_t index, const std::string& term) {
                    s << "g" << index << " += " << term << ";\n";
                };
                // Unselected branches have zero adjoint. Do not evaluate their singular derivatives.
                s << "if (" << d << " != 0.0) {\n";
                switch (n.op) {
                case MathOp::Input:
                    s << "j[" << row * inputs + n.a << "] += " << d << ";\n";
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
                    s << "if (" << a << (n.op == MathOp::Min ? " <= " : " >= ") << b << ") {\n";
                    add(n.a, d);
                    s << "} else {\n";
                    add(n.b, d);
                    s << "}\n";
                    break;
                case MathOp::Atan2:
                    add(n.a, d + "*" + b + "/(" + a + "*" + a + "+" + b + "*" + b + ")");
                    add(n.b, "-" + d + "*" + a + "/(" + a + "*" + a + "+" + b + "*" + b + ")");
                    break;
                case MathOp::Select:
                    s << "if (" << a << "!=0.0) {\n";
                    add(n.b, d);
                    s << "} else {\n";
                    add(n.c, d);
                    s << "}\n";
                    break;
                default:
                    break;
                }
                s << "}\n";
            }
            s << "}\n";
        }
    s << "}\n";
    return s.str();
}
} // namespace whimsical::dynamics