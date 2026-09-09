#ifndef WHIMSICAL_COMMON
#define WHIMSICAL_COMMON
#extension GL_EXT_nonuniform_qualifier : require
const float PI = 3.14159265359;
struct OutlineEntry {
    uvec4 identity;
    vec4 color;
};
struct Instance {
    mat4 model;
    mat4 previousModel;
    uvec4 info;
};
struct Material {
    uvec4 info; // renderer-owned Shader table index
    vec4 properties[8];
    ivec4 textures[2];
};
#include "../../lighting/LightSampling.h"
struct Vertex {
    vec4 position;
    vec4 normal;
    vec4 color;
    vec4 previousPosition;
    vec4 uv;
    vec4 tangent;
};
// stats = finalized contribution weight W, represented sample count M, selected target p-hat, validity.
struct GIReservoir {
    vec4 positionDistance;
    vec4 normalKind;
    vec4 radiancePdf;
    vec4 stats;
};
// Resource declarations are generated from the render graph registry, which is the only
// place that assigns a binding number. See engine/render/RenderResources.cpp.
#include "generated/graph.shared.glsl"
#ifdef SURFACE_PASS
#include "surface.glsl"
#include "surface_link.glsl"
#endif
#ifdef COMPUTE_PASS
#include "generated/graph.compute.glsl"
uint rng;
uint hash(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
float random() {
    rng = hash(rng + 0x9e3779b9u);
    return float(rng >> 8) * (1.0 / 16777216.0);
}
float luminance(vec3 v) {
    return dot(v, vec3(.2126, .7152, .0722));
}
uint indexOf(ivec2 p) {
    return uint(p.y) * uint(g.resolution.x) + uint(p.x);
}
bool inBounds(ivec2 p) {
    return all(greaterThanEqual(p, ivec2(0))) && all(lessThan(p, ivec2(g.resolution.xy)));
}
vec3 safeNormalize(vec3 v) {
    return v * inversesqrt(max(dot(v, v), 1e-12));
}
mat3 basis(vec3 n) {
    vec3 t = safeNormalize(cross(abs(n.y) < .95 ? vec3(0, 1, 0) : vec3(1, 0, 0), n));
    return mat3(t, cross(n, t), n);
}
vec3 cosineDirection(vec3 n) {
    float r = sqrt(random()), a = random() * 2 * PI;
    return basis(n) * vec3(r * cos(a), r * sin(a), sqrt(max(0, 1 - r * r)));
}
vec3 sky(vec3 d) {
    return mix(vec3(.025, .045, .055), vec3(.27, .37, .43), pow(max(d.y, 0), .45));
}
struct Surface {
    vec3 p;
    vec3 n;
    vec3 albedo;
    float roughness;
    float metallic;
    vec3 emission;
    uint id;
};
Surface surfaceAt(ivec2 pixel) {
    vec4 p = imageLoad(gPosition, pixel), n = imageLoad(gNormal, pixel), a = imageLoad(gAlbedo, pixel);
    Surface s;
    s.p = p.xyz;
    s.id = uint(p.w + .5);
    s.n = safeNormalize(n.xyz);
    s.roughness = max(n.w, .08);
    s.albedo = a.rgb;
    s.metallic = a.w;
    s.emission = imageLoad(gEmission, pixel).rgb;
    return s;
}
#ifdef SURFACE_PASS
MaterialContext rayMaterialContext(uint instanceIndex, uint primitive, vec2 bary,
                                   float distance, bool frontFacing, vec3 origin, vec3 direction) {
    Instance instance = instances[instanceIndex];
    uint first = instance.info.y + primitive * 3;
    Vertex a = vertices[indices[first]], b = vertices[indices[first+1]], c = vertices[indices[first+2]];
    vec3 w = vec3(1-bary.x-bary.y, bary);
    MaterialContext ctx;
    ctx.material = instance.info.x;
    ctx.position = origin + direction * distance;
    ctx.normal = normalize(transpose(inverse(mat3(instance.model))) *
                           (a.normal.xyz*w.x + b.normal.xyz*w.y + c.normal.xyz*w.z));
    ctx.frontFacing = frontFacing;
    if (!ctx.frontFacing) ctx.normal = -ctx.normal;
    ctx.tangent = a.tangent*w.x + b.tangent*w.y + c.tangent*w.z;
    ctx.tangent.xyz = mat3(instance.model) * ctx.tangent.xyz;
    ctx.vertexColor = a.color.rgb*w.x + b.color.rgb*w.y + c.color.rgb*w.z;
    ctx.uv = a.uv.xy*w.x + b.uv.xy*w.y + c.uv.xy*w.z;
    ctx.viewDirection = -direction;
    // Explicit secondary-ray sampling policy. Surface math is identical; derivatives
    // are unavailable in ray queries, so textures use LOD 2 and a world-space cone.
    ctx.textureLod = 2;
    ctx.footprint = max(.001, distance / g.resolution.y);
    return ctx;
}
bool visibleRay(vec3 origin, vec3 direction, float distance, uint mask) {
    if (distance <= .001) return true;
    rayQueryEXT query;
    rayQueryInitializeEXT(query, scene, gl_RayFlagsTerminateOnFirstHitEXT, mask,
                          origin, .001, direction, distance);
    while (rayQueryProceedEXT(query)) {
        if (rayQueryGetIntersectionTypeEXT(query, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
            MaterialContext ctx = rayMaterialContext(
                rayQueryGetIntersectionInstanceCustomIndexEXT(query,false),
                rayQueryGetIntersectionPrimitiveIndexEXT(query,false),
                rayQueryGetIntersectionBarycentricsEXT(query,false),
                rayQueryGetIntersectionTEXT(query,false),
                rayQueryGetIntersectionFrontFaceEXT(query,false), origin, direction);
            if (AcceptSurface(ctx, EvaluateSurface(ctx)))
                rayQueryConfirmIntersectionEXT(query);
        }
    }
    return rayQueryGetIntersectionTypeEXT(query,true) == gl_RayQueryCommittedIntersectionNoneEXT;
}
bool visibleTo(vec3 p, vec3 n, vec3 target, uint mask) {
    vec3 origin = p+n*.002, delta = target-origin;
    float distance = length(delta);
    if (distance <= .002) return true;
    return visibleRay(origin, delta/distance, distance-.001, mask);
}
bool visible(vec3 p, vec3 n, vec3 target) {
    return visibleTo(p,n,target,0xffu);
}
bool lightVisible(vec3 p, vec3 n, PhysicalLightSample l) {
    if (dot(l.weight, l.weight) == 0) return false;
    if (l.distance > 1.e29) return visibleRay(p+n*.002,l.direction,l.distance,0x01u);
    return visibleTo(p,n,l.position,0x01u);
}
bool traceSurface(vec3 origin, vec3 direction, out Surface hit, out float distance) {
    rayQueryEXT query;
    rayQueryInitializeEXT(query, scene, 0, 0xff, origin, .002, direction, 150.0);
    while (rayQueryProceedEXT(query)) {
        if (rayQueryGetIntersectionTypeEXT(query,false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
            MaterialContext ctx = rayMaterialContext(
                rayQueryGetIntersectionInstanceCustomIndexEXT(query,false),
                rayQueryGetIntersectionPrimitiveIndexEXT(query,false),
                rayQueryGetIntersectionBarycentricsEXT(query,false),
                rayQueryGetIntersectionTEXT(query,false),
                rayQueryGetIntersectionFrontFaceEXT(query,false), origin, direction);
            if (AcceptSurface(ctx, EvaluateSurface(ctx)))
                rayQueryConfirmIntersectionEXT(query);
        }
    }
    if (rayQueryGetIntersectionTypeEXT(query,true) == gl_RayQueryCommittedIntersectionNoneEXT) {
        distance = 150;
        return false;
    }
    distance = rayQueryGetIntersectionTEXT(query,true);
    MaterialContext ctx = rayMaterialContext(
                rayQueryGetIntersectionInstanceCustomIndexEXT(query,true),
                rayQueryGetIntersectionPrimitiveIndexEXT(query,true),
                rayQueryGetIntersectionBarycentricsEXT(query,true),
                rayQueryGetIntersectionTEXT(query,true),
                rayQueryGetIntersectionFrontFaceEXT(query,true), origin, direction);
    SurfaceData surface = EvaluateSurface(ctx);
    hit.p = ctx.position;
    hit.n = normalize(surface.normal);
    hit.albedo = surface.albedo;
    hit.roughness = max(surface.roughness,.08);
    hit.metallic = surface.metallic;
    hit.emission = surface.emission;
    hit.id = instances[rayQueryGetIntersectionInstanceCustomIndexEXT(query,true)].info.z;
    return true;
}
#endif
// RTXDI's material demodulation baseline. The same factor is used by resolve
// and composition, including the raw view, so this does not change the BRDF.
vec3 specularMaterialFactor(Surface s) {
    return max(mix(vec3(.04), s.albedo, s.metallic), vec3(.01));
}
float smithG1(float cosTheta, float a) {
    return 2 * cosTheta / max(cosTheta + sqrt(a * a + (1 - a * a) * cosTheta * cosTheta), 1e-6);
}
vec3 specularBrdf(Surface s, vec3 v, vec3 l) {
    float nv = max(dot(s.n, v), .001), nl = max(dot(s.n, l), .0);
    vec3 h = safeNormalize(v + l);
    float nh = max(dot(s.n, h), 0), vh = max(dot(v, h), 0), a = s.roughness * s.roughness, a2 = a * a;
    float denominator = nh * nh * (a2 - 1) + 1, D = a2 / max(PI * denominator * denominator, 1e-6),
          G = smithG1(nv, a) * smithG1(nl, a);
    vec3 f0 = mix(vec3(.04), s.albedo, s.metallic), F = f0 + (1 - f0) * pow(1 - vh, 5);
    return D * G * F / max(4 * nv * nl, 1e-5);
}
vec3 giDirection(Surface s, GIReservoir r) {
    return r.normalKind.w < 0 ? r.positionDistance.xyz : safeNormalize(r.positionDistance.xyz - s.p);
}
vec3 giSignal(Surface s, GIReservoir r) {
    return r.radiancePdf.rgb * (max(dot(s.n, giDirection(s, r)), 0) / PI);
}
float giTarget(Surface s, GIReservoir r) {
    return max(luminance(giSignal(s, r)), 1e-8);
}
void giAdd(inout GIReservoir r, GIReservoir candidate, float weight, float m, float target) {
    if (isnan(weight) || isinf(weight) || weight <= 0) {
        r.stats.y += m;
        return;
    }
    vec4 stats = r.stats;
    stats.x += weight;
    stats.y += m;
    if (random() * stats.x < weight) {
        r = candidate;
        stats.z = target;
        stats.w = 1;
    }
    r.stats = stats;
}
void giFinalize(inout GIReservoir r) {
    r.stats.x = r.stats.z > 0 ? r.stats.x / max(r.stats.y * r.stats.z, 1e-12) : 0;
    r.stats.y = min(r.stats.y, 16);
}
#ifdef SURFACE_PASS
vec3 secondaryLighting(Surface s, vec3 viewDirection) {
    if (g.counts.y == 0) return s.emission;
    uint id = min(uint(random()*float(g.counts.y)), g.counts.y-1);
    PhysicalLightSample l = samplePhysicalLight(lights[id], s.p, vec2(random(),random()));
    float nl = max(dot(s.n, l.direction), 0);
    vec3 brdf = s.albedo*(1-s.metallic)/PI + specularBrdf(s,viewDirection,l.direction);
    vec3 result = l.weight * brdf * (float(g.counts.y)*nl);
    if (nl <= 0 || !lightVisible(s.p, s.n, l))
        result = vec3(0);
    return result + s.emission;
}
#endif
// Isotropic GGX visible-normal sampling in the stretched view hemisphere.
vec3 sampleSpecular(Surface s, vec3 viewDirection, out float pdf) {
    mat3 frame = basis(s.n);
    vec3 v = transpose(frame) * viewDirection;
    float alpha = s.roughness * s.roughness;
    vec3 stretched = safeNormalize(vec3(alpha * v.xy, max(v.z, .001)));
    float lensq = dot(stretched.xy, stretched.xy);
    vec3 t1 = lensq > 0 ? vec3(-stretched.y, stretched.x, 0) / sqrt(lensq) : vec3(1, 0, 0),
         t2 = cross(stretched, t1);
    float r = sqrt(random()), phi = 2 * PI * random(), a = r * cos(phi), b = r * sin(phi),
          blend = .5 * (1 + stretched.z);
    b = (1 - blend) * sqrt(max(0, 1 - a * a)) + blend * b;
    vec3 hst = a * t1 + b * t2 + sqrt(max(0, 1 - a * a - b * b)) * stretched,
         h = safeNormalize(vec3(alpha * hst.xy, max(0, hst.z)));
    h = frame * h;
    vec3 direction = reflect(-viewDirection, h);
    float nh = max(dot(s.n, h), 0), nv = max(dot(s.n, viewDirection), .001),
          den = nh * nh * (alpha * alpha - 1) + 1, D = alpha * alpha / max(PI * den * den, 1e-8);
    pdf = D * smithG1(nv, alpha) / (4 * nv);
    return direction;
}
#ifdef RTXDI_PASS
#include "rtxdi_bridge.glsl"
#endif
#endif
#endif
