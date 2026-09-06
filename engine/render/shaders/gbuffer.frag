#version 460
#extension GL_GOOGLE_include_directive : require
#define FRAGMENT_PASS
#include "common.glsl"
layout(location = 0) in vec3 worldPosition;
layout(location = 1) in vec3 worldNormal;
layout(location = 2) in vec4 previousClip;
layout(location = 3) flat in uint instanceIndex;
layout(location = 4) in vec3 vertexColor;
layout(location = 5) in vec2 texcoord;
layout(location = 6) in vec4 worldTangent;
layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outPosition;
layout(location = 3) out vec4 outMotion;
layout(location = 4) out float outViewZ;
layout(location = 5) out vec4 outEmission;
void main() {
    Instance i = instances[instanceIndex];
    Material m = materials[i.info.x];
    vec3 n = normalize(worldNormal), color = m.colorRoughness.rgb * vertexColor;
    vec2 uvMaterial=texcoord*m.surface.xy;
    float lod=0;
    color*=sampleMaterialTexture(m.textures.x,uvMaterial,lod).rgb;
    n=materialNormal(m,uvMaterial,n,worldTangent,lod);
    vec3 orm=sampleMaterialTexture(m.textures.z,uvMaterial,lod).rgb;
    if (m.surface.w > .5 && n.y > .95 && worldPosition.y < .08) {
        vec2 uv = worldPosition.xz;
        uv.x += mod(floor(uv.y), 2) * .5;
        vec2 f = fract(uv), aa = fwidth(uv);
        vec2 edge = smoothstep(vec2(.014), vec2(.014) + aa, min(f, 1 - f));
        color *= mix(.32, 1, min(edge.x, edge.y));
        float stain = sin(worldPosition.x * .7) * cos(worldPosition.z * .53);
        color *= .95 + .05 * stain;
    }
    outAlbedo = vec4(color, m.emissionMetallic.w*orm.b);
    outNormal = vec4(n / max(max(abs(n.x), abs(n.y)), abs(n.z)), m.colorRoughness.w*orm.g);
    outPosition = vec4(worldPosition, float(i.info.z));
    vec2 currentUv = gl_FragCoord.xy / g.resolution.xy, prevUv = previousClip.xy / previousClip.w * .5 + .5;
    outMotion = vec4(prevUv - currentUv, previousClip.w, 0);
    outViewZ = abs((g.view * vec4(worldPosition, 1)).z);
    outEmission = vec4(m.emissionMetallic.rgb, 0);
}
