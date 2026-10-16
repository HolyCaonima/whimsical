#include "Expression.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
#include <tuple>

namespace whimsical::dynamics {
struct ExpressionData {
    uint32_t inputs;
    std::vector<MathNode> nodes;
    std::map<std::tuple<MathOp, uint32_t, uint32_t, uint32_t, uint32_t>, uint32_t> common;
    uint32_t add(MathNode n) {
        uint32_t bits;
        std::memcpy(&bits, &n.value, 4);
        auto key = std::make_tuple(n.op, n.a, n.b, n.c, bits);
        auto found = common.find(key);
        if (found != common.end())
            return found->second;
        auto index = uint32_t(nodes.size());
        nodes.push_back(n);
        common.emplace(key, index);
        return index;
    }
};
namespace {
Scalar constantLike(Scalar x, float value) {
    if (!std::isfinite(value))
        throw std::invalid_argument("Expression constant must be finite");
    return {x.expression, x.expression->add({MathOp::Constant, 0, 0, 0, value})};
}
Scalar unary(MathOp op, Scalar a) {
    return {a.expression, a.expression->add({op, a.node})};
}
Scalar binary(MathOp op, Scalar a, Scalar b) {
    if (a.expression != b.expression)
        throw std::invalid_argument("Expressions belong to different definitions");
    return {a.expression, a.expression->add({op, a.node, b.node})};
}
uint32_t arity(MathOp op) {
    switch (op) {
    case MathOp::Input:
    case MathOp::Constant:
        return 0;
    case MathOp::Negate:
    case MathOp::Sqrt:
    case MathOp::Sin:
    case MathOp::Cos:
    case MathOp::Exp:
    case MathOp::Log:
    case MathOp::Abs:
    case MathOp::Sum:
        return 1;
    case MathOp::Select:
        return 3;
    default:
        return 2;
    }
}
// Evaluates every node for count points: node i of point l lands at v[i * stride + l],
// and input(k, l) supplies input k of point l.
template <class Input>
void evaluateNodes(const Formula& f, uint32_t count, size_t stride, const Input& input, double* v) {
    for (size_t i = 0; i < f.nodes.size(); ++i) {
        const auto& n = f.nodes[i];
        double* r = v + i * stride;
        const auto operand = [&](uint32_t node) { return v + node * stride; };
        const auto unary = [&](auto op) {
            const double* a = operand(n.a);
            for (uint32_t l = 0; l < count; ++l)
                r[l] = op(a[l]);
        };
        const auto binary = [&](auto op) {
            const double *a = operand(n.a), *b = operand(n.b);
            for (uint32_t l = 0; l < count; ++l)
                r[l] = op(a[l], b[l]);
        };
        switch (n.op) {
        case MathOp::Constant:
            std::fill(r, r + count, double(n.value));
            break;
        case MathOp::Input:
            for (uint32_t l = 0; l < count; ++l)
                r[l] = input(n.a, l);
            break;
        case MathOp::Add:
            binary([](double a, double b) { return a + b; });
            break;
        case MathOp::Subtract:
            binary([](double a, double b) { return a - b; });
            break;
        case MathOp::Multiply:
            binary([](double a, double b) { return a * b; });
            break;
        case MathOp::Divide:
            binary([](double a, double b) { return a / b; });
            break;
        case MathOp::Negate:
            unary([](double a) { return -a; });
            break;
        case MathOp::Sqrt:
            unary([](double a) { return std::sqrt(a); });
            break;
        case MathOp::Sin:
            unary([](double a) { return std::sin(a); });
            break;
        case MathOp::Cos:
            unary([](double a) { return std::cos(a); });
            break;
        case MathOp::Exp:
            unary([](double a) { return std::exp(a); });
            break;
        case MathOp::Log:
            unary([](double a) { return std::log(a); });
            break;
        case MathOp::Abs:
            unary([](double a) { return std::abs(a); });
            break;
        case MathOp::Min:
            binary([](double a, double b) { return std::min(a, b); });
            break;
        case MathOp::Max:
            binary([](double a, double b) { return std::max(a, b); });
            break;
        case MathOp::Atan2:
            binary([](double a, double b) { return std::atan2(a, b); });
            break;
        case MathOp::Less:
            binary([](double a, double b) { return a < b ? 1.0 : 0.0; });
            break;
        case MathOp::Select: {
            const double *a = operand(n.a), *b = operand(n.b), *c = operand(n.c);
            for (uint32_t l = 0; l < count; ++l)
                r[l] = a[l] != 0 ? b[l] : c[l];
            break;
        }
        case MathOp::Sum:
            throw std::invalid_argument("A collection sum requires bound object members");
        }
    }
}
std::vector<double> values(const Formula& f, const std::vector<double>& x) {
    if (x.size() != f.inputs)
        throw std::invalid_argument("Expression input dimension mismatch");
    std::vector<double> v(f.nodes.size());
    evaluateNodes(f, 1, 1, [&](uint32_t k, uint32_t) { return x[k]; }, v.data());
    return v;
}
} // namespace
Expression::Expression(uint32_t count) : data_(std::make_shared<ExpressionData>()) {
    data_->inputs = count;
}
Scalar Expression::input(uint32_t index) const {
    if (index >= data_->inputs)
        throw std::out_of_range("Expression input index");
    return {data_, data_->add({MathOp::Input, index})};
}
Vector Expression::inputs(uint32_t first, uint32_t count) const {
    Vector result;
    result.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
        result.push_back(input(first + i));
    return result;
}
Scalar Expression::constant(float value) const {
    return constantLike({data_, 0}, value);
}
Formula Expression::finish(const Vector& results) const {
    Formula f;
    f.inputs = data_->inputs;
    // Prune dead construction nodes so an unused singular expression is never evaluated.
    std::vector<uint8_t> used(data_->nodes.size());
    for (const auto& result : results) {
        if (result.expression != data_)
            throw std::invalid_argument("Expression result owner mismatch");
        used.at(result.node) = 1;
    }
    for (size_t i = used.size(); i-- > 0;)
        if (used[i]) {
            const auto& n = data_->nodes[i];
            const auto count = arity(n.op);
            if (count > 0)
                used[n.a] = 1;
            if (count > 1)
                used[n.b] = 1;
            if (count > 2)
                used[n.c] = 1;
        }
    std::vector<uint32_t> map(used.size());
    for (size_t i = 0; i < used.size(); ++i)
        if (used[i]) {
            auto n = data_->nodes[i];
            const auto count = arity(n.op);
            if (count > 0)
                n.a = map[n.a];
            if (count > 1)
                n.b = map[n.b];
            if (count > 2)
                n.c = map[n.c];
            map[i] = uint32_t(f.nodes.size());
            f.nodes.push_back(n);
        }
    for (auto result : results)
        f.outputs.push_back(map[result.node]);
    return f;
}
void Formula::validate() const {
    if (outputs.empty())
        throw std::invalid_argument("Formula requires outputs");
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& n = nodes[i];
        const auto count = arity(n.op);
        if (uint32_t(n.op) > uint32_t(MathOp::Sum) || (n.op == MathOp::Sum && n.b > 1) ||
            (count > 0 && n.a >= i) || (count > 1 && n.b >= i) ||
            (count > 2 && n.c >= i) || (n.op == MathOp::Input && n.a >= inputs) || !std::isfinite(n.value))
            throw std::invalid_argument("Invalid expression DAG");
    }
    for (auto output : outputs)
        if (output >= nodes.size())
            throw std::invalid_argument("Invalid expression output");
}
std::vector<double> Formula::evaluate(const std::vector<double>& x) const {
    const auto v = values(*this, x);
    std::vector<double> result;
    for (auto output : outputs)
        result.push_back(v[output]);
    return result;
}
std::vector<float> Formula::evaluate(uint32_t count, const std::vector<Column>& columns) const {
    if (columns.size() != inputs)
        throw std::invalid_argument("Expression input dimension mismatch");
    constexpr uint32_t lanes = 64;
    std::vector<double> v(nodes.size() * lanes);
    std::vector<float> result(size_t(count) * outputs.size());
    auto* out = result.data();
    for (uint32_t first = 0; first < count; first += lanes) {
        const auto n = std::min(lanes, count - first);
        evaluateNodes(
            *this, n, lanes,
            [&](uint32_t k, uint32_t l) { return double(columns[k].first[(first + l) * columns[k].stride]); },
            v.data());
        for (uint32_t l = 0; l < n; ++l)
            for (auto output : outputs)
                *out++ = float(v[output * lanes + l]);
    }
    return result;
}
std::vector<double> Formula::jacobian(const std::vector<double>& x) const {
    const auto v = values(*this, x);
    std::vector<double> result(outputs.size() * inputs), g(nodes.size());
    for (size_t row = 0; row < outputs.size(); ++row) {
        std::fill(g.begin(), g.end(), 0);
        g[outputs[row]] = 1;
        for (size_t i = nodes.size(); i-- > 0;) {
            const auto& n = nodes[i];
            const auto d = g[i];
            if (d == 0)
                continue;
            const auto a = arity(n.op) ? v[n.a] : 0, b = arity(n.op) > 1 ? v[n.b] : 0;
            switch (n.op) {
            case MathOp::Input:
                result[row * inputs + n.a] += d;
                break;
            case MathOp::Add:
                g[n.a] += d;
                g[n.b] += d;
                break;
            case MathOp::Subtract:
                g[n.a] += d;
                g[n.b] -= d;
                break;
            case MathOp::Multiply:
                g[n.a] += d * b;
                g[n.b] += d * a;
                break;
            case MathOp::Divide:
                g[n.a] += d / b;
                g[n.b] -= d * a / (b * b);
                break;
            case MathOp::Negate:
                g[n.a] -= d;
                break;
            case MathOp::Sqrt:
                g[n.a] += d / (2 * v[i]);
                break;
            case MathOp::Sin:
                g[n.a] += d * std::cos(a);
                break;
            case MathOp::Cos:
                g[n.a] -= d * std::sin(a);
                break;
            case MathOp::Exp:
                g[n.a] += d * v[i];
                break;
            case MathOp::Log:
                g[n.a] += d / a;
                break;
            case MathOp::Abs:
                g[n.a] += a > 0 ? d : a < 0 ? -d : 0;
                break;
            case MathOp::Min:
                g[a <= b ? n.a : n.b] += d;
                break;
            case MathOp::Max:
                g[a >= b ? n.a : n.b] += d;
                break;
            case MathOp::Atan2:
                g[n.a] += d * b / (a * a + b * b);
                g[n.b] -= d * a / (a * a + b * b);
                break;
            case MathOp::Select:
                g[a != 0 ? n.b : n.c] += d;
                break;
            default:
                break;
            }
        }
    }
    return result;
}
Scalar operator+(Scalar a, Scalar b) {
    return binary(MathOp::Add, a, b);
}
Scalar operator-(Scalar a, Scalar b) {
    return binary(MathOp::Subtract, a, b);
}
Scalar operator*(Scalar a, Scalar b) {
    return binary(MathOp::Multiply, a, b);
}
Scalar operator/(Scalar a, Scalar b) {
    return binary(MathOp::Divide, a, b);
}
Scalar operator-(Scalar a) {
    return unary(MathOp::Negate, a);
}
Scalar operator+(Scalar a, float b) {
    return a + constantLike(a, b);
}
Scalar operator-(Scalar a, float b) {
    return a - constantLike(a, b);
}
Scalar operator*(Scalar a, float b) {
    return a * constantLike(a, b);
}
Scalar operator/(Scalar a, float b) {
    return a / constantLike(a, b);
}
Scalar operator+(float a, Scalar b) {
    return constantLike(b, a) + b;
}
Scalar operator-(float a, Scalar b) {
    return constantLike(b, a) - b;
}
Scalar operator*(float a, Scalar b) {
    return constantLike(b, a) * b;
}
Scalar operator/(float a, Scalar b) {
    return constantLike(b, a) / b;
}
Scalar sqrt(Scalar a) {
    return unary(MathOp::Sqrt, a);
}
Scalar sin(Scalar a) {
    return unary(MathOp::Sin, a);
}
Scalar cos(Scalar a) {
    return unary(MathOp::Cos, a);
}
Scalar exp(Scalar a) {
    return unary(MathOp::Exp, a);
}
Scalar log(Scalar a) {
    return unary(MathOp::Log, a);
}
Scalar abs(Scalar a) {
    return unary(MathOp::Abs, a);
}
Scalar min(Scalar a, Scalar b) {
    return binary(MathOp::Min, a, b);
}
Scalar max(Scalar a, Scalar b) {
    return binary(MathOp::Max, a, b);
}
Scalar atan2(Scalar a, Scalar b) {
    return binary(MathOp::Atan2, a, b);
}
Scalar less(Scalar a, Scalar b) {
    return binary(MathOp::Less, a, b);
}
Scalar select(Scalar a, Scalar b, Scalar c) {
    if (a.expression != b.expression || a.expression != c.expression)
        throw std::invalid_argument("Expression owner mismatch");
    return {a.expression, a.expression->add({MathOp::Select, a.node, b.node, c.node})};
}
Scalar sum(Scalar contribution, uint32_t object) {
    if (object > 1)
        throw std::invalid_argument("Sum requires formal object 0 or 1");
    return {contribution.expression, contribution.expression->add({MathOp::Sum, contribution.node, object})};
}
Vector sum(const Vector& contribution, uint32_t object) {
    Vector result;
    for (auto value : contribution) result.push_back(sum(value, object));
    return result;
}
Vector operator+(const Vector& a, const Vector& b) {
    if (a.size() != b.size())
        throw std::invalid_argument("Vector dimension mismatch");
    Vector r;
    for (size_t i = 0; i < a.size(); ++i)
        r.push_back(a[i] + b[i]);
    return r;
}
Vector operator-(const Vector& a, const Vector& b) {
    if (a.size() != b.size())
        throw std::invalid_argument("Vector dimension mismatch");
    Vector r;
    for (size_t i = 0; i < a.size(); ++i)
        r.push_back(a[i] - b[i]);
    return r;
}
Vector operator*(const Vector& a, Scalar b) {
    Vector r;
    for (auto x : a)
        r.push_back(x * b);
    return r;
}
Vector operator*(const Vector& a, float b) {
    Vector r;
    for (auto x : a)
        r.push_back(x * b);
    return r;
}
Scalar dot(const Vector& a, const Vector& b) {
    if (a.empty() || a.size() != b.size())
        throw std::invalid_argument("Dot product dimension mismatch");
    auto r = a[0] * b[0];
    for (size_t i = 1; i < a.size(); ++i)
        r = r + a[i] * b[i];
    return r;
}
Scalar length(const Vector& a) {
    return sqrt(dot(a, a));
}
Vector cross(const Vector& a, const Vector& b) {
    if (a.size() != 3 || b.size() != 3)
        throw std::invalid_argument("Cross product requires three components");
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
} // namespace whimsical::dynamics
