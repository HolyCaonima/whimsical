#include "physics/dynamics/compiler/FormulaGlsl.h"
#include "Schedule.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <numeric>
#include <sstream>

namespace whimsical::dynamics {
namespace {
void local(std::ostringstream& s, const char* name, uint32_t n) {
    s << "float " << name << "[" << std::max(1u, n) << "];\n";
}
void finite(std::ostringstream& s, const std::string& name, uint32_t n) {
    s << "for(int k=0;k<" << n << ";++k)if(isnan(" << name << "[k])||isinf(" << name
      << "[k])){invalidEvaluation();return;}\n";
}
std::string literal(float value) {
    if (value == 0)
        return "0.0";
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << std::scientific << std::setprecision(9) << value;
    return s.str();
}
std::string endpointAccess(uint32_t slot, int32_t mode) {
    const auto index = std::to_string(slot) + "u";
    if (mode == 0)
        return "x_endpoints[r.e+" + index + "]";
    if (mode == 1)
        return "linearEndpoint(r," + index + ")";
    return "endpoint(r," + index + "," + std::to_string(mode) + ")";
}
void relationInputs(std::ostringstream& s, const RelationType& t, int32_t endpointMode,
                    bool mappedEndpoints = false, bool loadInputs = true) {
    const auto n = t.inputSize();
    if (loadInputs)
        local(s, "x", n);
    if (endpointMode > 0) {
        const auto map = endpointMode - 1;
        s << "uint endpointAt=r.e&0x7fffffffu;\n";
        if (!mappedEndpoints)
            s << "uint endpointRow=r.id-x_endpoints[endpointAt+1u];"
                 "uint endpointLeft=endpointRow,endpointRight=endpointRow;\n";
        if (!mappedEndpoints && map == int(BindingDomain::Map::Product))
            s << "{uint count=x_endpoints[endpointAt+3u];endpointLeft=endpointRow/count;"
                 "endpointRight=endpointRow%count;}\n";
        else if (!mappedEndpoints && map == int(BindingDomain::Map::Directed))
            s << "{uint count=x_endpoints[endpointAt+3u];endpointLeft=endpointRow/(count-1u);"
                 "endpointRight=endpointRow%(count-1u);"
                 "if(endpointRight>=endpointLeft)++endpointRight;}\n";
        else if (!mappedEndpoints && (map == int(BindingDomain::Map::Upper) ||
                 map == int(BindingDomain::Map::UpperDiagonal))) {
            const bool diagonal = map == int(BindingDomain::Map::UpperDiagonal);
            s << "{uint count=x_endpoints[endpointAt+3u];float b=2.0*float(count)"
              << (diagonal ? "+1.0" : "-1.0")
              << ";endpointLeft=min(count-1u,uint(max(0.0,floor((b-sqrt(max(0.0,b*b-8.0*float("
                 "endpointRow))))*0.5))));"
                 "while(endpointLeft>0u&&trianglePrefix(endpointLeft,count,"
              << (diagonal ? "true" : "false")
              << ")>endpointRow)--endpointLeft;"
                 "while(endpointLeft+1u<count&&trianglePrefix(endpointLeft+1u,count,"
              << (diagonal ? "true" : "false")
              << ")<=endpointRow)++endpointLeft;"
                 "endpointRight=endpointLeft+"
              << (diagonal ? "0u" : "1u") << "+endpointRow-trianglePrefix(endpointLeft,count,"
              << (diagonal ? "true" : "false") << ");}\n";
        }
    }
    uint32_t offset = 0;
    for (uint32_t e = 0; e < t.spaces.size(); ++e) {
        s << "uint id" << e << "=";
        if (endpointMode > 0) {
            const auto field = 5 + 2 * e;
            const bool left = e < t.objects[0].size();
            s << "x_endpoints[endpointAt+" << field << "u]+endpoint"
              << (left ? "Left" : "Right") << "*x_endpoints[endpointAt+" << field + 1 << "u]";
        } else
            s << endpointAccess(e, endpointMode);
        s << "; Variable v" << e << "=variable(id" << e << ");\n";
        if (loadInputs)
            for (uint32_t c = 0; c < t.spaces[e]->stateSize; ++c)
                s << "x[" << offset + c << "]=loadEndpointValue(v" << e << "," << c << "u," << e << "u);\n";
        offset += t.spaces[e]->stateSize;
    }
    if (loadInputs)
        for (uint32_t i = 0; i < t.parameters; ++i)
            s << "x[" << offset + i << "]=loadParameter(r," << i << "u);\n";
    offset += t.parameters;
    if (loadInputs) {
        for (uint32_t i = 0; i < t.history; ++i)
            s << "x[" << offset + i << "]=loadHistory(r," << i << "u);\n";
        s << "x[" << n - 2 << "]=h; x[" << n - 1 << "]=time;\n";
    }
}
} // namespace
KernelFunction variableFunction(
    const Space& space, const char* operation, const std::string& name, bool directContributions) {
    const std::string op = operation;
    std::ostringstream s;
    const auto S = space.stateSize, T = space.tangentSize;
    s << emitGlsl(space.retract, name + "_retractValue") << emitGlsl(space.difference, name + "_differenceValue");
    s << "void " << name
      << "(uint id,float h,float time,float relaxation,float inverseH2,uint iteration){Variable v=variable(id);\n";
    if (op == "predict") {
        local(s, "inputValue", S + T);
        local(s, "outputValue", S);
        for (uint32_t i = 0; i < S; ++i)
            s << "inputValue[" << i << "]=loadValue(v," << i << "u);storePrevious(v," << i
              << "u,inputValue[" << i << "]);\n";
        s << "if(!variableEnabled(v))return;\n";
        for (uint32_t i = 0; i < T; ++i)
            s << "inputValue[" << S + i << "]=h*loadVelocity(v," << i
              << "u)+h*h*x_acceleration[v.v+" << i << "u*v.stride];\n";
        s << name << "_retractValue(inputValue,outputValue);\n";
        finite(s, "outputValue", S);
        for (uint32_t i = 0; i < S; ++i)
            s << "storeValue(v," << i << "u,outputValue[" << i << "]);\n";
    } else if (op == "recover") {
        s << "if(!variableEnabled(v))return;\n";
        local(s, "inputValue", S * 2);
        local(s, "outputValue", T);
        for (uint32_t i = 0; i < S; ++i)
            s << "inputValue[" << i << "]=loadValue(v," << i << "u);inputValue[" << S + i
              << "]=loadPrevious(v," << i << "u);\n";
        s << name << "_differenceValue(inputValue,outputValue);\n";
        finite(s, "outputValue", T);
        for (uint32_t i = 0; i < T; ++i)
            s << "storeVelocity(v," << i << "u,outputValue[" << i << "]/h);\n";
    } else {
        local(s, "inputValue", S + T);
        local(s, "outputValue", S);
        if (directContributions)
            for (uint32_t i = 0; i < T; ++i)
                s << "inputValue[" << S + i << "]=uintBitsToFloat(x_contributions[v.v+" << i
                  << "u*v.stride]);x_contributions[v.v+" << i << "u*v.stride]=0u;\n";
        s << "if(v.flags!=0u || !variableEnabled(v))return;\n";
        for (uint32_t i = 0; i < S; ++i)
            s << "inputValue[" << i << "]=loadValue(v," << i << "u);\n";
        if (!directContributions)
            for (uint32_t i = 0; i < T; ++i)
                s << "inputValue[" << S + i << "]=0.0;\n";
        s << "for(uint j=incidenceBegin(id);j<incidenceEnd(id);++j){\n";
        for (uint32_t i = 0; i < T; ++i)
            s << "inputValue[" << S + i << "]+=incidenceContribution(j," << i << "u);\n";
        s << "}\n" << name << "_retractValue(inputValue,outputValue);\n";
        finite(s, "outputValue", S);
        for (uint32_t i = 0; i < S; ++i)
            s << "storeValue(v," << i << "u,outputValue[" << i << "]);\n";
    }
    s << "}\n";
    // Direct gather also clears the shared accumulation slots. Local fused
    // schedules must publish that clear before the next iteration's atomics.
    return {name, s.str(), BufferRole::VariableWork, directContributions};
}
KernelFunction relationFunction(const RelationType& t, const std::vector<bool>& readOnly,
                                int32_t endpointMode, bool jacobi, bool directContributions,
                                bool separateDegrees, bool update, const std::string& name,
                                bool activeDegrees, bool activityChecked, bool distinctWritableEndpoints) {
    std::ostringstream s;
    const auto M = t.rows;
    uint32_t Q = 0, D = 0, activeSlots = 0, singleSlot = 0;
    std::vector<uint32_t> residualInputs;
    uint32_t inputOffset = 0;
    for (uint32_t e = 0; e < t.spaces.size(); ++e) {
        if (!readOnly[e]) {
            for (uint32_t c = 0; c < t.spaces[e]->stateSize; ++c)
                residualInputs.push_back(inputOffset + c);
            Q += t.spaces[e]->stateSize;
            D += t.spaces[e]->tangentSize;
            ++activeSlots;
            singleSlot = e;
        }
        inputOffset += t.spaces[e]->stateSize;
    }
    const bool feasibilityGuard = M == 1 && t.kind != RelationKind::Equality && !activityChecked;
    std::optional<std::vector<float>> residualConstants;
    std::vector<std::optional<std::vector<float>>> tangentConstants;
    std::optional<std::vector<float>> singleJacobian;
    if (update) {
        s << emitGlsl(*t.update, name + "_commitHistory");
    } else {
        residualConstants = constantJacobian(t.residual, residualInputs);
        if (residualConstants)
            s << emitGlsl(t.residual, name + "_residual");
        else {
            s << emitGlslDerivative(t.residual, name + "_residual", residualInputs);
            if (feasibilityGuard)
                s << emitGlsl(t.residual, name + "_feasibility");
        }
        for (uint32_t e = 0; e < t.spaces.size(); ++e) {
            if (readOnly[e]) {
                tangentConstants.emplace_back();
                continue;
            }
            const auto& space = *t.spaces[e];
            std::vector<uint32_t> tangentInputs(space.tangentSize);
            std::iota(tangentInputs.begin(), tangentInputs.end(), space.stateSize);
            auto prefix = name + "_retract" + std::to_string(e);
            tangentConstants.push_back(constantJacobian(space.retract, tangentInputs));
            if (!tangentConstants.back())
                s << emitGlslJacobian(space.retract, prefix + "Tangent", tangentInputs);
            s << emitGlsl(space.retract, prefix + "Value");
        }
        if (activeSlots == 1 && residualConstants && tangentConstants[singleSlot]) {
            const auto S = t.spaces[singleSlot]->stateSize, T = t.spaces[singleSlot]->tangentSize;
            std::vector<float> composed(size_t(M) * T);
            bool finite = true;
            for (uint32_t row = 0; row < M; ++row)
                for (uint32_t col = 0; col < T; ++col)
                    for (uint32_t k = 0; k < S; ++k)
                        finite = std::isfinite(
                                     composed[row * T + col] +=
                                         (*residualConstants)[row * S + k] *
                                         (*tangentConstants[singleSlot])[k * T + col]) &&
                                 finite;
            if (finite)
                singleJacobian = std::move(composed);
        }
    }
    s << "void " << name
      << "(uint id,float h,float time,float relaxation,float inverseH2,uint iteration){Relation r=relation(id);\n";
    // A relation owns its multiplier rows and is scheduled exactly once per
    // iteration. Initialize them in its first solve to avoid a full-buffer pass.
    if (!update && !activityChecked)
        for (uint32_t row = 0; row < M; ++row)
            s << "if(iteration==0u)storeMultiplier(r," << row << "u,0.0);\n";
    if (jacobi && !directContributions)
        for (uint32_t c = 0; c < D; ++c)
            s << "storeContribution(r," << c << "u,0.0);\n";
    s << "if(!relationEnabled(r))return;\n";
    relationInputs(s, t, endpointMode);
    if (update) {
        local(s, "nextHistory", t.history);
        s << name << "_commitHistory(x,nextHistory);\n";
        finite(s, "nextHistory", t.history);
        for (uint32_t c = 0; c < t.history; ++c)
            s << "storeHistory(r," << c << "u,nextHistory[" << c << "]);\n";
        s << "}\n";
        return {name, s.str(), BufferRole::RelationWork};
    }
    local(s, "c", M);
    if (!residualConstants)
        local(s, "rawJ", M * Q);
    if (!singleJacobian)
        local(s, "j", M * D);
    local(s, "wjt", D * M);
    if (feasibilityGuard) {
        s << name << (residualConstants ? "_residual" : "_feasibility") << "(x,c);\n";
        finite(s, "c", M);
        s << "if(c[0]" << (t.kind == RelationKind::GreaterEqual ? ">=" : "<=")
          << "0.0&&loadMultiplier(r,0u)==0.0)return;\n";
    }
    if (!feasibilityGuard || !residualConstants)
        s << name << "_residual(x,c" << (residualConstants ? "" : ",rawJ") << ");\n";
    uint32_t qo = 0, vo = 0;
    inputOffset = 0;
    for (uint32_t e = 0; e < t.spaces.size(); ++e) {
        const auto S = t.spaces[e]->stateSize, T = t.spaces[e]->tangentSize;
        const auto sourceOffset = inputOffset;
        inputOffset += S;
        if (readOnly[e])
            continue;
        s << "float input" << e << "[" << S + T << "], output" << e << "[" << S << "];\n";
        if (!tangentConstants[e])
            s << "float tangent" << e << "[" << S * T << "];\n";
        for (uint32_t a = 0; a < S; ++a)
            s << "input" << e << "[" << a << "]=x[" << sourceOffset + a << "];\n";
        for (uint32_t a = 0; a < T; ++a)
            s << "input" << e << "[" << S + a << "]=0.0;\n";
        if (!tangentConstants[e])
            s << name << "_retract" << e << "Tangent(input" << e << ",tangent" << e << ");\n";
        if (!singleJacobian)
            for (uint32_t row = 0; row < M; ++row)
                for (uint32_t col = 0; col < T; ++col) {
                s << "j[" << row * D + vo + col << "]=";
                if (residualConstants && tangentConstants[e]) {
                    float value = 0;
                    for (uint32_t k = 0; k < S; ++k)
                        value += (*residualConstants)[row * Q + qo + k] *
                                 (*tangentConstants[e])[k * T + col];
                    if (std::isfinite(value))
                        s << literal(value);
                    else {
                        s << "0.0";
                        for (uint32_t k = 0; k < S; ++k)
                            if ((*residualConstants)[row * Q + qo + k] != 0 &&
                                (*tangentConstants[e])[k * T + col] != 0)
                                s << "+(" << literal((*residualConstants)[row * Q + qo + k]) << "*"
                                  << literal((*tangentConstants[e])[k * T + col]) << ")";
                    }
                } else if (tangentConstants[e]) {
                    bool emitted = false;
                    for (uint32_t k = 0; k < S; ++k) {
                        const auto coefficient = (*tangentConstants[e])[k * T + col];
                        if (coefficient == 0)
                            continue;
                        if (emitted && coefficient > 0)
                            s << "+";
                        if (coefficient == 1)
                            s << "rawJ[" << row * Q + qo + k << "]";
                        else if (coefficient == -1)
                            s << "-rawJ[" << row * Q + qo + k << "]";
                        else
                            s << literal(coefficient) << "*rawJ[" << row * Q + qo + k << "]";
                        emitted = true;
                    }
                    if (!emitted)
                        s << "0.0";
                } else if (residualConstants) {
                    bool emitted = false;
                    for (uint32_t k = 0; k < S; ++k) {
                        const auto coefficient = (*residualConstants)[row * Q + qo + k];
                        if (coefficient == 0)
                            continue;
                        if (emitted && coefficient > 0)
                            s << "+";
                        if (coefficient == 1)
                            s << "tangent" << e << "[" << k * T + col << "]";
                        else if (coefficient == -1)
                            s << "-tangent" << e << "[" << k * T + col << "]";
                        else
                            s << literal(coefficient) << "*tangent" << e << "[" << k * T + col
                              << "]";
                        emitted = true;
                    }
                    if (!emitted)
                        s << "0.0";
                } else {
                    s << "0.0";
                    for (uint32_t k = 0; k < S; ++k)
                        s << "+rawJ[" << row * Q + qo + k << "]*tangent" << e << "["
                          << k * T + col << "]";
                }
                s << ";\n";
            }
        qo += S;
        vo += T;
    }
    // Repeated endpoint identities describe one variable. Merge their Jacobians
    // before forming the effective mass; otherwise cross terms would be lost.
    vo = 0;
    for (uint32_t e = 0; !distinctWritableEndpoints && e < t.spaces.size(); ++e) {
        if (readOnly[e])
            continue;
        uint32_t before = 0;
        bool emitted = false;
        for (uint32_t b = 0; b < e; ++b) {
            if (readOnly[b])
                continue;
            if (t.spaces[e]->stateSize == t.spaces[b]->stateSize &&
                t.spaces[e]->tangentSize == t.spaces[b]->tangentSize) {
                s << (emitted ? "else " : "") << "if(id" << e << "==id" << b << ") {\n";
                emitted = true;
                for (uint32_t row = 0; row < M; ++row)
                    for (uint32_t a = 0; a < t.spaces[e]->tangentSize; ++a)
                        s << "j[" << row * D + before + a << "]+=j[" << row * D + vo + a << "];j["
                          << row * D + vo + a << "]=0.0;\n";
                s << "}\n";
            }
            before += t.spaces[b]->tangentSize;
        }
        vo += t.spaces[e]->tangentSize;
    }
    vo = 0;
    for (uint32_t e = 0; e < t.spaces.size(); ++e) {
        if (readOnly[e])
            continue;
        auto T = t.spaces[e]->tangentSize;
        for (uint32_t col = 0; col < T; ++col)
            for (uint32_t row = 0; row < M; ++row) {
                s << "wjt[" << (vo + col) * M + row << "]=0.0;\nif(v" << e
                  << ".flags==0u && variableEnabled(v" << e << ")){if(identityMetric(v" << e << "))wjt["
                  << (vo + col) * M + row << "]=";
                if (singleJacobian)
                    s << literal((*singleJacobian)[row * T + col]);
                else
                    s << "j[" << row * D + vo + col << "]";
                s << ";else wjt[" << (vo + col) * M + row << "]=";
                if (singleJacobian) {
                    bool emitted = false;
                    for (uint32_t k = 0; k < T; ++k) {
                        const auto coefficient = (*singleJacobian)[row * T + k];
                        if (coefficient == 0)
                            continue;
                        if (emitted && coefficient > 0)
                            s << "+";
                        if (coefficient == -1)
                            s << "-";
                        else if (coefficient != 1)
                            s << literal(coefficient) << "*";
                        s << "x_metric[v" << e << ".m+" << col * T + k << "u*v" << e
                          << ".stride]";
                        emitted = true;
                    }
                    if (!emitted)
                        s << "0.0";
                } else
                    for (uint32_t k = 0; k < T; ++k)
                        s << (k ? "+" : "") << "x_metric[v" << e << ".m+" << col * T + k << "u*v"
                          << e << ".stride]*j[" << row * D + vo + k << "]";
                s << ";}\n";
            }
        vo += T;
    }
    local(s, "a", M * M);
    local(s, "rhs", M);
    local(s, "dl", M);
    for (uint32_t row = 0; row < M; ++row) {
        s << "float alpha" << row << "=loadCompliance(r," << row << "u,"
          << 2 + t.parameters << "u)*inverseH2;\n";
        s << "rhs[" << row << "]=-c[" << row << "]-alpha" << row << "*loadMultiplier(r," << row
          << "u);\n";
        for (uint32_t col = 0; col < M; ++col) {
            s << "a[" << row * M + col << "]=" << (row == col ? "alpha" + std::to_string(row) : "0.0");
            for (uint32_t k = 0; k < D; ++k) {
                if (singleJacobian) {
                    const auto coefficient = (*singleJacobian)[row * D + k];
                    if (coefficient == 0)
                        continue;
                    if (coefficient == 1)
                        s << "+";
                    else if (coefficient == -1)
                        s << "-";
                    else
                        s << (coefficient > 0 ? "+" : "") << literal(coefficient) << "*";
                    s << "wjt[" << k * M + col << "]";
                } else
                    s << "+j[" << row * D + k << "]*wjt[" << k * M + col << "]";
            }
            s << ";\n";
        }
    }
    // A singular local model does not have a defined XPBD correction. Leave it
    // unchanged; diagnostics expose it instead of inventing an epsilon compliance.
    if (M == 1) {
        // The scalar block has no pivot choice or elimination. Emitting the quotient
        // directly avoids materializing the generic matrix normalization path.
        s << "if(isnan(a[0])||isinf(a[0])||isnan(rhs[0])||isinf(rhs[0]))"
             "{invalidEvaluation();return;}\n";
        s << "if(a[0]==0.0){if(rhs[0]!=0.0)singularSystem();return;}\n";
        s << "rhs[0]/=a[0];\n";
    } else {
        s << "float scale=0.0;for(int k=0;k<" << M * M
          << ";++k){if(isnan(a[k])||isinf(a[k])){invalidEvaluation();return;}scale=max(scale,abs(a[k])"
             ");}\n";
        s << "for(int k=0;k<" << M
          << ";++k)if(isnan(rhs[k])||isinf(rhs[k])){invalidEvaluation();return;}\n";
        s << "if(scale==0.0){for(int k=0;k<" << M
          << ";++k)if(rhs[k]!=0.0){singularSystem();break;}return;}\n";
    }
    // Small block dimensions are compile-time facts. Static matrix accesses let
    // the GPU keep these blocks in registers instead of dynamically indexed arrays.
    // Preserve partial pivoting and the same elimination order as the general path.
    if (M > 1 && M <= 4) {
        for (uint32_t col = 0; col < M; ++col) {
            s << "{uint pivot=" << col << "u;float largest=abs(a[" << col * M + col << "]);\n";
            for (uint32_t row = col + 1; row < M; ++row)
                s << "if(abs(a[" << row * M + col << "])>largest){largest=abs(a[" << row * M + col
                  << "]);pivot=" << row << "u;}\n";
            s << "if(largest<=scale*1e-7){singularSystem();return;}\n";
            for (uint32_t row = col + 1; row < M; ++row) {
                s << "if(pivot==" << row << "u){\n";
                for (uint32_t k = 0; k < M; ++k)
                    s << "{float v=a[" << col * M + k << "];a[" << col * M + k << "]=a[" << row * M + k
                      << "];a[" << row * M + k << "]=v;}\n";
                s << "float b=rhs[" << col << "];rhs[" << col << "]=rhs[" << row << "];rhs[" << row
                  << "]=b;}\n";
            }
            s << "float inverse=1.0/a[" << col * M + col << "];\n";
            for (uint32_t k = 0; k < M; ++k)
                s << "a[" << col * M + k << "]*=inverse;\n";
            s << "rhs[" << col << "]*=inverse;\n";
            for (uint32_t row = 0; row < M; ++row) {
                if (row == col)
                    continue;
                s << "{float f=a[" << row * M + col << "];\n";
                for (uint32_t k = 0; k < M; ++k)
                    s << "a[" << row * M + k << "]-=f*a[" << col * M + k << "];\n";
                s << "rhs[" << row << "]-=f*rhs[" << col << "];}\n";
            }
            s << "}\n";
        }
    } else if (M > 4) {
        s << "for(int col=0;col<" << M << ";++col){int pivot=col;for(int row=col+1;row<" << M
          << ";++row)if(abs(a[row*" << M << "+col])>abs(a[pivot*" << M << "+col]))pivot=row;\n";
        s << "if(abs(a[pivot*" << M << "+col])<=scale*1e-7){singularSystem();return;}\n";
        s << "for(int k=0;k<" << M << ";++k){float v=a[col*" << M << "+k];a[col*" << M << "+k]=a[pivot*" << M
          << "+k];a[pivot*" << M << "+k]=v;}\n";
        s << "float b=rhs[col];rhs[col]=rhs[pivot];rhs[pivot]=b;float inverse=1.0/a[col*" << M
          << "+col];for(int k=0;k<" << M << ";++k)a[col*" << M << "+k]*=inverse;rhs[col]*=inverse;\n";
        s << "for(int row=0;row<" << M << ";++row)if(row!=col){float f=a[row*" << M << "+col];for(int k=0;k<" << M
          << ";++k)a[row*" << M << "+k]-=f*a[col*" << M << "+k];rhs[row]-=f*rhs[col];}}\n";
    }
    if (jacobi) {
        if (activeDegrees) {
            s << "uint degree=1u;\n";
            for (uint32_t e = 0; e < t.spaces.size(); ++e)
                if (!readOnly[e])
                    s << "degree=max(degree,x_activeDegrees[id" << e << "]);\n";
        } else if (directContributions)
            s << "uint degree=r.cs;\n";
        else {
            s << "uint degree=1u;\n";
            for (uint32_t e = 0; e < t.spaces.size(); ++e)
                if (!readOnly[e])
                    if (separateDegrees)
                        s << "degree=max(degree,x_adjCursors[id" << e << "]);\n";
                    else
                        s << "degree=max(degree,x_adjOffsets[id" << e << "+1u]-x_adjOffsets[id" << e
                          << "]);\n";
        }
    }
    local(s, "nextLambda", M);
    for (uint32_t row = 0; row < M; ++row) {
        s << "float candidate" << row << "=loadMultiplier(r," << row << "u)+rhs[" << row
          << "]*relaxation" << (jacobi ? "/float(degree)" : "") << ";\n";
        std::string next = "candidate" + std::to_string(row);
        // XPBD uses positive multipliers for C >= 0 and negative for C <= 0.
        if (t.kind == RelationKind::GreaterEqual)
            next = "max(0.0," + next + ")";
        if (t.kind == RelationKind::LessEqual)
            next = "min(0.0," + next + ")";
        s << "nextLambda[" << row << "]=" << next << ";dl[" << row << "]=nextLambda[" << row
          << "]-loadMultiplier(r," << row << "u);\n";
    }
    finite(s, "nextLambda", M);
    finite(s, "dl", M);
    vo = 0;
    for (uint32_t e = 0; e < t.spaces.size(); ++e) {
        if (readOnly[e])
            continue;
        const auto S = t.spaces[e]->stateSize, T = t.spaces[e]->tangentSize;
        for (uint32_t col = 0; col < T; ++col) {
            s << "input" << e << "[" << S + col << "]=0.0";
            for (uint32_t row = 0; row < M; ++row)
                s << "+wjt[" << (vo + col) * M + row << "]*dl[" << row << "]";
            s << ";\n";
        }
        if (jacobi) {
            finite(s, "input" + std::to_string(e), S + T);
        } else {
            s << "if(v" << e << ".flags==0u && variableEnabled(v" << e << ")";
            for (uint32_t b = 0; !distinctWritableEndpoints && b < e; ++b)
                s << " && id" << e << "!=id" << b;
            s << ") {\n";
            s << name << "_retract" << e << "Value(input" << e << ",output" << e << ");\n";
            finite(s, "output" + std::to_string(e), S);
            s << "}\n";
        }
        vo += T;
    }
    for (uint32_t row = 0; row < M; ++row)
        s << "storeMultiplier(r," << row << "u,nextLambda[" << row << "]);\n";
    vo = 0;
    for (uint32_t e = 0; e < t.spaces.size(); ++e) {
        if (readOnly[e])
            continue;
        const auto S = t.spaces[e]->stateSize, T = t.spaces[e]->tangentSize;
        if (jacobi) {
            if (directContributions) {
                s << "if(v" << e << ".flags==0u && variableEnabled(v" << e << ")";
                for (uint32_t b = 0; !distinctWritableEndpoints && b < e; ++b)
                    s << " && id" << e << "!=id" << b;
                s << ") {\n";
                for (uint32_t col = 0; col < T; ++col)
                    s << "addContribution(v" << e << ".v+" << col << "u*v" << e << ".stride,input"
                      << e << "[" << S + col << "]);\n";
                s << "}\n";
            } else
                for (uint32_t col = 0; col < T; ++col)
                    s << "storeContribution(r," << vo + col << "u,input" << e
                      << "[" << S + col << "]);\n";
        } else {
            s << "if(v" << e << ".flags==0u && variableEnabled(v" << e << ")";
            for (uint32_t b = 0; !distinctWritableEndpoints && b < e; ++b)
                s << " && id" << e << "!=id" << b;
            s << ") {\n";
            for (uint32_t col = 0; col < S; ++col)
                s << "storeEndpointValue(v" << e << "," << col << "u," << e << "u,output" << e << "[" << col << "]);\n";
            s << "}\n";
        }
        vo += T;
    }
    s << "}\n";
    return {name, s.str(), BufferRole::RelationWork, jacobi};
}
KernelFunction relationActivityFunction(const RelationType& t, const std::vector<bool>& readOnly,
                                       int32_t endpointMode, const std::string& name, bool countDegrees,
                                       bool mappedEndpoints, bool residualChecked) {
    std::ostringstream s;
    const bool guarded = t.rows == 1 && t.kind != RelationKind::Equality;
    if (guarded && !residualChecked)
        s << emitGlsl(t.residual, name + "_value");
    s << "bool " << name
      << "(uint id,float h,float time,float relaxation,float inverseH2,uint iteration";
    if (mappedEndpoints)
        s << ",uint endpointLeft,uint endpointRight";
    if (residualChecked)
        s << ",float checkedResidual";
    s << "){Relation r=relation(id);\n";
    for (uint32_t row = 0; row < t.rows; ++row)
        s << "if(iteration==0u)storeMultiplier(r," << row << "u,0.0);\n";
    s << "if(!relationEnabled(r))return false;\n";
    relationInputs(s, t, endpointMode, mappedEndpoints, !residualChecked);
    if (guarded) {
        s << "float c[1];";
        if (residualChecked)
            s << "c[0]=checkedResidual;\n";
        else
            s << name << "_value(x,c);\n";
        s
          << "if(!isnan(c[0])&&!isinf(c[0])&&c[0]" << (t.kind == RelationKind::GreaterEqual ? ">=" : "<=")
          << "0.0&&loadMultiplier(r,0u)==0.0)return false;\n";
    }
    for (uint32_t e = 0; countDegrees && e < t.spaces.size(); ++e) {
        if (readOnly[e])
            continue;
        s << "if(v" << e << ".flags==0u&&variableEnabled(v" << e << ")";
        for (uint32_t before = 0; before < e; ++before)
            s << "&&id" << e << "!=id" << before;
        s << ")atomicAdd(x_activeDegrees[id" << e << "],1u);\n";
    }
    s << "return true;}\n";
    return {name, s.str(), BufferRole::RelationWork};
}
KernelFunction relationDispatchFunction(
    const std::vector<std::pair<uint32_t, KernelFunction>>& functions,
    bool jacobi,
    const std::string& name) {
    std::ostringstream s;
    for (const auto& item : functions)
        s << item.second.source;
    s << "void " << name
      << "(uint id,float h,float time,float relaxation,float inverseH2,uint iteration){switch(relation(id).type){\n";
    for (const auto& [type, function] : functions)
        s << "case " << type << "u:" << function.entry
          << "(id,h,time,relaxation,inverseH2,iteration);break;\n";
    s << "}}\n";
    return {name, s.str(), BufferRole::RelationWork, jacobi};
}
std::string incidenceKernel(const RelationType& t, const std::vector<bool>& readOnly, int32_t endpointMode, bool scatter) {
    std::ostringstream s;
    s << fieldAccess(false) << "void main(){uint i=invocation();if(i>=step.count)return;uint "
         "id=x_relationWork[step.first+i];Relation r=relation(id);if(!relationEnabled(r))return;\n";
    uint32_t offset = 0;
    for (uint32_t e = 0; e < t.spaces.size(); ++e) {
        s << "uint id" << e << "=" << endpointAccess(e, endpointMode) << ";\n";
        if (readOnly[e])
            continue;
        s << "if(variable(id" << e << ").flags==0u";
        for (uint32_t b = 0; b < e; ++b)
            s << " && id" << e << "!=id" << b;
        s << ") {\n";
        if (scatter)
            s << "uint at=x_adjOffsets[id" << e << "]+atomicAdd(x_adjCursors[id" << e
              << "],1u);x_adjEntries[at*2u]=r.c+" << offset << "u*r.cs;x_adjEntries[at*2u+1u]=r.cs;\n";
        else
            s << "atomicAdd(x_scanScratch[id" << e << "],1u);\n";
        s << "}\n";
        offset += t.spaces[e]->tangentSize;
    }
    s << "}\n";
    return s.str();
}
std::string globalKernel(const KernelFunction& function) {
    const char* work = function.work == BufferRole::VariableWork ? "variableWork" : "relationWork";
    return stateAccess(StateStorage::Global) + function.source + "void main(){uint lane=invocation();if(lane>=step.count)return;" +
           function.entry + "(x_" + work +
           "[step.first+lane],step.h,step.time,step.relaxation,1.0/(step.h*step.h),step.iteration); }\n";
}
std::string fieldAccess(bool localFields) {
    if (localFields)
        return R"(
bool variableEnabled(Variable v){return x_variableEnabled[v.id]!=0.0;}
bool identityMetric(Variable v){return false;}
bool relationEnabled(Relation r){return x_relationEnabled[r.id]!=0.0;}
float loadParameter(Relation r,uint c){return x_parameters[r.p+c*r.stride];}
float loadCompliance(Relation r,uint c,uint uniformOffset){return x_compliance[r.a+c*r.stride];}
)";
    return R"(
#if DYNAMICS_UNIFORM_VARIABLE_ENABLED
bool variableEnabled(Variable v){
    uint modes=x_fieldModes[v.u];
    return (modes&1u)!=0u?(modes&8u)!=0u:x_variableEnabled[v.id]!=0.0;
}
#else
bool variableEnabled(Variable v){return x_variableEnabled[v.id]!=0.0;}
#endif
#if DYNAMICS_IDENTITY_METRIC
bool identityMetric(Variable v){return (x_fieldModes[v.u]&2u)!=0u;}
#else
bool identityMetric(Variable v){return false;}
#endif
#if DYNAMICS_UNIFORM_RELATION_ENABLED
bool relationEnabled(Relation r){
    uint modes=x_fieldModes[r.u];
    return (modes&1u)!=0u?(modes&8u)!=0u:x_relationEnabled[r.id]!=0.0;
}
#else
bool relationEnabled(Relation r){return x_relationEnabled[r.id]!=0.0;}
#endif
#if DYNAMICS_UNIFORM_PARAMETERS
float loadParameter(Relation r,uint c){
    return (x_fieldModes[r.u]&2u)!=0u?uintBitsToFloat(x_fieldModes[r.u+2u+c]):
        x_parameters[r.p+c*r.stride];
}
#else
float loadParameter(Relation r,uint c){return x_parameters[r.p+c*r.stride];}
#endif
#if DYNAMICS_UNIFORM_COMPLIANCE
float loadCompliance(Relation r,uint c,uint uniformOffset){
    uint modes=x_fieldModes[r.u];
    return (modes&4u)!=0u?((modes&16u)!=0u?0.0:
        uintBitsToFloat(x_fieldModes[r.u+uniformOffset+c])):x_compliance[r.a+c*r.stride];
}
#else
float loadCompliance(Relation r,uint c,uint uniformOffset){return x_compliance[r.a+c*r.stride];}
#endif
)";
}
std::string stateAccess(StateStorage storage, uint32_t variableCount, bool externalInputs) {
    const bool localState = storage == StateStorage::Region, epoch = storage == StateStorage::Epoch;
    struct FieldAccess {
        const char* name;
        const char* buffer;
        const char* offset;
        bool variable;
    };
    const FieldAccess fields[] = {{"Value", "q", "q", true}, {"Previous", "oldq", "q", true},
                                  {"Velocity", "velocity", "v", true}, {"History", "history", "h", false},
                                  {"Multiplier", "lambda", "l", false}};
    std::ostringstream s;
    s << fieldAccess(localState);
    s << "void invalidEvaluation(){" << (epoch ? "if(epochOwned)" : "")
      << "atomicAdd(x_diagnostics[0],1u);}\n"
         "void singularSystem(){" << (epoch ? "if(epochOwned)" : "")
      << "atomicAdd(x_diagnostics[1],1u);}\n";
    if (epoch)
        s << R"(
uint incidenceBegin(uint id){return 0u;}
uint incidenceEnd(uint id){return epochIncidenceCount;}
float incidenceContribution(uint j,uint c){
    uint at=epochIncidenceFirst+j*2u;
    return epochState[x_epochData[at]+c*x_epochData[at+1u]];
}
void storeContribution(Relation r,uint c,float value){epochState[epochContribution+c*epochContributionStride]=value;}
)";
    else
        s << R"(
uint incidenceBegin(uint id){return x_adjOffsets[id];}
uint incidenceEnd(uint id){return x_adjOffsets[id+1u];}
float incidenceContribution(uint j,uint c){
    return uintBitsToFloat(x_contributions[x_adjEntries[j*2u]+c*x_adjEntries[j*2u+1u]]);
}
void storeContribution(Relation r,uint c,float value){x_contributions[r.c+c*r.cs]=floatBitsToUint(value);}
)";
    s << "void addContribution(uint at,float value){if(value==0.0)return;uint expected=x_contributions[at];"
         "for(;;){uint desired=floatBitsToUint(uintBitsToFloat(expected)+value);"
         "uint observed=atomicCompSwap(x_contributions[at],expected,desired);"
         "if(observed==expected)return;expected=observed;}}\n";
    for (uint32_t i = 0; i < 5; ++i) {
        const auto& field = fields[i];
        const char* type = field.variable ? "Variable" : "Relation";
        std::string address = std::string("x_") + field.buffer + "[v." + field.offset + "+c*v.stride]";
        if (epoch && i == 0) {
            s << "float loadValue(Variable v,uint c){return epochState[epochVariableOffset+c];}\n"
                 "void storeValue(Variable v,uint c,float value){epochState[epochVariableOffset+c]=value;}\n";
            continue;
        }
        if (epoch && i == 4) {
            s << "float loadMultiplier(Relation v,uint c){return step.iteration==0u?0.0:"
              << address << ";}\n"
                 "void storeMultiplier(Relation v,uint c,float value){if(epochOwned)"
                 "x_epochOutput[EPOCH_VALUE_WORDS+v.l+c*v.stride]=value;}\n";
            continue;
        }
        const auto globalAddress = address;
        if (localState)
            address = "regionState[x_localOffsets[" +
                (field.variable ? "v.id*3u+" + std::to_string(i) :
                 std::to_string(uint64_t(variableCount) * 3) + "u+v.id*2u+" + std::to_string(i - 3)) + "u]+c]";
        std::string external;
        if (localState && externalInputs && field.variable)
            external = "x_localOffsets[v.id*3u]==0xffffffffu";
        s << "float load" << field.name << "(" << type << " v,uint c){return ";
        if (!external.empty())
            s << external << "?" << globalAddress << ":";
        s << address << ";}\n";
        s << "void store" << field.name << "(" << type << " v,uint c,float value){";
        if (!external.empty())
            s << "if(" << external << ")" << globalAddress << "=value;else ";
        s << address << "=value;}\n";
    }
    if (epoch)
        s << R"(
float loadEndpointValue(Variable v,uint c,uint slot){
    return v.flags!=0u?x_q[v.q+c*v.stride]:epochState[x_epochData[epochEndpointFirst+slot]+c];
}
void storeEndpointValue(Variable v,uint c,uint slot,float value){
    epochState[x_epochData[epochEndpointFirst+slot]+c]=value;
}
)";
    else
        s << R"(
float loadEndpointValue(Variable v,uint c,uint slot){return loadValue(v,c);}
void storeEndpointValue(Variable v,uint c,uint slot,float value){storeValue(v,c,value);}
)";
    return s.str();
}
} // namespace whimsical::dynamics
