#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace whimsical::xpbd {
enum class MathOp : uint8_t {
    Constant,
    Input,
    Add,
    Subtract,
    Multiply,
    Divide,
    Negate,
    Sqrt,
    Sin,
    Cos,
    Exp,
    Log,
    Abs,
    Min,
    Max,
    Atan2,
    Less,
    Select
};
struct MathNode {
    MathOp op;
    uint32_t a = 0, b = 0, c = 0;
    float value = 0;
};
struct ExpressionData;
struct Scalar {
    std::shared_ptr<ExpressionData> expression;
    uint32_t node = 0;
};
using Vector = std::vector<Scalar>;

// One immutable local expression DAG per mathematical definition, never per instance.
struct Formula {
    uint32_t inputs = 0;
    std::vector<MathNode> nodes;
    std::vector<uint32_t> outputs;
    std::vector<double> evaluate(const std::vector<double>&) const;
    std::vector<double> jacobian(const std::vector<double>&) const;
    // Scalar arrays keep spaces and relation arity independent of GLSL vector widths.
    // Jacobian is row-major [output][input]. No resource access can enter this IR.
    std::string glsl(const std::string& function, bool derivatives = false) const;
    void validate() const;
};
class Expression {
    std::shared_ptr<ExpressionData> data_;

  public:
    explicit Expression(uint32_t inputCount);
    Scalar input(uint32_t) const;
    Vector inputs(uint32_t first, uint32_t count) const;
    Scalar constant(float) const;
    Formula finish(const Vector&) const;
    Formula finish(Scalar value) const {
        return finish(Vector{value});
    }
};
Scalar operator+(Scalar, Scalar);
Scalar operator-(Scalar, Scalar);
Scalar operator*(Scalar, Scalar);
Scalar operator/(Scalar, Scalar);
Scalar operator-(Scalar);
Scalar operator+(Scalar, float);
Scalar operator-(Scalar, float);
Scalar operator*(Scalar, float);
Scalar operator/(Scalar, float);
Scalar operator+(float, Scalar);
Scalar operator-(float, Scalar);
Scalar operator*(float, Scalar);
Scalar operator/(float, Scalar);
Scalar sqrt(Scalar);
Scalar sin(Scalar);
Scalar cos(Scalar);
Scalar exp(Scalar);
Scalar log(Scalar);
Scalar abs(Scalar);
Scalar min(Scalar, Scalar);
Scalar max(Scalar, Scalar);
Scalar atan2(Scalar y, Scalar x);
Scalar less(Scalar, Scalar);
Scalar select(Scalar condition, Scalar yes, Scalar no);
Vector operator+(const Vector&, const Vector&);
Vector operator-(const Vector&, const Vector&);
Vector operator*(const Vector&, Scalar);
Vector operator*(const Vector&, float);
Scalar dot(const Vector&, const Vector&);
Scalar length(const Vector&);
Vector cross(const Vector&, const Vector&);
} // namespace whimsical::xpbd
