#ifndef AFTERLIGHT_LIGHT_SAMPLING
#define AFTERLIGHT_LIGHT_SAMPLING
// Shared production math: GLSL shaders and the native physical integration tests.
#ifdef __cplusplus
#define LIGHT_INLINE inline
using glm::dot;
using glm::cross;
using glm::normalize;
using glm::max;
using glm::abs;
using glm::clamp;
using std::sqrt;
using std::sin;
using std::cos;
#else
#define LIGHT_INLINE
#endif
const float LIGHT_PI = 3.14159265359f;
const int LIGHT_DIRECTIONAL = 0, LIGHT_SPOT = 1, LIGHT_POINT = 2, LIGHT_RECT = 3, LIGHT_CAPSULE = 4;
struct Light {
    vec4 positionRadius;  // world position, radius (m)
    vec4 colorIntensity;  // normalized RGB energy proportions, W (local) or W/m^2 (directional)
    vec4 directionType;   // local +Z emission axis in world space, type
    vec4 tangentWidth;    // local +X in world space, rect width (m)
    vec4 shape;           // rect height / capsule length, 1-cos(inner), 1-cos(outer), two-sided
    vec4 emission;        // area, angular normalization, 1-cos(directional angular radius), unused
};
struct PhysicalLightSample {
    vec3 position;
    vec3 direction;       // receiver -> emitter; independent of distance for directional lights
    vec3 normal;
    vec3 radiance;        // Le for finite emitters; delta samples carry integrated incident weight
    vec3 weight;          // radiance / conditional solid-angle PDF, or delta weight
    float distance;       // shadow ray extent; directional rays use a scene-independent long ray
    float pdfArea;        // conditional density on emitter (zero for delta/directional)
    float pdfSolidAngle;  // conditional density in sr^-1 (zero means delta)
};
LIGHT_INLINE float lightSpotProfile(Light l, float oneMinusCos) {
    float qi = l.shape.y, qo = l.shape.z;
    if (qi == qo) return oneMinusCos <= qo ? 1.f : 0.f;
    float t = clamp((qo-oneMinusCos)/(qo-qi), 0.f, 1.f);
    return t*t*(3.f-2.f*t);
}
LIGHT_INLINE vec3 lightSphere(float u, float v) {
    float z = 1.f-2.f*u, r = sqrt(max(0.f, 1.f-z*z)), phi = 2.f*LIGHT_PI*v;
    return vec3(r*cos(phi), r*sin(phi), z);
}
LIGHT_INLINE PhysicalLightSample samplePhysicalLight(Light l, vec3 receiver, vec2 uv) {
    PhysicalLightSample s;
    s.position = vec3(l.positionRadius);
    s.direction = vec3(0.f);
    s.normal = vec3(0.f);
    s.radiance = vec3(0.f);
    s.weight = vec3(0.f);
    s.distance = 0.f;
    s.pdfArea = 0.f;
    s.pdfSolidAngle = 0.f;
    int type = int(l.directionType.w);
    vec3 axis = vec3(l.directionType), tangent = vec3(l.tangentWidth), bitangent = cross(axis, tangent);
    vec3 power = vec3(l.colorIntensity)*l.colorIntensity.w;
    float radius = l.positionRadius.w, area = l.emission.x;
    if (type == LIGHT_DIRECTIONAL) {
        float oneMinusCos = l.emission.z;
        float c = 1.f-uv.x*oneMinusCos;
        float r = sqrt(uv.x*oneMinusCos*(2.f-uv.x*oneMinusCos)), phi = 2.f*LIGHT_PI*uv.y;
        s.direction = -axis*c + (tangent*cos(phi)+bitangent*sin(phi))*r;
        s.distance = 1.e30f;
        if (oneMinusCos > 0.f) {
            s.pdfSolidAngle = 1.f/(2.f*LIGHT_PI*oneMinusCos);
            s.radiance = power/(LIGHT_PI*oneMinusCos*(2.f-oneMinusCos));
            s.weight = power*(2.f/(2.f-oneMinusCos));
        } else {
            s.radiance = power;
            s.weight = power;
        }
        return s;
    }
    if (type == LIGHT_POINT && radius > 0.f) {
        s.normal = lightSphere(uv.x, uv.y);
        s.position += radius*s.normal;
    } else if (type == LIGHT_RECT) {
        s.normal = axis;
        s.position += tangent*((uv.x-.5f)*l.tangentWidth.w) + bitangent*((uv.y-.5f)*l.shape.x);
    } else if (type == LIGHT_SPOT && radius > 0.f) {
        s.normal = axis;
        float r = radius*sqrt(uv.x), phi = 2.f*LIGHT_PI*uv.y;
        s.position += r*(tangent*cos(phi)+bitangent*sin(phi));
    } else if (type == LIGHT_CAPSULE) {
        // Uniform area: cylinder and two hemispheres selected in proportion to area.
        // Capsule axis is local +Y; zero cylinder length is exactly a sphere.
        float length = l.shape.x, cylinderFraction = length/(length+2.f*radius);
        float phi = 2.f*LIGHT_PI*uv.y;
        if (uv.x < cylinderFraction) {
            s.normal = tangent*cos(phi)+axis*sin(phi);
            s.position += radius*s.normal + bitangent*((uv.x/cylinderFraction-.5f)*length);
        } else {
            float u = (uv.x-cylinderFraction)/(1.f-cylinderFraction);
            float z = 1.f-2.f*u, r = sqrt(max(0.f, 1.f-z*z));
            s.normal = (tangent*cos(phi)+axis*sin(phi))*r + bitangent*z;
            s.position += radius*s.normal + bitangent*(z >= 0.f ? .5f*length : -.5f*length);
        }
    }
    vec3 delta = s.position-receiver;
    float d2 = dot(delta,delta);
    // The coincident point has no defined direction, not a distance-softened emitter.
    if (d2 == 0.f) return s;
    s.distance = sqrt(d2);
    s.direction = delta/s.distance;
    // |axis-wOut|^2/2 evaluates 1-cos(theta) without cancellation for narrow spots.
    vec3 angularDelta = axis+s.direction;
    float profile = type == LIGHT_SPOT ? lightSpotProfile(l, .5f*dot(angularDelta,angularDelta)) : 1.f;
    if (area == 0.f) {
        s.radiance = power*(profile/(l.emission.y*d2));
        s.weight = s.radiance;
        return s;
    }
    s.pdfArea = 1.f/area;
    float projected = dot(s.normal,-s.direction);
    if (type == LIGHT_RECT && l.shape.w > 0.f) projected = abs(projected);
    if (projected <= 0.f) return s; // Includes convex emitter self-occlusion.
    s.pdfSolidAngle = d2/(area*projected);
    s.radiance = power*(profile/(area*l.emission.y));
    s.weight = s.radiance/s.pdfSolidAngle;
    return s;
}
#undef LIGHT_INLINE
#endif
