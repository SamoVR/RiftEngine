#version 330 core
// Parts: materials, sun + sky ambient + point/spot lights, soft shadows, fog.
// Colours come in as sRGB, lighting is linear, ACES tone mapping, sRGB out.
in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUV;
in vec3 vLocalPos;
in vec3 vLocalNormal;
in vec4 vLightPos;
out vec4 FragColor;

// Material
uniform vec3 uColor;
uniform float uTransparency;
uniform int uMaterial;          // Plastic, SmoothPlastic, Metal, Neon, Glass, Wood, Concrete
uniform float uReflectance;
uniform sampler2D uTexture;
uniform int uHasTexture;
uniform vec2 uTextureScale;     // studs per repeat; 0 = stretch over each face
uniform vec3 uSize;             // part size in studs
uniform vec3 uTint;             // editor highlight
uniform float uTintAmount;

// Environment
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

// Texture coordinates in studs: each face is projected on its own plane, so
// textures tile the same on a 1-stud cube and on a 64-stud baseplate.
vec2 studUV() {
    vec3 p = vLocalPos * uSize;
    vec3 n = abs(vLocalNormal);
    vec2 uv;
    if (n.y >= n.x && n.y >= n.z) uv = p.xz;
    else if (n.x >= n.z) uv = p.zy;
    else uv = p.xy;
    return uv / max(uTextureScale, vec2(0.001));
}

float shadowFactor(vec3 normal) {
    if (uShadows == 0) return 1.0;
    vec3 p = vLightPos.xyz / vLightPos.w * 0.5 + 0.5;
    if (p.z > 1.0) return 1.0;
    float bias = max(0.0015 * (1.0 - dot(normal, uSunDirection)), 0.0004);
    float sum = 0.0;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            sum += texture(uShadowMap, vec3(p.xy + vec2(x, y) * uShadowTexel, p.z - bias));
    return sum / 9.0;
}

void main() {
    vec3 N = normalize(vNormal);
    if (!gl_FrontFacing) N = -N;
    vec3 V = normalize(uCameraPos - vWorldPos);

    vec3 albedo = toLinear(uColor);
    if (uHasTexture == 1) {
        vec2 uv = (uTextureScale.x > 0.0) ? studUV() : vUV;
        albedo *= toLinear(texture(uTexture, uv).rgb);
    }

    float roughness = 0.55;
    float specular = 0.2;
    float metallic = 0.0;
    float alpha = 1.0 - uTransparency;
    bool emissive = false;
    vec3 grainPos = vLocalPos * uSize;
    if (uMaterial == 1) { roughness = 0.22; specular = 0.45; }                      // SmoothPlastic
    else if (uMaterial == 2) { roughness = 0.32; specular = 0.9; metallic = 0.85; }  // Metal
    else if (uMaterial == 3) { emissive = true; }                                     // Neon
    else if (uMaterial == 4) { roughness = 0.05; specular = 1.0; alpha = min(alpha, 0.35); } // Glass
    else if (uMaterial == 5) {                                                          // Wood: rings along X
        float rings = fract(length(grainPos.yz * vec2(1.0, 0.35)) * 2.2 + noise(grainPos * vec3(0.4, 3.0, 3.0)) * 1.4);
        albedo *= mix(0.72, 1.08, smoothstep(0.0, 0.6, rings)) * mix(0.92, 1.05, noise(grainPos * vec3(0.2, 9.0, 9.0)));
        roughness = 0.6; specular = 0.12;
    } else if (uMaterial == 6) {                                                        // Concrete: speckles
        albedo *= 0.86 + 0.18 * noise(grainPos * 6.0) + 0.06 * noise(grainPos * 23.0);
        roughness = 0.9; specular = 0.05;
    }

    vec3 color;
    if (emissive) {
        color = albedo * 2.2 + albedo * 0.3 * pow(1.0 - max(dot(N, V), 0.0), 2.0);
    } else {
        vec3 diffuseColor = albedo * (1.0 - metallic);
        vec3 specColor = mix(vec3(specular), albedo, metallic);
        float shininess = mix(6.0, 512.0, pow(1.0 - roughness, 2.0));

        // Sky-tinted ambient: brighter from above, darker from below.
        float hemi = N.y * 0.5 + 0.5;
        vec3 ambient = uAmbient * mix(0.55, 1.0, hemi) + uSkyColor * 0.12 * hemi;
        vec3 light = ambient * diffuseColor;

        // Sun.
        float ndl = max(dot(N, uSunDirection), 0.0);
        if (ndl > 0.0) {
            float shadow = shadowFactor(N);
            vec3 H = normalize(uSunDirection + V);
            float spec = pow(max(dot(N, H), 0.0), shininess) * (shininess + 8.0) / 25.0;
            light += uSunColor * shadow * (diffuseColor * ndl + specColor * spec * ndl);
        }

        // Point and spot lights.
        for (int i = 0; i < uLightCount; ++i) {
            vec3 toLight = uLightPosRange[i].xyz - vWorldPos;
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

        // Reflections of the sky (Reflectance, metals, glass) with Fresnel.
        float fresnel = pow(1.0 - max(dot(N, V), 0.0), 5.0);
        float reflectAmount = clamp(uReflectance + metallic * 0.5 + (uMaterial == 4 ? 0.35 : 0.0) + fresnel * specular * 0.5, 0.0, 1.0);
        vec3 env = skyAt(reflect(-V, N)) * mix(vec3(1.0), albedo, metallic);
        color = mix(light, env, reflectAmount * (1.0 - roughness * 0.7));
        if (uMaterial == 4) alpha = clamp(alpha + fresnel * 0.5, 0.0, 1.0);
    }

    // Fog.
    float dist = length(uCameraPos - vWorldPos);
    float fog = clamp((dist - uFogStart) / max(uFogEnd - uFogStart, 0.001), 0.0, 1.0);
    color = mix(color, uFogColor, fog);

    color *= uExposure;
    color = mix(color, toLinear(uTint) * 1.4, uTintAmount);
    // ACES (Narkowicz) tone mapping, then sRGB.
    color = clamp((color * (2.51 * color + 0.03)) / (color * (2.43 * color + 0.59) + 0.14), 0.0, 1.0);
    FragColor = vec4(pow(color, vec3(1.0 / 2.2)), alpha);
}
