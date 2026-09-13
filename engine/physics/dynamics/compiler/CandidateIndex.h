#pragma once
#include <cstdint>
#include <string>

namespace whimsical::dynamics {
// Shared hash-grid primitives for relation candidates and expression members.
inline std::string candidateHash(uint32_t buckets) {
    return "uint bucket(ivec3 c){uvec3 u=uvec3(c);return ((u.x*73856093u)^(u.y*19349663u)^(u.z*83492791u))&" +
        std::to_string(buckets - 1) + "u;}\n";
}
inline std::string candidateInsert(const std::string& buffer, uint32_t heads, uint32_t next, uint32_t cells) {
    return "void insertMember(uint member,ivec3 c){uint at=" + std::to_string(cells) + "u+member*3u;" +
        buffer + "[at]=uint(c.x);" + buffer + "[at+1u]=uint(c.y);" + buffer + "[at+2u]=uint(c.z);" +
        buffer + "[" + std::to_string(next) + "u+member]=atomicExchange(" + buffer + "[" +
        std::to_string(heads) + "u+bucket(c)],member);}\n";
}
}
