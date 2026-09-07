// Application bridge for the pinned RTXDI-Library DI implementation.
// The reservoir streaming, normalization and reuse algorithms live in the SDK.
#define RTXDI_GLSL
#define RTXDI_ENABLE_PRESAMPLING 0
#define RTXDI_ALLOWED_BIAS_CORRECTION 3
#define lerp mix
#define rsqrt inversesqrt
#include "Rtxdi/RtxdiParameters.h"
#include "Rtxdi/DI/ReSTIRDIParameters.h"
#include "Rtxdi/Utils/RandomSamplerState.hlsli"
#include "Rtxdi/DI/Reservoir.hlsli"

// Four block-linear arrays: previous final, initial/temporal (later replay),
// spatial/final, previous replay. Replay keeps even occluded light selections.
layout(set = 0, binding = 13, std430) buffer DiReservoirs {
    RTXDI_PackedDIReservoir diReservoirs[];
};
layout(set = 0, binding = 14, std430) readonly buffer DiNeighbors { vec2 diNeighbors[]; };
layout(set = 0, binding = 15, std430) readonly buffer DiLights {
    vec4 lightDistribution[256]; // CDF, discrete PDF, unused, unused
    Light previousLights[256];
};
#define RTXDI_LIGHT_RESERVOIR_BUFFER diReservoirs
#define RTXDI_NEIGHBOR_OFFSETS_BUFFER diNeighbors
#include "Rtxdi/DI/ReservoirStorage.hlsli"

struct RAB_Surface { Surface data; vec3 viewDirection; float depth; };
struct RAB_LightInfo { Light data; };
struct RAB_LightSample { PhysicalLightSample data; };

RAB_Surface RAB_EmptySurface() {
    return RAB_Surface(Surface(vec3(0), vec3(0,1,0), vec3(0), 1, 0, vec3(0), 0), vec3(0,1,0), 0);
}
RAB_Surface RAB_GetGBufferSurface(ivec2 p, bool previousFrame) {
    if (!inBounds(p)) return RAB_EmptySurface();
    Surface s = surfaceAt(p);
    float z = imageLoad(gViewZ, p).x;
    if (previousFrame) {
        vec4 position = imageLoad(previousPosition, p);
        vec4 normal = imageLoad(previousNormal, p);
        vec4 albedo = imageLoad(previousAlbedo, p);
        s = Surface(position.xyz, safeNormalize(normal.xyz), albedo.rgb, normal.w,
                    albedo.w, vec3(0), uint(position.w + .5));
        z = imageLoad(previousViewZ, p).x;
    }
    vec3 eye = previousFrame ? g.previousEye.xyz : g.eyeTime.xyz;
    return RAB_Surface(s, safeNormalize(eye - s.p), z);
}
bool RAB_IsSurfaceValid(RAB_Surface s) { return s.data.id != 0; }
vec3 RAB_GetSurfaceNormal(RAB_Surface s) { return s.data.n; }
float RAB_GetSurfaceLinearDepth(RAB_Surface s) { return s.depth; }
Surface RAB_GetMaterial(RAB_Surface s) { return s.data; }
bool RAB_AreMaterialsSimilar(Surface a, Surface b) {
    return abs(a.roughness-b.roughness) < .2 && abs(a.metallic-b.metallic) < .1 &&
           length(a.albedo-b.albedo) < .3;
}
ivec2 RAB_ClampSamplePositionIntoView(ivec2 p, bool previousFrame) {
    return clamp(p, ivec2(0), ivec2(g.resolution.xy)-1);
}
RAB_LightInfo RAB_EmptyLightInfo() { return RAB_LightInfo(Light(vec4(0),vec4(0),vec4(0),vec4(0),vec4(0),vec4(0))); }
RAB_LightSample RAB_EmptyLightSample() {
    return RAB_LightSample(PhysicalLightSample(vec3(0),vec3(0),vec3(0),vec3(0),vec3(0),0,0,0));
}
RAB_LightInfo RAB_LoadLightInfo(uint id, bool previousFrame) {
    return RAB_LightInfo(previousFrame ? previousLights[id] : lights[id]);
}
// Engine light slots are stable while the count is unchanged. A topology change
// restarts DI/NRD history; parameter animation uses previousLights and replay.
int RAB_TranslateLightIndex(uint id, bool toPrevious) { return id < g.counts.y ? int(id) : -1; }
RAB_LightSample RAB_SamplePolymorphicLight(RAB_LightInfo info, RAB_Surface s, vec2 uv) {
    return RAB_LightSample(samplePhysicalLight(info.data,s.data.p,uv));
}
void diSignal(RAB_Surface s, RAB_LightSample l, out vec3 diffuse, out vec3 specular) {
    vec3 direction = l.data.direction;
    float nl = max(dot(s.data.n,direction),0);
    // RTXDI reservoirs live in (light ID, uniform UV), not solid-angle measure.
    // Include Le/pdfOmega in the target; StreamSample divides only by discrete p(light).
    // Re-evaluation recomputes the entire UV integrand at the destination surface.
    diffuse = l.data.weight * (nl/PI);
    specular = l.data.weight * specularBrdf(s.data,s.viewDirection,direction) * nl;
}
float RAB_GetLightSampleTargetPdfForSurface(RAB_LightSample l, RAB_Surface s) {
    vec3 d, sp; diSignal(s,l,d,sp);
    return luminance(d*s.data.albedo*(1-s.data.metallic)+sp);
}
bool RAB_GetConservativeVisibility(RAB_Surface s, RAB_LightSample l) {
    return lightVisible(s.data.p,s.data.n,l.data);
}
bool RAB_GetTemporalConservativeVisibility(RAB_Surface currentSurface, RAB_Surface previousSurface,
                                         RAB_LightSample l) {
    // RTXDI permits the current TLAS as a cheaper approximation. We explicitly
    // disable the visibility shortcut; an old visible sample can now be occluded.
    return RAB_GetConservativeVisibility(previousSurface,l);
}
RTXDI_ReservoirBufferParameters diBufferParams() {
    uint row = ((uint(g.resolution.x)+RTXDI_RESERVOIR_BLOCK_SIZE-1)/RTXDI_RESERVOIR_BLOCK_SIZE)*RTXDI_RESERVOIR_BLOCK_SIZE*RTXDI_RESERVOIR_BLOCK_SIZE;
    return RTXDI_ReservoirBufferParameters(row,row*((uint(g.resolution.y)+RTXDI_RESERVOIR_BLOCK_SIZE-1)/RTXDI_RESERVOIR_BLOCK_SIZE),0,0);
}
RTXDI_RuntimeParameters diRuntimeParams() { return RTXDI_RuntimeParameters(1023,0,g.counts.z,0); }

void diInitial(ivec2 pixel) {
    RTXDI_DIReservoir reservoir = RTXDI_EmptyDIReservoir();
    RTXDI_RandomSamplerState randomState = RTXDI_InitRandomSampler(uvec2(pixel),g.counts.z,1);
    RAB_Surface s = RAB_GetGBufferSurface(pixel,false);
    RTXDI_StoreDIReservoir(reservoir,diBufferParams(),uvec2(pixel),3);
    if (g.counts.y > 0) {
        for (int i=0; i<8; ++i) {
            float pick = RTXDI_GetNextRandom(randomState);
            uint lo=0, hi=g.counts.y-1;
            while (lo<hi) {
                uint middle=(lo+hi)/2;
                if (pick < lightDistribution[middle].x) hi=middle;
                else lo=middle+1;
            }
            // Evaluate exactly the UV that will survive SDK reservoir packing.
            vec2 uv = floor(vec2(RTXDI_GetNextRandom(randomState),RTXDI_GetNextRandom(randomState))*65535)/65535;
            RAB_LightSample l = RAB_SamplePolymorphicLight(RAB_LoadLightInfo(lo,false),s,uv);
            RTXDI_StreamSample(reservoir,lo,uv,RTXDI_GetNextRandom(randomState),
                RAB_GetLightSampleTargetPdfForSurface(l,s),1/lightDistribution[lo].y);
        }
        RTXDI_FinalizeResampling(reservoir,1,reservoir.M);
        // Initial RIS represents one resampled candidate, as in RTXDI initial sampling.
        reservoir.M = 1;
        if (RTXDI_IsValidDIReservoir(reservoir)) {
            RAB_LightSample l = RAB_SamplePolymorphicLight(
                RAB_LoadLightInfo(RTXDI_GetDIReservoirLightIndex(reservoir),false),s,RTXDI_GetDIReservoirSampleUV(reservoir));
            bool lit = RAB_GetConservativeVisibility(s,l);
            RTXDI_StoreVisibilityInDIReservoir(reservoir,vec3(lit ? 1 : 0),false);
            RTXDI_StoreDIReservoir(reservoir,diBufferParams(),uvec2(pixel),3);
            RTXDI_StoreVisibilityInDIReservoir(reservoir,vec3(lit ? 1 : 0),true);
        }
    }
    RTXDI_StoreDIReservoir(reservoir,diBufferParams(),uvec2(pixel),1);
}
