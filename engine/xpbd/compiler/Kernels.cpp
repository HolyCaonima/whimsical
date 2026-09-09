#include "CompiledPlan.h"
#include <sstream>
#include <algorithm>

namespace whimsical::xpbd {
namespace {
void local(std::ostringstream& s, const char* name, uint32_t n) {
    s << "float " << name << "[" << std::max(1u, n) << "];\n";
}
void finite(std::ostringstream& s, const std::string& name, uint32_t n) {
    s << "for(int k=0;k<" << n << ";++k)if(isnan(" << name << "[k])||isinf(" << name
      << "[k])){atomicAdd(x_diagnostics[0],1u);return;}\n";
}
void relationInputs(std::ostringstream& s, const RelationType& t) {
    const auto n = t.inputSize();
    local(s, "x", n);
    uint32_t offset = 0;
    for (uint32_t e = 0; e < t.spaces.size(); ++e) {
        s << "uint id" << e << "=x_endpoints[r.e+" << e << "u]; Variable v" << e << "=variable(id" << e
          << ");\n";
        for (uint32_t c = 0; c < t.spaces[e]->stateSize; ++c)
            s << "x[" << offset + c << "]=x_q[v" << e << ".q+" << c << "u*v" << e << ".stride];\n";
        offset += t.spaces[e]->stateSize;
    }
    for (uint32_t i = 0; i < t.parameters; ++i)
        s << "x[" << offset + i << "]=x_parameters[r.p+" << i << "u*r.stride];\n";
    offset += t.parameters;
    for (uint32_t i = 0; i < t.history; ++i)
        s << "x[" << offset + i << "]=x_history[r.h+" << i << "u*r.stride];\n";
    offset += t.history;
    for (uint32_t i = 0; i < t.rows; ++i)
        s << "x[" << offset + i << "]=x_lambda[r.l+" << i << "u*r.stride];\n";
    s << "x[" << n - 2 << "]=step.h; x[" << n - 1 << "]=step.time;\n";
}
} // namespace
std::string variableKernel(const Space& space, const char* operation) {
    const std::string op = operation;
    std::ostringstream s;
    const auto S = space.stateSize, T = space.tangentSize;
    s << space.retract.glsl("retractValue") << space.difference.glsl("differenceValue");
    s << "void main(){uint lane=invocation();if(lane>=step.count)return;uint "
         "id=x_variableWork[step.first+lane];Variable v=variable(id);\n";
    if (op == "predict") {
        local(s, "inputValue", S + T);
        local(s, "outputValue", S);
        for (uint32_t i = 0; i < S; ++i)
            s << "inputValue[" << i << "]=x_q[v.q+" << i << "u*v.stride];x_oldq[v.q+" << i
              << "u*v.stride]=inputValue[" << i << "];\n";
        s << "if(x_variableEnabled[id]==0.0)return;\n";
        for (uint32_t i = 0; i < T; ++i)
            s << "inputValue[" << S + i << "]=step.h*x_velocity[v.v+" << i
              << "u*v.stride]+step.h*step.h*x_acceleration[v.v+" << i << "u*v.stride];\n";
        s << "retractValue(inputValue,outputValue);\n";
        finite(s, "outputValue", S);
        for (uint32_t i = 0; i < S; ++i)
            s << "x_q[v.q+" << i << "u*v.stride]=outputValue[" << i << "];\n";
    } else if (op == "recover") {
        s << "if(x_variableEnabled[id]==0.0)return;\n";
        local(s, "inputValue", S * 2);
        local(s, "outputValue", T);
        for (uint32_t i = 0; i < S; ++i)
            s << "inputValue[" << i << "]=x_q[v.q+" << i << "u*v.stride];inputValue[" << S + i
              << "]=x_oldq[v.q+" << i << "u*v.stride];\n";
        s << "differenceValue(inputValue,outputValue);\n";
        finite(s, "outputValue", T);
        for (uint32_t i = 0; i < T; ++i)
            s << "x_velocity[v.v+" << i << "u*v.stride]=outputValue[" << i << "]/step.h;\n";
    } else {
        s << "if(v.flags!=0u || x_variableEnabled[id]==0.0)return;\n";
        local(s, "inputValue", S + T);
        local(s, "outputValue", S);
        for (uint32_t i = 0; i < S; ++i)
            s << "inputValue[" << i << "]=x_q[v.q+" << i << "u*v.stride];\n";
        for (uint32_t i = 0; i < T; ++i)
            s << "inputValue[" << S + i << "]=0.0;\n";
        s << "for(uint j=x_adjOffsets[id];j<x_adjOffsets[id+1u];++j){uint "
             "at=x_adjEntries[j*2u],stride=x_adjEntries[j*2u+1u];\n";
        for (uint32_t i = 0; i < T; ++i)
            s << "inputValue[" << S + i << "]+=x_contributions[at+" << i << "u*stride];\n";
        s << "}\nretractValue(inputValue,outputValue);\n";
        finite(s, "outputValue", S);
        for (uint32_t i = 0; i < S; ++i)
            s << "x_q[v.q+" << i << "u*v.stride]=outputValue[" << i << "];\n";
    }
    s << "}\n";
    return s.str();
}
std::string relationKernel(const RelationType& t, bool jacobi, bool update) {
    std::ostringstream s;
    const auto N = t.inputSize(), D = t.tangentSize(), M = t.rows;
    s << t.residual.glsl("residual", true);
    if (t.projection)
        s << t.projection->glsl("projectMultiplier");
    if (t.update)
        s << t.update->glsl("commitHistory");
    for (uint32_t e = 0; e < t.spaces.size(); ++e)
        s << t.spaces[e]->retract.glsl("retract" + std::to_string(e), true);
    s << "void main(){uint lane=invocation();if(lane>=step.count)return;uint "
         "id=x_relationWork[step.first+lane];Relation r=relation(id);\n";
    if (jacobi)
        for (uint32_t c = 0; c < D; ++c)
            s << "x_contributions[r.c+" << c << "u*r.cs]=0.0;\n";
    s << "if(x_relationEnabled[id]==0.0)return;\n";
    relationInputs(s, t);
    if (update) {
        local(s, "nextHistory", t.history);
        s << "commitHistory(x,nextHistory);\n";
        finite(s, "nextHistory", t.history);
        for (uint32_t c = 0; c < t.history; ++c)
            s << "x_history[r.h+" << c << "u*r.stride]=nextHistory[" << c << "];\n";
        s << "}\n";
        return s.str();
    }
    local(s, "c", M);
    local(s, "rawJ", M * N);
    local(s, "j", M * D);
    local(s, "wjt", D * M);
    s << "residual(x,c,rawJ);\n";
    uint32_t qo = 0, vo = 0;
    for (uint32_t e = 0; e < t.spaces.size(); ++e) {
        const auto S = t.spaces[e]->stateSize, T = t.spaces[e]->tangentSize;
        s << "float input" << e << "[" << S + T << "], output" << e << "[" << S << "], tangent" << e << "["
          << S * (S + T) << "];\n";
        for (uint32_t a = 0; a < S; ++a)
            s << "input" << e << "[" << a << "]=x[" << qo + a << "];\n";
        for (uint32_t a = 0; a < T; ++a)
            s << "input" << e << "[" << S + a << "]=0.0;\n";
        s << "retract" << e << "(input" << e << ",output" << e << ",tangent" << e << ");\n";
        for (uint32_t row = 0; row < M; ++row)
            for (uint32_t col = 0; col < T; ++col) {
                s << "j[" << row * D + vo + col << "]=0.0";
                for (uint32_t k = 0; k < S; ++k)
                    s << "+rawJ[" << row * N + qo + k << "]*tangent" << e << "[" << k * (S + T) + S + col
                      << "]";
                s << ";\n";
            }
        qo += S;
        vo += T;
    }
    // Repeated endpoint identities describe one variable. Merge their Jacobians
    // before forming the effective mass; otherwise cross terms would be lost.
    vo = 0;
    for (uint32_t e = 0; e < t.spaces.size(); ++e) {
        uint32_t before = 0;
        bool emitted = false;
        for (uint32_t b = 0; b < e; ++b) {
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
        auto T = t.spaces[e]->tangentSize;
        for (uint32_t col = 0; col < T; ++col)
            for (uint32_t row = 0; row < M; ++row) {
                s << "wjt[" << (vo + col) * M + row << "]=0.0;\nif(v" << e
                  << ".flags==0u && x_variableEnabled[id" << e << "]!=0.0)wjt[" << (vo + col) * M + row
                  << "]=";
                for (uint32_t k = 0; k < T; ++k)
                    s << (k ? "+" : "") << "x_metric[v" << e << ".m+" << col * T + k << "u*v" << e
                      << ".stride]*j[" << row * D + vo + k << "]";
                s << ";\n";
            }
        vo += T;
    }
    local(s, "a", M * M);
    local(s, "rhs", M);
    local(s, "dl", M);
    for (uint32_t row = 0; row < M; ++row) {
        s << "float alpha" << row << "=x_compliance[r.a+" << row << "u*r.stride]/(step.h*step.h);\n";
        s << "rhs[" << row << "]=-c[" << row << "]-alpha" << row << "*x_lambda[r.l+" << row
          << "u*r.stride];\n";
        for (uint32_t col = 0; col < M; ++col) {
            s << "a[" << row * M + col << "]=" << (row == col ? "alpha" + std::to_string(row) : "0.0");
            for (uint32_t k = 0; k < D; ++k)
                s << "+j[" << row * D + k << "]*wjt[" << k * M + col << "]";
            s << ";\n";
        }
    }
    // A singular local model does not have a defined XPBD correction. Leave it
    // unchanged; diagnostics expose it instead of inventing an epsilon compliance.
    s << "float scale=0.0;for(int k=0;k<" << M * M
      << ";++k){if(isnan(a[k])||isinf(a[k])){atomicAdd(x_diagnostics[0],1u);return;}scale=max(scale,abs(a[k])"
         ");}\n";
    s << "for(int k=0;k<" << M
      << ";++k)if(isnan(rhs[k])||isinf(rhs[k])){atomicAdd(x_diagnostics[0],1u);return;}\n";
    s << "if(scale==0.0){for(int k=0;k<" << M
      << ";++k)if(rhs[k]!=0.0){atomicAdd(x_diagnostics[1],1u);break;}return;}\n";
    s << "for(int col=0;col<" << M << ";++col){int pivot=col;for(int row=col+1;row<" << M
      << ";++row)if(abs(a[row*" << M << "+col])>abs(a[pivot*" << M << "+col]))pivot=row;\n";
    s << "if(abs(a[pivot*" << M << "+col])<=scale*1e-7){atomicAdd(x_diagnostics[1],1u);return;}\n";
    s << "for(int k=0;k<" << M << ";++k){float v=a[col*" << M << "+k];a[col*" << M << "+k]=a[pivot*" << M
      << "+k];a[pivot*" << M << "+k]=v;}\n";
    s << "float b=rhs[col];rhs[col]=rhs[pivot];rhs[pivot]=b;float inverse=1.0/a[col*" << M
      << "+col];for(int k=0;k<" << M << ";++k)a[col*" << M << "+k]*=inverse;rhs[col]*=inverse;\n";
    s << "for(int row=0;row<" << M << ";++row)if(row!=col){float f=a[row*" << M << "+col];for(int k=0;k<" << M
      << ";++k)a[row*" << M << "+k]-=f*a[col*" << M << "+k];rhs[row]-=f*rhs[col];}}\n";
    const auto lambdaInput = N - M - 2;
    if (jacobi) {
        s << "uint degree=1u;\n";
        for (uint32_t e = 0; e < t.spaces.size(); ++e)
            s << "degree=max(degree,x_adjOffsets[id" << e << "+1u]-x_adjOffsets[id" << e << "]);\n";
    }
    for (uint32_t row = 0; row < M; ++row)
        s << "x[" << lambdaInput + row << "]=x_lambda[r.l+" << row << "u*r.stride]+rhs[" << row
          << "]*step.relaxation" << (jacobi ? "/float(degree)" : "") << ";\n";
    if (t.domain == Domain::Projected) {
        local(s, "projected", M);
        s << "projectMultiplier(x,projected);\n";
    }
    local(s, "nextLambda", M);
    for (uint32_t row = 0; row < M; ++row) {
        std::string next = "x[" + std::to_string(lambdaInput + row) + "]";
        if (t.domain == Domain::NonNegative)
            next = "max(0.0," + next + ")";
        if (t.domain == Domain::NonPositive)
            next = "min(0.0," + next + ")";
        if (t.domain == Domain::Projected)
            next = "projected[" + std::to_string(row) + "]";
        s << "nextLambda[" << row << "]=" << next << ";dl[" << row << "]=nextLambda[" << row
          << "]-x_lambda[r.l+" << row << "u*r.stride];\n";
    }
    finite(s, "nextLambda", M);
    finite(s, "dl", M);
    vo = 0;
    for (uint32_t e = 0; e < t.spaces.size(); ++e) {
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
            s << "if(v" << e << ".flags==0u && x_variableEnabled[id" << e << "]!=0.0";
            for (uint32_t b = 0; b < e; ++b)
                s << " && id" << e << "!=id" << b;
            s << ") {\n";
            s << "retract" << e << "(input" << e << ",output" << e << ",tangent" << e << ");\n";
            finite(s, "output" + std::to_string(e), S);
            s << "}\n";
        }
        vo += T;
    }
    for (uint32_t row = 0; row < M; ++row)
        s << "x_lambda[r.l+" << row << "u*r.stride]=nextLambda[" << row << "];\n";
    vo = 0;
    for (uint32_t e = 0; e < t.spaces.size(); ++e) {
        const auto S = t.spaces[e]->stateSize, T = t.spaces[e]->tangentSize;
        if (jacobi) {
            for (uint32_t col = 0; col < T; ++col)
                s << "x_contributions[r.c+" << vo + col << "u*r.cs]=input" << e << "[" << S + col << "];\n";
        } else {
            s << "if(v" << e << ".flags==0u && x_variableEnabled[id" << e << "]!=0.0";
            for (uint32_t b = 0; b < e; ++b)
                s << " && id" << e << "!=id" << b;
            s << ") {\n";
            for (uint32_t col = 0; col < S; ++col)
                s << "x_q[v" << e << ".q+" << col << "u*v" << e << ".stride]=output" << e << "[" << col
                  << "];\n";
            s << "}\n";
        }
        vo += T;
    }
    s << "}\n";
    return s.str();
}
std::string incidenceKernel(const RelationType& t, bool scatter) {
    std::ostringstream s;
    s << "void main(){uint i=invocation();if(i>=step.count)return;uint "
         "id=x_relationWork[step.first+i];if(x_relationEnabled[id]==0.0)return;Relation r=relation(id);\n";
    uint32_t offset = 0;
    for (uint32_t e = 0; e < t.spaces.size(); ++e) {
        s << "uint id" << e << "=x_endpoints[r.e+" << e << "u];\n";
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
} // namespace whimsical::xpbd
