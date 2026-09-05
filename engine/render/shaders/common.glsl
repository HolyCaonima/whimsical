#ifndef AFTERLIGHT_COMMON
#define AFTERLIGHT_COMMON
const float PI = 3.14159265359;
struct Instance {
    mat4 model;
    mat4 previousModel;
    uvec4 info;
};
struct Material {
    vec4 colorRoughness;
    vec4 emissionMetallic;
};
struct Light {
    vec4 positionRadius;
    vec4 colorIntensity;
};
struct Vertex {
    vec4 position;
    vec4 normal;
};
// stats = finalized contribution weight W, represented sample count M, selected target p-hat, validity.
struct DIReservoir {
    vec4 sampleLight;
    vec4 stats;
};
struct GIReservoir {
    vec4 positionDistance;
    vec4 normalKind;
    vec4 radiancePdf;
    vec4 stats;
};
layout(set = 0, binding = 0, std140) uniform Globals {
    mat4 viewProjection;
    mat4 previousViewProjection;
    mat4 view;
    mat4 inverseViewProjection;
    vec4 eyeTime;
    vec4 resolution;
    vec4 player;
    vec4 destination;
    uvec4 counts;
}
g;
layout(set = 0, binding = 1, std430) readonly buffer Instances {
    Instance instances[];
};
layout(set = 0, binding = 2, std430) readonly buffer Materials {
    Material materials[];
};
layout(set = 0, binding = 3, std430) readonly buffer Lights {
    Light lights[];
};
#ifdef COMPUTE_PASS
layout(set = 0, binding = 4, std430) readonly buffer Vertices {
    Vertex vertices[];
};
layout(set = 0, binding = 5, std430) readonly buffer Indices {
    uint indices[];
};
layout(set = 0, binding = 6) uniform accelerationStructureEXT scene;
layout(set = 0, binding = 7, rgba16f) uniform image2D gAlbedo;
layout(set = 0, binding = 8, rgba16f) uniform image2D gNormal;
layout(set = 0, binding = 9, rgba32f) uniform image2D gPosition;
layout(set = 0, binding = 10, rgba16f) uniform image2D gMotion;
layout(set = 0, binding = 11, r32f) uniform image2D gViewZ;
layout(set = 0, binding = 12, rgba16f) uniform image2D gEmission;
layout(set = 0, binding = 13, std430) buffer CurrentDI {
    DIReservoir currentDI[];
};
layout(set = 0, binding = 14, std430) readonly buffer PreviousDI {
    DIReservoir previousDI[];
};
layout(set = 0, binding = 15, std430) buffer CandidateDI {
    DIReservoir candidateDI[];
};
layout(set = 0, binding = 16, std430) buffer CurrentGI {
    GIReservoir currentGI[];
};
layout(set = 0, binding = 17, std430) readonly buffer PreviousGI {
    GIReservoir previousGI[];
};
layout(set = 0, binding = 18, std430) buffer CandidateGI {
    GIReservoir candidateGI[];
};
layout(set = 0, binding = 19, rgba16f) uniform image2D rawDiffuse;
layout(set = 0, binding = 20, rgba16f) uniform image2D rawSpecular;
layout(set = 0, binding = 21, rgba16f) uniform image2D denoisedDiffuse;
layout(set = 0, binding = 22, rgba16f) uniform image2D denoisedSpecular;
layout(set = 0, binding = 23, rgba16f) uniform image2D finalImage;
layout(set = 0, binding = 24, rgba16f) uniform image2D previousNormal;
layout(set = 0, binding = 25, rgba32f) uniform image2D previousPosition;
layout(set = 0, binding = 26, rgba16f) uniform image2D directDebug;
layout(set = 0, binding = 27, rgba16f) uniform image2D indirectDebug;
layout(set = 0, binding = 28, rgba8) uniform image2D hudImage;
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
bool visible(vec3 p, vec3 n, vec3 target) {
    vec3 delta = target - p;
    float distance = length(delta);
    if (distance < .03)
        return true;
    rayQueryEXT query;
    rayQueryInitializeEXT(query, scene, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, 0xff,
                          p + n * .012, .001, delta / distance, max(.002, distance - .025));
    while (rayQueryProceedEXT(query)) {
    }
    return rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT;
}
bool traceSurface(vec3 origin, vec3 direction, out Surface hit, out float distance) {
    rayQueryEXT query;
    rayQueryInitializeEXT(query, scene, gl_RayFlagsOpaqueEXT, 0xff, origin, .002, direction, 150.0);
    while (rayQueryProceedEXT(query)) {
    }
    if (rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT) {
        distance = 150;
        return false;
    }
    distance = rayQueryGetIntersectionTEXT(query, true);
    uint instanceIndex = rayQueryGetIntersectionInstanceCustomIndexEXT(query, true),
         primitive = rayQueryGetIntersectionPrimitiveIndexEXT(query, true);
    Instance instance = instances[instanceIndex];
    uint first = instance.info.y + primitive * 3;
    Vertex a = vertices[indices[first]], b = vertices[indices[first + 1]], c = vertices[indices[first + 2]];
    vec2 uv = rayQueryGetIntersectionBarycentricsEXT(query, true);
    vec3 normal = a.normal.xyz * (1 - uv.x - uv.y) + b.normal.xyz * uv.x + c.normal.xyz * uv.y;
    hit.n = safeNormalize(transpose(inverse(mat3(instance.model))) * normal);
    if (dot(hit.n, direction) > 0)
        hit.n = -hit.n;
    hit.p = origin + direction * distance;
    Material m = materials[instance.info.x];
    hit.albedo = m.colorRoughness.rgb;
    hit.roughness = m.colorRoughness.a;
    hit.metallic = m.emissionMetallic.a;
    hit.emission = m.emissionMetallic.rgb;
    hit.id = instance.info.z;
    return true;
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
void directSignal(Surface s, vec4 sampleLight, out vec3 diffuse, out vec3 specular) {
    uint id = min(uint(sampleLight.w + .5), g.counts.y - 1);
    Light l = lights[id];
    vec3 delta = sampleLight.xyz - s.p;
    float d2 = max(dot(delta, delta), .04);
    vec3 direction = delta * inversesqrt(d2), v = safeNormalize(g.eyeTime.xyz - s.p);
    vec3 radiance =
        l.colorIntensity.rgb * l.colorIntensity.a / (d2 + l.positionRadius.a * l.positionRadius.a);
    float nl = max(dot(s.n, direction), 0);
    diffuse = radiance * (nl / PI);
    specular = radiance * specularBrdf(s, v, direction) * nl;
}
float diTarget(Surface s, vec4 light) {
    vec3 d, sp;
    directSignal(s, light, d, sp);
    return max(1e-8, luminance(d * s.albedo * (1 - s.metallic) + sp));
}
vec4 lightSample() {
    uint id = min(uint(random() * float(g.counts.y)), g.counts.y - 1);
    Light l = lights[id];
    float z = 1 - 2 * random(), a = 2 * PI * random();
    vec3 unit = vec3(sqrt(max(0, 1 - z * z)) * cos(a), z, sqrt(max(0, 1 - z * z)) * sin(a));
    return vec4(l.positionRadius.xyz + unit * l.positionRadius.w, float(id));
}
void diAdd(inout DIReservoir r, vec4 sampleLight, float weight, float m, float target) {
    if (isnan(weight) || isinf(weight) || weight <= 0) {
        r.stats.y += m;
        return;
    }
    r.stats.x += weight;
    r.stats.y += m;
    if (random() * r.stats.x < weight) {
        r.sampleLight = sampleLight;
        r.stats.z = target;
        r.stats.w = 1;
    }
}
void diFinalize(inout DIReservoir r) {
    r.stats.x = r.stats.z > 0 ? r.stats.x / max(r.stats.y * r.stats.z, 1e-12) : 0;
    r.stats.y = min(r.stats.y, 32);
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
vec3 secondaryLighting(Surface s) {
    vec4 l = lightSample();
    vec3 direction = safeNormalize(l.xyz - s.p);
    float nl = max(dot(s.n, direction), 0), d2 = dot(l.xyz - s.p, l.xyz - s.p);
    Light light = lights[uint(l.w + .5)];
    vec3 result = light.colorIntensity.rgb * light.colorIntensity.a * float(g.counts.y) * nl /
                  (PI * (d2 + light.positionRadius.w * light.positionRadius.w));
    if (nl <= 0 || !visible(s.p, s.n, l.xyz))
        result = vec3(0);
    return result * s.albedo * (1 - s.metallic) + s.emission;
}
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
#endif
#endif
