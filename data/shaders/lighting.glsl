// Shared lighting for part.frag, terrain.frag and water.frag: the sun with
// soft shadows, sky-tinted ambient, up to 16 point / spot lights, sky
// reflections, fog, exposure and ACES tone mapping. Colours come in as sRGB,
// lighting is linear, the result is sRGB.

uniform vec3 uCameraPos;
uniform vec3 uSunDirection;     // towards the sun
uniform vec3 uSunColor;         // colour * intensity (linear)
uniform vec3 uAmbient;          // linear
uniform vec3 uSkyColor;         // linear
uniform vec3 uHorizonColor;     // linear
uniform vec3 uFogColor;         // linear
uniform float uFogStart;
uniform float uFogEnd;
uniform float uExposure;        // multiplier
uniform sampler2DShadow uShadowMap;
uniform int uShadows;
uniform float uShadowTexel;

#define MAX_LIGHTS 16
uniform int uLightCount;
uniform vec4 uLightPosRange[MAX_LIGHTS];   // xyz position, w range
uniform vec4 uLightColorType[MAX_LIGHTS];  // rgb colour * brightness (linear), w: 0 point, 1 spot
uniform vec4 uLightDirCone[MAX_LIGHTS];    // xyz direction the spot points at, w cos(half angle)

vec3 toLinear(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }

float hash(vec3 p) {
    p = fract(p * 0.3183099 + 0.1);
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float noise(vec3 x) {
    vec3 i = floor(x);
    vec3 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash(i + vec3(0, 0, 0)), hash(i + vec3(1, 0, 0)), f.x),
                   mix(hash(i + vec3(0, 1, 0)), hash(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(hash(i + vec3(0, 0, 1)), hash(i + vec3(1, 0, 1)), f.x),
                   mix(hash(i + vec3(0, 1, 1)), hash(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}

vec3 skyAt(vec3 dir) {
    float up = clamp(dir.y, -1.0, 1.0);
    vec3 sky = mix(uHorizonColor, uSkyColor, pow(max(up, 0.0), 0.35));
    return up < 0.0 ? mix(uHorizonColor, uAmbient * 0.8, min(-up * 3.0, 1.0)) : sky;
}

float shadowFactor(vec4 lightPos, vec3 normal) {
    if (uShadows == 0) return 1.0;
    vec3 p = lightPos.xyz / lightPos.w * 0.5 + 0.5;
    if (p.z > 1.0) return 1.0;
    float bias = max(0.0015 * (1.0 - dot(normal, uSunDirection)), 0.0004);
    float sum = 0.0;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            sum += texture(uShadowMap, vec3(p.xy + vec2(x, y) * uShadowTexel, p.z - bias));
    return sum / 9.0;
}

// Lit colour of an opaque surface (linear). `reflectance` adds sky reflections.
vec3 shadeSurface(vec3 worldPos, vec4 lightPos, vec3 N, vec3 V, vec3 albedo,
                  float roughness, float specular, float metallic, float reflectance) {
    vec3 diffuseColor = albedo * (1.0 - metallic);
    vec3 specColor = mix(vec3(specular), albedo, metallic);
    float shininess = mix(6.0, 512.0, pow(1.0 - roughness, 2.0));

    // Sky-tinted ambient: brighter from above, darker from below.
    float hemi = N.y * 0.5 + 0.5;
    vec3 ambient = uAmbient * mix(0.6, 1.1, hemi) + uSkyColor * 0.07 * hemi;
    vec3 light = ambient * diffuseColor;

    // Sun.
    float ndl = max(dot(N, uSunDirection), 0.0);
    if (ndl > 0.0) {
        // Shadows keep a little sun (bounce light) so they read soft, not black.
        float shadow = mix(0.22, 1.0, shadowFactor(lightPos, N));
        vec3 H = normalize(uSunDirection + V);
        float spec = pow(max(dot(N, H), 0.0), shininess) * (shininess + 8.0) / 25.0;
        light += uSunColor * shadow * (diffuseColor * ndl + specColor * spec * ndl);
    }

    // Point and spot lights.
    for (int i = 0; i < uLightCount; ++i) {
        vec3 toLight = uLightPosRange[i].xyz - worldPos;
        float d = length(toLight);
        float range = uLightPosRange[i].w;
        if (d >= range) continue;
        vec3 L = toLight / max(d, 1e-4);
        float atten = pow(clamp(1.0 - pow(d / range, 2.0), 0.0, 1.0), 2.0) / (1.0 + d * d * 0.02);
        if (uLightColorType[i].w > 0.5) {
            float cosAngle = dot(-L, uLightDirCone[i].xyz);
            float edge = uLightDirCone[i].w;
            atten *= smoothstep(edge, mix(edge, 1.0, 0.25), cosAngle);
        }
        float nl = max(dot(N, L), 0.0);
        vec3 H = normalize(L + V);
        float spec = pow(max(dot(N, H), 0.0), shininess) * (shininess + 8.0) / 25.0;
        light += uLightColorType[i].rgb * atten * (diffuseColor * nl + specColor * spec * nl);
    }

    // Reflections of the sky with Fresnel.
    float fresnel = pow(1.0 - max(dot(N, V), 0.0), 5.0);
    float reflectAmount = clamp(reflectance + metallic * 0.5 + fresnel * specular * 0.5, 0.0, 1.0);
    vec3 env = skyAt(reflect(-V, N)) * mix(vec3(1.0), albedo, metallic);
    return mix(light, env, reflectAmount * (1.0 - roughness * 0.7));
}

// Fog, exposure, an optional highlight tint, tone mapping, sRGB.
vec3 finishColor(vec3 color, vec3 worldPos, vec3 tint, float tintAmount) {
    float dist = length(uCameraPos - worldPos);
    float fog = clamp((dist - uFogStart) / max(uFogEnd - uFogStart, 0.001), 0.0, 1.0);
    color = mix(color, uFogColor, fog);
    color *= uExposure;
    color = mix(color, toLinear(tint) * 1.4, tintAmount);
    // ACES (Narkowicz).
    color = clamp((color * (2.51 * color + 0.03)) / (color * (2.43 * color + 0.59) + 0.14), 0.0, 1.0);
    return pow(color, vec3(1.0 / 2.2));
}
